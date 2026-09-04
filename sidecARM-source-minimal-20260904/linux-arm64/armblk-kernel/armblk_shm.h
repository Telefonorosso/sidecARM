/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARMBLK_SHM_H
#define _ARMBLK_SHM_H

#include <linux/types.h>

#define ARMBLK_SHM_MAGIC          0x41524d42U /* "ARMB" */
#define ARMBLK_SHM_VERSION        1

#define ARMBLK_QUEUE_DEPTH        8
#define ARMBLK_MAX_BYTES          (128U * 1024U)

#define ARMBLK_F_BACKEND_UP       (1U << 0)
#define ARMBLK_F_LINUX_UP         (1U << 1)

#define ARMBLK_SLOT_FREE          0
#define ARMBLK_SLOT_REQ           1
#define ARMBLK_SLOT_DONE          2

#define ARMBLK_OP_GET_SIZE        1
#define ARMBLK_OP_READ            2
#define ARMBLK_OP_WRITE           3
#define ARMBLK_OP_FLUSH           4
#define ARMBLK_OP_RESET           5

/*
 * All control fields are big-endian because the peer is 68k AmigaOS.
 * The data[] payload is raw bytes and is never byte-swapped.
 */
struct armblk_slot {
	__be32 state;
	__be32 op;
	__be32 id;
	__be32 status;
	__be64 offset;
	__be32 length;
	__be32 reserved0;
	__be64 result;
	u8 data[ARMBLK_MAX_BYTES];
};

struct armblk_shm {
	__be32 magic;
	__be32 version;
	__be32 total_size;
	__be32 flags;
	__be32 queue_depth;
	__be32 max_bytes;
	__be32 reserved0;
	__be32 reserved1;
	struct armblk_slot slot[ARMBLK_QUEUE_DEPTH];
};

#endif
