/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PISTORM_ARMTERM_SHM_H
#define _PISTORM_ARMTERM_SHM_H

#include <linux/types.h>

#define ARMTERM_SHM_MAGIC       "ARMT0008"
#define ARMTERM_SHM_VERSION     8U
#define ARMTERM_SHM_SIZE        0x00021000U
#define ARMTERM_RING_SIZE       0x00010000U

#define ARMTERM_F_DRIVER_UP     (1U << 0)
#define ARMTERM_F_TTY_OPEN      (1U << 1)
#define ARMTERM_F_GRAPHICAL     (1U << 2)

struct armterm_control {
	u8 magic[8];
	__le32 version;
	__le32 total_size;
	__le32 flags;
	__le32 in_head;       /* producer: Amiga, consumer: Linux */
	__le32 in_tail;
	__le32 out_head;      /* producer: Linux, consumer: Amiga */
	__le32 out_tail;
	__le32 heartbeat;
	__le32 generation;
	__le32 owner_cookie;  /* frontend lease; transport does not depend on it */
	__le32 owner_hb;
	__le32 rx_bytes;
	__le32 tx_bytes;
	__le32 rx_overruns;
	__le32 tx_full;
	__le32 opens;
	__le32 rows;          /* reserved for later TIOCSWINSZ bridge */
	__le32 cols;
	__le32 reserved[12];
	u8 info[128];
	u8 pad[4096 - 128 - 128];
} __packed;

struct armterm_shm {
	struct armterm_control ctl;
	u8 in_ring[ARMTERM_RING_SIZE];
	u8 out_ring[ARMTERM_RING_SIZE];
} __packed;

#endif
