// SPDX-License-Identifier: GPL-2.0
/*
 * armblk.c - PiStorm/Emu68 shared-RAM block device
 *
 * POC1:
 *   - exposes /dev/armblk0
 *   - blk-mq, one hardware queue, 8 tags / 8 shared slots
 *   - up to 128 KiB per request
 *   - GET_SIZE handshake during probe
 *   - READ / WRITE / FLUSH
 *   - 1 ms completion polling, mirroring the proven armnet POC style
 *   - shared RAM is normal WB/cacheable coherent RAM, not MMIO
 *
 * The AmigaOS backend owns the file (for example Work:linux.img), keeps it
 * open, initializes the shared header, and services slots whose state changes
 * from FREE to REQ.  It completes a slot by writing status/result/data and
 * finally publishing state=DONE after a write barrier.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/memremap.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/highmem.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include "armblk_shm.h"

#define DRV_NAME             "armblk"
#define ARMBLK_DISK_NAME     "armblk0"
#define ARMBLK_POLL_MS        1
#define ARMBLK_PROBE_MS       2000
#define ARMBLK_SECTOR_SIZE    512U

struct armblk_dev {
	struct device *dev;
	struct armblk_shm *shm;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
	struct timer_list poll_timer;
	spinlock_t lock;
	struct request *pending[ARMBLK_QUEUE_DEPTH];
	u64 capacity_bytes;
	int major;
};

static inline u32 armblk_read32(const __be32 *p)
{
	return be32_to_cpu(READ_ONCE(*p));
}

static inline void armblk_write32(__be32 *p, u32 v)
{
	WRITE_ONCE(*p, cpu_to_be32(v));
}

static inline u64 armblk_read64(const __be64 *p)
{
	return be64_to_cpu(READ_ONCE(*p));
}

static inline void armblk_write64(__be64 *p, u64 v)
{
	WRITE_ONCE(*p, cpu_to_be64(v));
}

static int armblk_copy_rq_to_buf(struct request *rq, u8 *dst,
				 unsigned int expected)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	unsigned int done = 0;

	rq_for_each_segment(bvec, rq, iter) {
		void *p;

		if (unlikely(done + bvec.bv_len > expected))
			return -EIO;

		p = bvec_kmap_local(&bvec);
		memcpy(dst + done, p, bvec.bv_len);
		kunmap_local(p);
		done += bvec.bv_len;
	}

	return done == expected ? 0 : -EIO;
}

static int armblk_copy_buf_to_rq(struct request *rq, const u8 *src,
				 unsigned int expected)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	unsigned int done = 0;

	rq_for_each_segment(bvec, rq, iter) {
		void *p;

		if (unlikely(done + bvec.bv_len > expected))
			return -EIO;

		p = bvec_kmap_local(&bvec);
		memcpy(p, src + done, bvec.bv_len);
		kunmap_local(p);
		done += bvec.bv_len;
	}

	return done == expected ? 0 : -EIO;
}

static blk_status_t armblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				    const struct blk_mq_queue_data *bd)
{
	struct armblk_dev *ab = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct armblk_slot *s;
	unsigned long irqflags;
	unsigned int tag = rq->tag;
	unsigned int bytes = blk_rq_bytes(rq);
	u32 op;
	int ret;

	if (unlikely(tag >= ARMBLK_QUEUE_DEPTH))
		return BLK_STS_IOERR;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		op = ARMBLK_OP_READ;
		break;
	case REQ_OP_WRITE:
		op = ARMBLK_OP_WRITE;
		break;
	case REQ_OP_FLUSH:
		op = ARMBLK_OP_FLUSH;
		bytes = 0;
		break;
	default:
		return BLK_STS_NOTSUPP;
	}

	if (unlikely(bytes > ARMBLK_MAX_BYTES))
		return BLK_STS_IOERR;

	s = &ab->shm->slot[tag];

	/* A tag maps 1:1 to a transport slot. */
	if (unlikely(armblk_read32(&s->state) != ARMBLK_SLOT_FREE))
		return BLK_STS_RESOURCE;

	if (op == ARMBLK_OP_WRITE && bytes) {
		ret = armblk_copy_rq_to_buf(rq, s->data, bytes);
		if (unlikely(ret))
			return BLK_STS_IOERR;
	}

	blk_mq_start_request(rq);

	armblk_write32(&s->op, op);
	armblk_write32(&s->id, tag);
	armblk_write32(&s->status, 0);
	armblk_write64(&s->offset, op == ARMBLK_OP_FLUSH ? 0 :
		      ((u64)blk_rq_pos(rq) << 9));
	armblk_write32(&s->length, bytes);
	armblk_write64(&s->result, 0);

	spin_lock_irqsave(&ab->lock, irqflags);
	WRITE_ONCE(ab->pending[tag], rq);
	spin_unlock_irqrestore(&ab->lock, irqflags);

	/* Publish descriptor/data before publishing REQ. */
	smp_wmb();
	armblk_write32(&s->state, ARMBLK_SLOT_REQ);

	return BLK_STS_OK;
}

static const struct blk_mq_ops armblk_mq_ops = {
	.queue_rq = armblk_queue_rq,
};

static void armblk_complete_slot(struct armblk_dev *ab, unsigned int tag)
{
	struct armblk_slot *s = &ab->shm->slot[tag];
	struct request *rq;
	unsigned long irqflags;
	blk_status_t bst = BLK_STS_OK;
	u32 status, op, len;

	if (armblk_read32(&s->state) != ARMBLK_SLOT_DONE)
		return;

	/* Backend publishes status/result/data before DONE. */
	smp_rmb();

	spin_lock_irqsave(&ab->lock, irqflags);
	rq = READ_ONCE(ab->pending[tag]);
	if (rq)
		WRITE_ONCE(ab->pending[tag], NULL);
	spin_unlock_irqrestore(&ab->lock, irqflags);

	if (unlikely(!rq))
		return;

	status = armblk_read32(&s->status);
	op = armblk_read32(&s->op);
	len = armblk_read32(&s->length);

	if (status) {
		bst = BLK_STS_IOERR;
	} else if (op == ARMBLK_OP_READ) {
		if (unlikely(len != blk_rq_bytes(rq) ||
		    armblk_copy_buf_to_rq(rq, s->data, len)))
			bst = BLK_STS_IOERR;
	}

	/* Slot can be reused only after Linux consumed READ data. */
	smp_wmb();
	armblk_write32(&s->state, ARMBLK_SLOT_FREE);

	blk_mq_end_request(rq, bst);
}

static void armblk_poll_timer(struct timer_list *t)
{
	struct armblk_dev *ab = from_timer(ab, t, poll_timer);
	unsigned int i;

	for (i = 0; i < ARMBLK_QUEUE_DEPTH; i++)
		armblk_complete_slot(ab, i);

	mod_timer(&ab->poll_timer,
		  jiffies + max_t(unsigned long, 1,
				  msecs_to_jiffies(ARMBLK_POLL_MS)));
}

/*
 * Small synchronous transport transaction used only before /dev/armblk0 is
 * registered.  Slot 0 is safe here because blk-mq is not active yet.
 */
static int armblk_get_size(struct armblk_dev *ab, u64 *bytes)
{
	struct armblk_slot *s = &ab->shm->slot[0];
	unsigned long deadline;

	if (armblk_read32(&s->state) != ARMBLK_SLOT_FREE)
		return -EBUSY;

	armblk_write32(&s->op, ARMBLK_OP_GET_SIZE);
	armblk_write32(&s->id, 0);
	armblk_write32(&s->status, 0);
	armblk_write64(&s->offset, 0);
	armblk_write32(&s->length, 0);
	armblk_write64(&s->result, 0);
	smp_wmb();
	armblk_write32(&s->state, ARMBLK_SLOT_REQ);

	deadline = jiffies + msecs_to_jiffies(ARMBLK_PROBE_MS);
	for (;;) {
		if (armblk_read32(&s->state) == ARMBLK_SLOT_DONE)
			break;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		msleep(1);
	}

	smp_rmb();
	if (armblk_read32(&s->status)) {
		armblk_write32(&s->state, ARMBLK_SLOT_FREE);
		return -EIO;
	}

	*bytes = armblk_read64(&s->result);
	smp_wmb();
	armblk_write32(&s->state, ARMBLK_SLOT_FREE);

	if (!*bytes || (*bytes & (ARMBLK_SECTOR_SIZE - 1)))
		return -EINVAL;

	return 0;
}

static const struct block_device_operations armblk_fops = {
	.owner = THIS_MODULE,
};

static int armblk_probe(struct platform_device *pdev)
{
	struct armblk_dev *ab;
	struct resource *res;
	struct queue_limits lim = {
		.logical_block_size = ARMBLK_SECTOR_SIZE,
		.physical_block_size = ARMBLK_SECTOR_SIZE,
		.io_min = ARMBLK_SECTOR_SIZE,
		.max_hw_sectors = ARMBLK_MAX_BYTES >> 9,
	};
	resource_size_t size;
	void *base;
	u32 flags;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	size = resource_size(res);
	if (size < sizeof(struct armblk_shm)) {
		dev_err(&pdev->dev,
			"shared region too small: %pa bytes, need %zu\n",
			&size, sizeof(struct armblk_shm));
		return -EINVAL;
	}

	/* Same memory model as armnet: reserved normal WB/cacheable shared RAM. */
	base = devm_memremap(&pdev->dev, res->start, size, MEMREMAP_WB);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ab = devm_kzalloc(&pdev->dev, sizeof(*ab), GFP_KERNEL);
	if (!ab)
		return -ENOMEM;

	ab->dev = &pdev->dev;
	ab->shm = base;
	spin_lock_init(&ab->lock);

	if (armblk_read32(&ab->shm->magic) != ARMBLK_SHM_MAGIC ||
	    armblk_read32(&ab->shm->version) != ARMBLK_SHM_VERSION) {
		dev_err(&pdev->dev,
			"backend ABI not ready (magic=%08x version=%u)\n",
			armblk_read32(&ab->shm->magic),
			armblk_read32(&ab->shm->version));
		return -ENODEV;
	}

	if (armblk_read32(&ab->shm->queue_depth) != ARMBLK_QUEUE_DEPTH ||
	    armblk_read32(&ab->shm->max_bytes) != ARMBLK_MAX_BYTES) {
		dev_err(&pdev->dev, "backend queue geometry mismatch\n");
		return -EINVAL;
	}

	flags = armblk_read32(&ab->shm->flags);
	if (!(flags & ARMBLK_F_BACKEND_UP)) {
		dev_err(&pdev->dev, "Amiga backend is not up\n");
		return -ENODEV;
	}

	ret = armblk_get_size(ab, &ab->capacity_bytes);
	if (ret) {
		dev_err(&pdev->dev, "GET_SIZE failed: %d\n", ret);
		return ret;
	}

	ab->tag_set.ops = &armblk_mq_ops;
	ab->tag_set.nr_hw_queues = 1;
	ab->tag_set.queue_depth = ARMBLK_QUEUE_DEPTH;
	ab->tag_set.numa_node = NUMA_NO_NODE;
	ab->tag_set.flags = BLK_MQ_F_NO_SCHED_BY_DEFAULT;
	ab->tag_set.driver_data = ab;

	ret = blk_mq_alloc_tag_set(&ab->tag_set);
	if (ret)
		return ret;

	ab->disk = blk_mq_alloc_disk(&ab->tag_set, &lim, ab);
	if (IS_ERR(ab->disk)) {
		ret = PTR_ERR(ab->disk);
		ab->disk = NULL;
		goto err_tags;
	}

	ab->major = register_blkdev(0, DRV_NAME);
	if (ab->major < 0) {
		ret = ab->major;
		goto err_disk;
	}

	ab->disk->major = ab->major;
	ab->disk->first_minor = 0;
	ab->disk->minors = 1;
	ab->disk->fops = &armblk_fops;
	ab->disk->private_data = ab;
	strscpy(ab->disk->disk_name, ARMBLK_DISK_NAME, DISK_NAME_LEN);
	set_capacity(ab->disk, ab->capacity_bytes >> 9);

	timer_setup(&ab->poll_timer, armblk_poll_timer, 0);

	flags = armblk_read32(&ab->shm->flags) | ARMBLK_F_LINUX_UP;
	armblk_write32(&ab->shm->flags, flags);
	smp_wmb();

	/*
	 * The completion engine must be running before add_disk().
	 * add_disk() may trigger an immediate partition scan, which can issue
	 * READ requests before it returns.  Those requests need the poll timer
	 * alive so DONE slots are consumed and blk_mq_end_request() is called.
	 */
	mod_timer(&ab->poll_timer, jiffies + 1);

	ret = add_disk(ab->disk);
	if (ret)
		goto err_major;

	platform_set_drvdata(pdev, ab);

	dev_info(&pdev->dev,
		 "registered /dev/%s phys=%pa size=%pa capacity=%llu bytes ABI=%u\n",
		 ab->disk->disk_name, &res->start, &size,
		 (unsigned long long)ab->capacity_bytes, ARMBLK_SHM_VERSION);
	return 0;

err_major:
	del_timer_sync(&ab->poll_timer);
	flags = armblk_read32(&ab->shm->flags) & ~ARMBLK_F_LINUX_UP;
	armblk_write32(&ab->shm->flags, flags);
	unregister_blkdev(ab->major, DRV_NAME);
err_disk:
	put_disk(ab->disk);
err_tags:
	blk_mq_free_tag_set(&ab->tag_set);
	return ret;
}

static void armblk_remove(struct platform_device *pdev)
{
	struct armblk_dev *ab = platform_get_drvdata(pdev);
	u32 flags;

	if (!ab)
		return;

	del_timer_sync(&ab->poll_timer);

	flags = armblk_read32(&ab->shm->flags) & ~ARMBLK_F_LINUX_UP;
	armblk_write32(&ab->shm->flags, flags);
	smp_wmb();

	del_gendisk(ab->disk);
	put_disk(ab->disk);
	unregister_blkdev(ab->major, DRV_NAME);
	blk_mq_free_tag_set(&ab->tag_set);
}

static const struct of_device_id armblk_of_match[] = {
	{ .compatible = "pistorm,armblk" },
	{ }
};
MODULE_DEVICE_TABLE(of, armblk_of_match);

static struct platform_driver armblk_driver = {
	.probe = armblk_probe,
	.remove = armblk_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = armblk_of_match,
	},
};
module_platform_driver(armblk_driver);

MODULE_DESCRIPTION("PiStorm/Emu68 ARM-Amiga shared-RAM block device");
MODULE_LICENSE("GPL");
