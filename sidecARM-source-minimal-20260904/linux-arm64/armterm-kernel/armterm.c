// SPDX-License-Identifier: GPL-2.0
/*
 * armterm.c - PiStorm/Emu68 shared-RAM Linux TTY
 *
 * /dev/armterm is a normal Linux tty.  The transport is two lockless SPSC
 * rings in reserved WB/cacheable service RAM shared with AmigaOS.
 *
 * No PTY, no userspace relay, no shell supervision in this driver.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vt_kern.h>
#include <linux/console_struct.h>

#include "armterm_shm.h"

#define DRV_NAME              "armterm"
#define ARMTERM_POLL_MS       2
#define ARMTERM_OWNER_MS      2000
#define ARMTERM_BURST         512

struct armterm_dev {
	struct device *dev;
	struct armterm_shm *shm;
	struct tty_driver *tty_drv;
	struct tty_port port;
	struct timer_list poll_timer;
	u32 last_out_tail;
	u32 seen_owner;
	u32 seen_owner_hb;
	unsigned long owner_seen_jiffies;
};

static inline u32 at_read32(const __le32 *p)
{
	return le32_to_cpu(READ_ONCE(*p));
}

static inline void at_write32(__le32 *p, u32 v)
{
	WRITE_ONCE(*p, cpu_to_le32(v));
}

static inline u32 ring_used(u32 head, u32 tail)
{
	return head - tail;
}

static inline u32 ring_free(u32 head, u32 tail)
{
	u32 used = ring_used(head, tail);

	return used >= ARMTERM_RING_SIZE ? 0 : ARMTERM_RING_SIZE - used;
}

static void armterm_set_flag(struct armterm_dev *at, u32 bit, bool on)
{
	u32 flags = at_read32(&at->shm->ctl.flags);

	if (on)
		flags |= bit;
	else
		flags &= ~bit;
	at_write32(&at->shm->ctl.flags, flags);
}

static int armterm_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct armterm_dev *at = driver->driver_state;
	int ret;

	tty->driver_data = at;
	ret = tty_port_install(&at->port, driver, tty);
	return ret;
}

static int armterm_open(struct tty_struct *tty, struct file *filp)
{
	struct armterm_dev *at = tty->driver_data;
	int ret = tty_port_open(&at->port, tty, filp);

	if (!ret) {
		at_write32(&at->shm->ctl.opens, at_read32(&at->shm->ctl.opens) + 1);
		armterm_set_flag(at, ARMTERM_F_TTY_OPEN, true);
	}
	return ret;
}

static void armterm_close(struct tty_struct *tty, struct file *filp)
{
	struct armterm_dev *at = tty->driver_data;

	tty_port_close(&at->port, tty, filp);
	if (!READ_ONCE(at->port.count))
		armterm_set_flag(at, ARMTERM_F_TTY_OPEN, false);
}

static void armterm_cleanup(struct tty_struct *tty)
{
	/* tty_port_install() does not take an extra reference in this 1-port driver. */
}

static ssize_t armterm_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	struct armterm_dev *at = tty->driver_data;
	struct armterm_control *c = &at->shm->ctl;
	u32 head, tail, free;
	size_t n, first;

	if (!count)
		return 0;

	head = at_read32(&c->out_head);
	tail = at_read32(&c->out_tail);
	free = ring_free(head, tail);
	if (!free) {
		at_write32(&c->tx_full, at_read32(&c->tx_full) + 1);
		return 0;
	}

	n = min_t(size_t, count, free);
	first = min_t(size_t, n, ARMTERM_RING_SIZE - (head & (ARMTERM_RING_SIZE - 1)));
	memcpy(at->shm->out_ring + (head & (ARMTERM_RING_SIZE - 1)), buf, first);
	if (first < n)
		memcpy(at->shm->out_ring, buf + first, n - first);

	/* Publish bytes before head. */
	smp_wmb();
	at_write32(&c->out_head, head + n);
	at_write32(&c->tx_bytes, at_read32(&c->tx_bytes) + n);
	return n;
}

static unsigned int armterm_write_room(struct tty_struct *tty)
{
	struct armterm_dev *at = tty->driver_data;
	u32 head = at_read32(&at->shm->ctl.out_head);
	u32 tail = at_read32(&at->shm->ctl.out_tail);

	return ring_free(head, tail);
}

static unsigned int armterm_chars_in_buffer(struct tty_struct *tty)
{
	struct armterm_dev *at = tty->driver_data;
	u32 head = at_read32(&at->shm->ctl.out_head);
	u32 tail = at_read32(&at->shm->ctl.out_tail);
	u32 used = ring_used(head, tail);

	return min_t(u32, used, ARMTERM_RING_SIZE);
}

static const struct tty_port_operations armterm_port_ops = {
};

static const struct tty_operations armterm_tty_ops = {
	.install = armterm_install,
	.open = armterm_open,
	.close = armterm_close,
	.cleanup = armterm_cleanup,
	.write = armterm_write,
	.write_room = armterm_write_room,
	.chars_in_buffer = armterm_chars_in_buffer,
};

static void armterm_expire_owner(struct armterm_dev *at)
{
	struct armterm_control *c = &at->shm->ctl;
	u32 owner = at_read32(&c->owner_cookie);
	u32 hb = at_read32(&c->owner_hb);

	if (!owner) {
		at->seen_owner = 0;
		at->seen_owner_hb = 0;
		at->owner_seen_jiffies = jiffies;
		return;
	}

	if (owner != at->seen_owner || hb != at->seen_owner_hb) {
		at->seen_owner = owner;
		at->seen_owner_hb = hb;
		at->owner_seen_jiffies = jiffies;
		return;
	}

	if (time_after_eq(jiffies, at->owner_seen_jiffies + msecs_to_jiffies(ARMTERM_OWNER_MS))) {
		/* A crashed graphical frontend must never leave keyboard input
		 * routed away from /dev/armterm. */
		armterm_set_flag(at, ARMTERM_F_GRAPHICAL, false);
		at_write32(&c->owner_hb, 0);
		smp_wmb();
		at_write32(&c->owner_cookie, 0);
		at->seen_owner = 0;
		at->seen_owner_hb = 0;
		at->owner_seen_jiffies = jiffies;
	}
}

static void armterm_poll_timer(struct timer_list *t)
{
	struct armterm_dev *at = from_timer(at, t, poll_timer);
	struct armterm_control *c = &at->shm->ctl;
	struct tty_struct *tty = NULL;
	struct tty_port *dst_port;
	struct vc_data *vc = NULL;
	u8 buf[ARMTERM_BURST];
	u32 head, tail, used, n, first;
	u32 out_tail, flags;
	int copied;
	bool graphical;

	at_write32(&c->heartbeat, at_read32(&c->heartbeat) + 1);
	armterm_expire_owner(at);

	/* Wake Linux writers when Amiga has consumed output. */
	out_tail = at_read32(&c->out_tail);
	if (out_tail != at->last_out_tail) {
		at->last_out_tail = out_tail;
		tty_port_tty_wakeup(&at->port);
	}

	flags = at_read32(&c->flags);
	graphical = !!(flags & ARMTERM_F_GRAPHICAL);

	/* Normal mode preserves the existing /dev/armterm behaviour exactly.
	 * Graphical mode feeds the same already-translated ASCII/ANSI bytes to
	 * tty1's native flip buffer, i.e. the same input side used by the VT.
	 * vc_cons[0] is tty1 (console tty index 0).
	 */
	if (graphical) {
		vc = READ_ONCE(vc_cons[0].d);
		if (!vc)
			goto out;
		dst_port = &vc->port;
	} else {
		tty = tty_port_tty_get(&at->port);
		if (!tty)
			goto out;
		dst_port = &at->port;
	}

	head = at_read32(&c->in_head);
	tail = at_read32(&c->in_tail);
	/* Head publication by Amiga precedes ring data. */
	smp_rmb();
	used = ring_used(head, tail);
	if (unlikely(used > ARMTERM_RING_SIZE)) {
		/* Producer got out of sync: discard corrupt pending input, recover. */
		at_write32(&c->rx_overruns, at_read32(&c->rx_overruns) + 1);
		at_write32(&c->in_tail, head);
		goto put_tty;
	}
	if (!used)
		goto put_tty;

	n = min_t(u32, used, ARMTERM_BURST);
	first = min_t(u32, n, ARMTERM_RING_SIZE - (tail & (ARMTERM_RING_SIZE - 1)));
	memcpy(buf, at->shm->in_ring + (tail & (ARMTERM_RING_SIZE - 1)), first);
	if (first < n)
		memcpy(buf + first, at->shm->in_ring, n - first);

	copied = tty_insert_flip_string(dst_port, buf, n);
	if (copied > 0) {
		at_write32(&c->in_tail, tail + copied);
		at_write32(&c->rx_bytes, at_read32(&c->rx_bytes) + copied);
		tty_flip_buffer_push(dst_port);
	}

put_tty:
	if (tty)
		tty_kref_put(tty);
out:
	mod_timer(&at->poll_timer,
		  jiffies + max_t(unsigned long, 1, msecs_to_jiffies(ARMTERM_POLL_MS)));
}

static void armterm_init_shared(struct armterm_dev *at)
{
	struct armterm_control *c = &at->shm->ctl;
	u32 gen = 1;

	if (!memcmp(c->magic, ARMTERM_SHM_MAGIC, 8) &&
	    at_read32(&c->version) == ARMTERM_SHM_VERSION)
		gen = at_read32(&c->generation) + 1;
	if (!gen)
		gen = 1;

	memset(at->shm, 0, sizeof(*at->shm));
	memcpy(c->magic, ARMTERM_SHM_MAGIC, 8);
	at_write32(&c->version, ARMTERM_SHM_VERSION);
	at_write32(&c->total_size, sizeof(*at->shm));
	at_write32(&c->generation, gen);
	strscpy(c->info, "kernel tty /dev/armterm", sizeof(c->info));
	smp_wmb();
	at_write32(&c->flags, ARMTERM_F_DRIVER_UP);
}

static int armterm_probe(struct platform_device *pdev)
{
	struct armterm_dev *at;
	struct resource *res;
	resource_size_t size;
	void *base;
	struct device *tty_dev;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	size = resource_size(res);
	if (size < sizeof(struct armterm_shm))
		return dev_err_probe(&pdev->dev, -EINVAL,
			"shared region too small: %pa, need %zu\n", &size, sizeof(struct armterm_shm));

	base = devm_memremap(&pdev->dev, res->start, size, MEMREMAP_WB);
	if (IS_ERR(base))
		return PTR_ERR(base);

	at = devm_kzalloc(&pdev->dev, sizeof(*at), GFP_KERNEL);
	if (!at)
		return -ENOMEM;
	at->dev = &pdev->dev;
	at->shm = base;
	at->owner_seen_jiffies = jiffies;

	tty_port_init(&at->port);
	at->port.ops = &armterm_port_ops;

	at->tty_drv = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW |
					TTY_DRIVER_DYNAMIC_DEV |
					TTY_DRIVER_UNNUMBERED_NODE);
	if (IS_ERR(at->tty_drv)) {
		ret = PTR_ERR(at->tty_drv);
		goto err_port;
	}

	at->tty_drv->driver_name = DRV_NAME;
	at->tty_drv->name = "armterm";
	at->tty_drv->major = 0;
	at->tty_drv->minor_start = 0;
	at->tty_drv->type = TTY_DRIVER_TYPE_SERIAL;
	at->tty_drv->subtype = SERIAL_TYPE_NORMAL;
	at->tty_drv->init_termios = tty_std_termios;
	at->tty_drv->init_termios.c_cflag = B115200 | CS8 | CREAD | CLOCAL;
	at->tty_drv->driver_state = at;
	tty_set_operations(at->tty_drv, &armterm_tty_ops);

	ret = tty_register_driver(at->tty_drv);
	if (ret)
		goto err_driver;

	tty_dev = tty_port_register_device(&at->port, at->tty_drv, 0, &pdev->dev);
	if (IS_ERR(tty_dev)) {
		ret = PTR_ERR(tty_dev);
		goto err_unregister;
	}

	platform_set_drvdata(pdev, at);
	armterm_init_shared(at);
	at->last_out_tail = at_read32(&at->shm->ctl.out_tail);
	timer_setup(&at->poll_timer, armterm_poll_timer, 0);
	mod_timer(&at->poll_timer, jiffies + 1);

	dev_info(&pdev->dev, "registered /dev/armterm shared phys=%pa size=%pa ABI=%u\n",
		 &res->start, &size, ARMTERM_SHM_VERSION);
	return 0;

err_unregister:
	tty_unregister_driver(at->tty_drv);
err_driver:
	tty_driver_kref_put(at->tty_drv);
err_port:
	tty_port_destroy(&at->port);
	return ret;
}

static void armterm_remove(struct platform_device *pdev)
{
	struct armterm_dev *at = platform_get_drvdata(pdev);

	if (!at)
		return;
	del_timer_sync(&at->poll_timer);
	at_write32(&at->shm->ctl.flags, 0);
	tty_unregister_device(at->tty_drv, 0);
	tty_unregister_driver(at->tty_drv);
	tty_driver_kref_put(at->tty_drv);
	tty_port_destroy(&at->port);
}

static const struct of_device_id armterm_of_match[] = {
	{ .compatible = "pistorm,armterm" },
	{ }
};
MODULE_DEVICE_TABLE(of, armterm_of_match);

static struct platform_driver armterm_driver = {
	.probe = armterm_probe,
	.remove = armterm_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = armterm_of_match,
	},
};
module_platform_driver(armterm_driver);

MODULE_DESCRIPTION("PiStorm/Emu68 shared-RAM TTY");
MODULE_LICENSE("GPL");
