/*
 * PiStorm ARM service aperture - experimental POC2c.
 *
 * Zorro III non-MEMLIST aperture backed by ARM-reserved RAM.
 *
 * W^X design:
 *   - AmigaOS sees a writable ZIII aperture.
 *   - CPU1 executes through a second read-only alias of the same physical RAM.
 *
 * This mirrors the existing Emu68 JIT mapping model instead of creating an
 * RWX region.
 */

#include <boards.h>
#include <mmu.h>
#include <support.h>
#include <stdint.h>

#define ARMSERVICE_SIZE        (256U * 1024U * 1024U)
#define ARMSERVICE_PRODUCT_ID  0x11U
#define ARMSERVICE_MFR_ID      0x6d73U
#define ARMSERVICE_SERIAL      0x4c4e5832U /* "LNX2" */

/* Keep the execution alias outside the 32-bit Amiga address space. */
#define ARMSERVICE_EXEC_ALIAS  0x0000001000000000ULL

#define HI_NIBBLE(v) ((uint8_t)((v) & 0xf0U))
#define LO_NIBBLE(v) ((uint8_t)(((v) << 4) & 0xf0U))
#define INV_HI(v)    ((uint8_t)((~(v)) & 0xf0U))
#define INV_LO(v)    ((uint8_t)((~((v) << 4)) & 0xf0U))

#define RAW_BYTE(v) HI_NIBBLE(v), 0x00, LO_NIBBLE(v), 0x00
#define INV_BYTE(v) INV_HI(v),    0x00, INV_LO(v),    0x00

/*
 * Logical ExpansionRom:
 *   er_Type  = 0x84 : Zorro III, MEMLIST clear, extended size index 4
 *   er_Flags = 0x70 : NOSHUTUP | EXTENDED | ZORRO_III
 * Extended size index 4 = 256 MiB.
 */
static const uint8_t armservice_autoconfig[64] = {
    RAW_BYTE(0x84),
    INV_BYTE(ARMSERVICE_PRODUCT_ID),
    INV_BYTE(0x70),
    INV_BYTE(0x00),

    INV_BYTE((ARMSERVICE_MFR_ID >> 8) & 0xff),
    INV_BYTE(ARMSERVICE_MFR_ID & 0xff),

    INV_BYTE((ARMSERVICE_SERIAL >> 24) & 0xff),
    INV_BYTE((ARMSERVICE_SERIAL >> 16) & 0xff),
    INV_BYTE((ARMSERVICE_SERIAL >> 8) & 0xff),
    INV_BYTE(ARMSERVICE_SERIAL & 0xff),

    INV_BYTE(0x00),
    INV_BYTE(0x00),

    INV_BYTE(0x00),
    INV_BYTE(0x00),
    INV_BYTE(0x00),
    INV_BYTE(0x00),
};

/* Set by mmu_init() while the physical block is removed from normal RAM. */
uintptr_t armservice_phys_base = 0;
/* 32-bit mirror for m68k MOVEC reads on AArch64 big-endian builds. */
volatile uint32_t armservice_phys_base_68k = 0;

/* Set when expansion.library assigns the ZIII board address. */
volatile uint32_t armservice_z3_base = 0;

/* ARM-only, read-only executable alias of the same physical backing. */
volatile uintptr_t armservice_exec_base = 0;

static void armservice_map(struct ExpansionBoard *board)
{
    uintptr_t exec_base;

    if (!armservice_phys_base) {
        kprintf("[ARMSVC] ERROR: no reserved physical backing\n");
        return;
    }

    armservice_z3_base = board->map_base;
    exec_base = ((uintptr_t)board->map_base) | ARMSERVICE_EXEC_ALIAS;
    armservice_exec_base = exec_base;

    /*
     * Amiga/JIT-visible mapping: writable data aperture.
     * With Emu68's WXN policy this mapping is intentionally not executable.
     */
    mmu_map(armservice_phys_base,
            board->map_base,
            board->rom_size,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED,
            0);

    /*
     * CPU1 execution mapping: same physical pages, read-only.
     * No MMU_ALLOW_EL0 is needed: the POC payload executes at EL1.
     * Read-only is important because SCTLR_EL1.WXN forbids execution from a
     * writable mapping.
     */
    mmu_map(armservice_phys_base,
            exec_base,
            board->rom_size,
            MMU_ACCESS | MMU_ISHARE | MMU_READ_ONLY | MMU_ATTR_CACHED,
            0);

    __asm__ volatile("dsb sy; isb" ::: "memory");

    kprintf("[ARMSVC] configured: ZIII-RW=%08x ARM-RX=%016lx "
            "phys=%08lx size=%u MiB\n",
            board->map_base,
            exec_base,
            armservice_phys_base,
            (unsigned)(board->rom_size >> 20));
}

static struct ExpansionBoard armservice_board = {
    armservice_autoconfig,
    ARMSERVICE_SIZE,
    0,
    1,
    1,
    armservice_map
};

static void armservice_init(void)
{
    kprintf("[ARMSVC] registered: 256 MiB ZIII non-MEMLIST W^X service aperture, "
            "mfr=%04x product=%02x phys=%08lx\n",
            ARMSERVICE_MFR_ID,
            ARMSERVICE_PRODUCT_ID,
            armservice_phys_base);
}

static void * __attribute__((used, section(".init"))) _armservice_init =
    &armservice_init;

static void * __attribute__((used, section(".boards.z3"))) _armservice_board =
    &armservice_board;
