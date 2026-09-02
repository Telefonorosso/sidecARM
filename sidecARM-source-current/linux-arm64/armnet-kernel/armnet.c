// SPDX-License-Identifier: GPL-2.0
/*
 * armnet.c - PiStorm/Emu68 shared-RAM Ethernet interface
 *
 * POC2-realmap:
 *   - normal Linux Ethernet interface: arm0
 *   - no TAP and no userspace helper
 *   - shared RAM is normal cacheable coherent RAM, not MMIO
 *   - two fixed-size SPSC rings
 *   - RX polling at 1 ms for the first end-to-end milestone
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/if_ether.h>
#include <linux/memremap.h>

#include "armnet_shm.h"

#define DRV_NAME "armnet"
#define ARMNET_POLL_MS 1

struct armnet_priv {
	struct net_device *ndev;
	struct device *dev;
	struct armnet_shm *shm;
	struct timer_list rx_timer;
};

static inline u32 armnet_read32(const __be32 *p)
{
	return be32_to_cpu(READ_ONCE(*p));
}

static inline void armnet_write32(__be32 *p, u32 v)
{
	WRITE_ONCE(*p, cpu_to_be32(v));
}

static inline u16 armnet_read16(const __be16 *p)
{
	return be16_to_cpu(READ_ONCE(*p));
}

static inline void armnet_write16(__be16 *p, u16 v)
{
	WRITE_ONCE(*p, cpu_to_be16(v));
}

/*
 * Ring ordering:
 * producer: write payload+len, smp_wmb(), publish prod
 * consumer: read prod, smp_rmb(), consume payload, publish cons
 *
 * The physical backing is cacheable Inner Shareable RAM on the Pi.  The
 * Amiga side must use matching barriers around prod/cons publication.
 */
static netdev_tx_t armnet_start_xmit(struct sk_buff *skb,
				    struct net_device *ndev)
{
	struct armnet_priv *priv = netdev_priv(ndev);
	struct armnet_ring *r = &priv->shm->l2a;
	struct armnet_slot *s;
	u32 prod, cons, next;
	unsigned int len;

	if (unlikely(skb->len > ARMNET_SLOT_DATA)) {
		ndev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	prod = armnet_read32(&r->prod);
	cons = armnet_read32(&r->cons);
	next = (prod + 1) % ARMNET_RING_SLOTS;

	if (unlikely(next == cons)) {
		netif_stop_queue(ndev);
		smp_mb();
		cons = armnet_read32(&r->cons);
		if (next != cons)
			netif_wake_queue(ndev);
		else
			return NETDEV_TX_BUSY;
	}

	s = &r->slot[prod];
	len = skb->len;

	memcpy(s->data, skb->data, len);
	armnet_write16(&s->len, len);
	smp_wmb();
	armnet_write32(&r->prod, next);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	dev_kfree_skb(skb);

	cons = armnet_read32(&r->cons);
	if (((next + 1) % ARMNET_RING_SLOTS) == cons)
		netif_stop_queue(ndev);

	return NETDEV_TX_OK;
}

static int armnet_rx_one(struct armnet_priv *priv)
{
	struct net_device *ndev = priv->ndev;
	struct armnet_ring *r = &priv->shm->a2l;
	struct armnet_slot *s;
	struct sk_buff *skb;
	u32 prod, cons, next;
	u16 len;

	cons = armnet_read32(&r->cons);
	prod = armnet_read32(&r->prod);

	if (cons == prod)
		return 0;

	smp_rmb();
	s = &r->slot[cons];
	len = armnet_read16(&s->len);

	if (unlikely(len < ETH_HLEN || len > ARMNET_SLOT_DATA)) {
		ndev->stats.rx_errors++;
		ndev->stats.rx_length_errors++;
		goto consume;
	}

	skb = netdev_alloc_skb_ip_align(ndev, len);
	if (unlikely(!skb)) {
		ndev->stats.rx_dropped++;
		return -ENOMEM; /* leave slot pending: natural back-pressure */
	}

	memcpy(skb_put(skb, len), s->data, len);
	skb->protocol = eth_type_trans(skb, ndev);
	skb->ip_summed = CHECKSUM_NONE;

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;
	netif_rx(skb);

consume:
	next = (cons + 1) % ARMNET_RING_SLOTS;
	smp_wmb();
	armnet_write32(&r->cons, next);
	return 1;
}

static void armnet_poll_timer(struct timer_list *t)
{
	struct armnet_priv *priv = from_timer(priv, t, rx_timer);
	struct net_device *ndev = priv->ndev;
	int budget = ARMNET_RING_SLOTS;

	if (netif_running(ndev)) {
		while (budget-- > 0) {
			int ret = armnet_rx_one(priv);
			if (ret <= 0)
				break;
		}

		if (netif_queue_stopped(ndev)) {
			struct armnet_ring *r = &priv->shm->l2a;
			u32 prod = armnet_read32(&r->prod);
			u32 cons = armnet_read32(&r->cons);

			if (((prod + 1) % ARMNET_RING_SLOTS) != cons)
				netif_wake_queue(ndev);
		}
	}

	mod_timer(&priv->rx_timer,
		  jiffies + max_t(unsigned long, 1,
				  msecs_to_jiffies(ARMNET_POLL_MS)));
}

static int armnet_open(struct net_device *ndev)
{
	struct armnet_priv *priv = netdev_priv(ndev);
	u32 flags = armnet_read32(&priv->shm->flags);

	flags |= ARMNET_F_LINUX_UP;
	armnet_write32(&priv->shm->flags, flags);
	smp_wmb();

	netif_start_queue(ndev);
	mod_timer(&priv->rx_timer, jiffies + 1);
	return 0;
}

static int armnet_stop(struct net_device *ndev)
{
	struct armnet_priv *priv = netdev_priv(ndev);
	u32 flags;

	netif_stop_queue(ndev);
	del_timer_sync(&priv->rx_timer);

	flags = armnet_read32(&priv->shm->flags);
	flags &= ~ARMNET_F_LINUX_UP;
	armnet_write32(&priv->shm->flags, flags);
	smp_wmb();
	return 0;
}

static const struct net_device_ops armnet_netdev_ops = {
	.ndo_open       = armnet_open,
	.ndo_stop       = armnet_stop,
	.ndo_start_xmit = armnet_start_xmit,
};

static void armnet_init_shared(struct armnet_priv *priv)
{
	struct armnet_shm *shm = priv->shm;
	static const u8 linux_mac[ETH_ALEN] =
		{ 0x02, 0x68, 0x00, 0x00, 0x00, 0x02 };
	static const u8 amiga_mac[ETH_ALEN] =
		{ 0x02, 0x68, 0x00, 0x00, 0x00, 0x01 };

	if (armnet_read32(&shm->magic) == ARMNET_SHM_MAGIC &&
	    armnet_read32(&shm->version) == ARMNET_SHM_VERSION)
		return;

	memset(shm, 0, sizeof(*shm));
	memcpy(shm->linux_mac, linux_mac, ETH_ALEN);
	memcpy(shm->amiga_mac, amiga_mac, ETH_ALEN);
	armnet_write32(&shm->total_size, sizeof(*shm));
	armnet_write32(&shm->version, ARMNET_SHM_VERSION);
	smp_wmb();
	armnet_write32(&shm->magic, ARMNET_SHM_MAGIC);
	smp_wmb();
}

static int armnet_probe(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct armnet_priv *priv;
	struct resource *res;
	void *base;
	resource_size_t size;
	int ret;
	static const u8 linux_mac[ETH_ALEN] =
		{ 0x02, 0x68, 0x00, 0x00, 0x00, 0x02 };

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	size = resource_size(res);
	if (size < sizeof(struct armnet_shm)) {
		dev_err(&pdev->dev,
			"shared region too small: %pa bytes, need %zu\n",
			&size, sizeof(struct armnet_shm));
		return -EINVAL;
	}

	/*
	 * This is reserved NORMAL RAM shared with Emu68, not a peripheral.
	 * Keep the Linux alias WB/cacheable to match Emu68's Inner-Shareable
	 * cacheable service-RAM alias.
	 */
	base = devm_memremap(&pdev->dev, res->start, size, MEMREMAP_WB);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ndev = alloc_etherdev(sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &pdev->dev);
	priv = netdev_priv(ndev);
	priv->ndev = ndev;
	priv->dev = &pdev->dev;
	priv->shm = base;

	timer_setup(&priv->rx_timer, armnet_poll_timer, 0);

	ndev->netdev_ops = &armnet_netdev_ops;
	ndev->mtu = ARMNET_ETH_MTU;
	ndev->min_mtu = 68;
	ndev->max_mtu = ARMNET_ETH_MTU;
	eth_hw_addr_set(ndev, linux_mac);
	netif_carrier_on(ndev);
	strscpy(ndev->name, "arm0", IFNAMSIZ);

	armnet_init_shared(priv);

	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		return ret;
	}

	platform_set_drvdata(pdev, ndev);
	dev_info(&pdev->dev,
		 "registered %s shared phys=%pa size=%pa ABI=%u\n",
		 ndev->name, &res->start, &size, ARMNET_SHM_VERSION);
	return 0;
}

static void armnet_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct armnet_priv *priv;

	if (!ndev)
		return;
	priv = netdev_priv(ndev);
	del_timer_sync(&priv->rx_timer);
	unregister_netdev(ndev);
	free_netdev(ndev);
}

static const struct of_device_id armnet_of_match[] = {
	{ .compatible = "pistorm,armnet" },
	{ }
};
MODULE_DEVICE_TABLE(of, armnet_of_match);

static struct platform_driver armnet_driver = {
	.probe  = armnet_probe,
	.remove = armnet_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = armnet_of_match,
	},
};
module_platform_driver(armnet_driver);

MODULE_DESCRIPTION("PiStorm/Emu68 ARM-Amiga shared-RAM Ethernet");
MODULE_LICENSE("GPL");
