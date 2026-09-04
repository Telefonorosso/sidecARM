/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARMNET_SHM_H
#define _ARMNET_SHM_H

#include <linux/types.h>

#define ARMNET_SHM_MAGIC       0x41524d4eU  /* "ARMN" */
#define ARMNET_SHM_VERSION     1

#define ARMNET_RING_SLOTS      64
#define ARMNET_SLOT_DATA       2048
#define ARMNET_ETH_MTU         1500

#define ARMNET_F_LINUX_UP      (1U << 0)
#define ARMNET_F_AMIGA_UP      (1U << 1)

struct armnet_slot {
	__be16 len;
	__be16 reserved;
	u8 data[ARMNET_SLOT_DATA];
};

struct armnet_ring {
	__be32 prod;
	__be32 cons;
	struct armnet_slot slot[ARMNET_RING_SLOTS];
};

struct armnet_shm {
	__be32 magic;
	__be32 version;
	__be32 total_size;
	__be32 flags;

	u8 linux_mac[6];
	u8 amiga_mac[6];
	__be32 reserved0;

	/* Producer Linux, consumer AmigaOS. */
	struct armnet_ring l2a;

	/* Producer AmigaOS, consumer Linux. */
	struct armnet_ring a2l;
};

#endif
