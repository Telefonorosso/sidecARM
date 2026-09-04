#ifndef ARMNET_SHM_H
#define ARMNET_SHM_H
#include <exec/types.h>

#define ARMNET_SHM_MAGIC       0x41524d4eUL
#define ARMNET_SHM_VERSION     1UL
#define ARMNET_RING_SLOTS      64UL
#define ARMNET_RING_MASK       63UL
#define ARMNET_SLOT_DATA       2048UL
#define ARMNET_ETH_MTU         1500UL
#define ARMNET_F_LINUX_UP      (1UL << 0)
#define ARMNET_F_AMIGA_UP      (1UL << 1)

/* ABI integers are big endian. 68k accesses them natively. */
struct armnet_slot {
    UWORD len;
    UWORD reserved;
    UBYTE data[ARMNET_SLOT_DATA];
};
struct armnet_ring {
    ULONG prod;
    ULONG cons;
    struct armnet_slot slot[ARMNET_RING_SLOTS];
};
struct armnet_shm {
    ULONG magic;
    ULONG version;
    ULONG total_size;
    ULONG flags;
    UBYTE linux_mac[6];
    UBYTE amiga_mac[6];
    ULONG reserved0;
    struct armnet_ring l2a; /* Linux -> Amiga */
    struct armnet_ring a2l; /* Amiga -> Linux */
};
#endif
