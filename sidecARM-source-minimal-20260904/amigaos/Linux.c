/* Linux-armblk-poc2.c
 * AArch64 Linux handoff + integrated AmigaOS armblk backend.
 * Usage: Linux <Image> <dtb> <initramfs.cpio> <diskfile>
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

struct ExecBase *SysBase;

#define EMU68_MANUFACTURER 0x6d73
#define ARMSERVICE_PRODUCT 0x11

#define SERVICE_SIZE       0x08000000UL
#define INITRD_OFF         0x04000000UL
#define INITRD_MAX         0x02000000UL  /* 32 MiB Alpine initramfs window */
#define DTB_OFF            0x06000000UL
#define DTB_MAX            0x00200000UL

#define ARMBLK_OFF         0x039e0000UL
#define ARMBLK_SIZE        0x00101000UL
#define ARMNET_OFF         0x03b00000UL
#define ARMNET_SIZE        0x00041000UL
#define ARMTERM_OFF        0x03b80000UL
#define ARMTERM_SIZE       0x00021000UL
#define ARMFB_OFF          0x03c00000UL
#define ARMFB_SIZE         0x00200000UL
#define ARMFB_WIDTH        800UL
#define ARMFB_HEIGHT       600UL
#define ARMFB_PITCH        (ARMFB_WIDTH * 2UL)

#define ST_ALIVE   0x414c4956UL /* ALIV */
#define ST_FLUSH   0x464c5348UL /* FLSH */
#define ST_HANDOFF 0x484e444fUL /* HNDO */
#define ST_LINUX   0x4c4e5833UL /* LNX3 */

static ULONG read_cr(ULONG which)
{
    ULONG v = 0;
    APTR old = SuperState();
    switch (which) {
        case 0x1e2: __asm__ volatile("movec #0x1e2,%0" : "=r"(v)); break;
        case 0x1e3: __asm__ volatile("movec #0x1e3,%0" : "=r"(v)); break;
        case 0x1e4: __asm__ volatile("movec #0x1e4,%0" : "=r"(v)); break;
        case 0x1e5: __asm__ volatile("movec #0x1e5,%0" : "=r"(v)); break;
        case 0x1e6: __asm__ volatile("movec #0x1e6,%0" : "=r"(v)); break;
    }
    if (old) UserState(old);
    return v;
}

static void write_cr(ULONG which, ULONG v)
{
    APTR old = SuperState();
    if (which == 0x1e1)
        __asm__ volatile("movec %0,#0x1e1" :: "r"(v) : "memory");
    else if (which == 0x1e4)
        __asm__ volatile("movec %0,#0x1e4" :: "r"(v) : "memory");
    else if (which == 0x1e5)
        __asm__ volatile("movec %0,#0x1e5" :: "r"(v) : "memory");
    if (old) UserState(old);
}

static ULONG le32(const unsigned char *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) |
           ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static unsigned long long le64(const unsigned char *p)
{
    unsigned long long lo = le32(p);
    unsigned long long hi = le32(p + 4);
    return lo | (hi << 32);
}

static int load_at(const char *name, unsigned char *dst,
                   ULONG max_size, long *loaded)
{
    FILE *f = fopen(name, "rb");
    long size;
    size_t got;

    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (size <= 0 || (ULONG)size > max_size) {
        fclose(f);
        return 0;
    }
    got = fread(dst, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return 0;
    if (loaded) *loaded = size;
    return 1;
}



/* ------------------------------------------------------------------------- */
/* Temporary VC4/HVS direct ARMFB display                                    */
/*                                                                           */
/* This intentionally mirrors only the unity-RGB565 display-list sequence    */
/* used by VideoCore.card on VC4 (Pi 0-3).  It does not touch Picasso96       */
/* state apart from temporarily replacing channel 1's active display-list    */
/* pointer, which is restored before returning to the shell.                  */
/* ------------------------------------------------------------------------- */

#define HVS_BASE_ADDR          0xf2400000UL
#define HVS_DLIST_ADDR         0xf2402000UL
#define HVS_DISPLIST1_ADDR     0xf2400024UL
#define HVS_ARMFB_SLOT         0x0400UL  /* outside VideoCore.card's 0..0x3ff */
#define HVS_DISPSTAT1_ADDR     0xf2400058UL
#define HVS_FRAME_COUNT_MASK   0x0003f000UL
#define HVS_FRAME_COUNT_SHIFT  12
#define HVS_SAFE_DLIST_LIMIT   0x0300UL

#define HVS_CONTROL_FORMAT(n)      ((n) & 0xf)
#define HVS_CONTROL_VALID          (1UL << 30)
#define HVS_CONTROL_WORDS(n)       (((n) & 0x3f) << 24)
#define HVS_CONTROL_PIXEL_ORDER(n) (((n) & 3) << 13)
#define HVS_CONTROL_UNITY          (1UL << 4)
#define HVS_POS0_X(n)              ((n) & 0xfff)
#define HVS_POS0_Y(n)              (((n) & 0xfff) << 12)
#define HVS_POS0_ALPHA(n)          (((n) & 0xff) << 24)
#define HVS_POS2_W(n)              ((n) & 0xffff)
#define HVS_POS2_H(n)              (((n) & 0xffff) << 16)
#define HVS_PIXEL_FORMAT_RGB565    4UL
#define HVS_PIXEL_ORDER_XRGB       2UL
#define HVS_END                    0x80000000UL

static ULONG hvs_old_displist1;
static ULONG hvs_saved_words[8];
static int hvs_takeover_active;

static ULONG hvs_bswap32(ULONG v)
{
    return ((v & 0x000000ffUL) << 24) |
           ((v & 0x0000ff00UL) << 8)  |
           ((v & 0x00ff0000UL) >> 8)  |
           ((v & 0xff000000UL) >> 24);
}

static ULONG hvs_read32(volatile ULONG *p)
{
    return hvs_bswap32(*p);
}

static void hvs_write32(volatile ULONG *p, ULONG v)
{
    *p = hvs_bswap32(v);
    __asm__ volatile("nop" ::: "memory");
}

static int hvs_wait_next_frame(void)
{
    volatile ULONG *stat = (volatile ULONG *)HVS_DISPSTAT1_ADDR;
    ULONG old_frame = (hvs_read32(stat) & HVS_FRAME_COUNT_MASK) >> HVS_FRAME_COUNT_SHIFT;
    ULONG timeout = 5000000UL;

    while (timeout--) {
        ULONG frame = (hvs_read32(stat) & HVS_FRAME_COUNT_MASK) >> HVS_FRAME_COUNT_SHIFT;
        if (frame != old_frame)
            return 1;
        __asm__ volatile("nop" ::: "memory");
    }
    return 0;
}

static int armfb_hvs_enable(ULONG armfb_phys)
{
    volatile ULONG *dlist = (volatile ULONG *)HVS_DLIST_ADDR;
    volatile ULONG *active = (volatile ULONG *)HVS_DISPLIST1_ADDR;
    const ULONG pos = HVS_ARMFB_SLOT;
    ULONG source_bus;
    int i;

    /* VC4/HVS uses the 0xc0000000 SDRAM bus alias, as VideoCore.card does. */
    if (armfb_phys & 0xc0000000UL) {
        printf("ARMFB direct display: physical address %08lx is outside VC4 low-RAM range.\n",
               armfb_phys);
        return 0;
    }
    source_bus = 0xc0000000UL | armfb_phys;

    hvs_old_displist1 = hvs_read32(active);
    if (hvs_old_displist1 >= HVS_SAFE_DLIST_LIMIT)
        return 0;

    /* Preserve the temporary HVS slot byte-for-byte (in CPU-endian form) so
     * the takeover is completely reversible even if VideoCore.card happened
     * to have data there already. */
    for (i = 0; i < 8; ++i)
        hvs_saved_words[i] = hvs_read32(&dlist[pos + i]);

    /* Seven-word unity plane + terminating END word.  Position (0,0) is
     * deliberately used: no scaling, no P96 dependency, no display-size
     * discovery.  On a larger HDMI mode this is simply an 800x600 rectangle
     * at the top-left.  The first functional version prioritises robustness. */
    hvs_write32(&dlist[pos + 0],
                HVS_CONTROL_VALID |
                HVS_CONTROL_WORDS(7) |
                HVS_CONTROL_UNITY |
                HVS_CONTROL_FORMAT(HVS_PIXEL_FORMAT_RGB565) |
                HVS_CONTROL_PIXEL_ORDER(HVS_PIXEL_ORDER_XRGB));
    hvs_write32(&dlist[pos + 1], HVS_POS0_X(0) | HVS_POS0_Y(0) | HVS_POS0_ALPHA(0xff));
    hvs_write32(&dlist[pos + 2], HVS_POS2_H(ARMFB_HEIGHT) | HVS_POS2_W(ARMFB_WIDTH) | (1UL << 30));
    hvs_write32(&dlist[pos + 3], 0xdeadbeefUL);
    hvs_write32(&dlist[pos + 4], source_bus);
    hvs_write32(&dlist[pos + 5], 0xdeadbeefUL);
    hvs_write32(&dlist[pos + 6], ARMFB_PITCH);
    hvs_write32(&dlist[pos + 7], HVS_END);

    /* VideoCore.card changes DISPLIST1 on a frame boundary.  Do the same
     * for the temporary ARMFB takeover. */
    if (!hvs_wait_next_frame())
        return 0;
    hvs_write32(active, pos);
    hvs_takeover_active = 1;

    printf("ARMFB direct display ON: phys=%08lx bus=%08lx 800x600 RGB565, HVS slot=%lx\n",
           armfb_phys, source_bus, pos);
    return 1;
}

static void armfb_hvs_disable(void)
{
    volatile ULONG *dlist = (volatile ULONG *)HVS_DLIST_ADDR;
    volatile ULONG *active = (volatile ULONG *)HVS_DISPLIST1_ADDR;
    const ULONG pos = HVS_ARMFB_SLOT;
    int i;

    if (!hvs_takeover_active)
        return;

    /* Restore DISPLIST1 on an HVS frame boundary, matching VideoCore.card. */
    (void)hvs_wait_next_frame();
    if (hvs_old_displist1 < HVS_SAFE_DLIST_LIMIT)
        hvs_write32(active, hvs_old_displist1);

    /* Only restore the temporary slot after HVS has advanced to another
     * frame, so it cannot still be fetching words from slot 0x400. */
    if (hvs_wait_next_frame()) {
        for (i = 0; i < 8; ++i)
            hvs_write32(&dlist[pos + i], hvs_saved_words[i]);
    }

    hvs_takeover_active = 0;
    printf("ARMFB direct display OFF: restored HVS display-list %08lx\n",
           hvs_old_displist1);
}

static BPTR armfb_key_input;
static int armfb_key_raw;

static void armfb_keywait_begin(void)
{
    UBYTE ch;

    armfb_key_input = Input();
    armfb_key_raw = 0;

    if (!armfb_key_input)
        return;

    /* Enter used to launch "Linux ... -fb" can still be pending on the
     * Amiga console.  Enter RAW mode and drain everything already queued
     * BEFORE switching HVS, so only a genuinely new key can end the view. */
    SetMode(armfb_key_input, DOSTRUE);
    armfb_key_raw = 1;

    while (WaitForChar(armfb_key_input, 0)) {
        if (Read(armfb_key_input, &ch, 1) != 1)
            break;
    }
}

static void armfb_wait_new_key(void)
{
    UBYTE ch;

    printf("ARMFB direct display active. Press any NEW key to return to AmigaOS.\n");
    fflush(stdout);

    if (armfb_key_input)
        Read(armfb_key_input, &ch, 1);
}

static void armfb_keywait_end(void)
{
    if (armfb_key_input && armfb_key_raw)
        SetMode(armfb_key_input, DOSFALSE);

    armfb_key_input = 0;
    armfb_key_raw = 0;
}

/* ------------------------------------------------------------------------- */
/* armblk shared-memory ABI + AmigaOS backend                                */
/* ------------------------------------------------------------------------- */

#define ARMBLK_SHM_MAGIC          0x41524d42UL /* "ARMB" */
#define ARMBLK_SHM_VERSION        1UL
#define ARMBLK_QUEUE_DEPTH        8UL
#define ARMBLK_MAX_BYTES          (128UL * 1024UL)

#define ARMBLK_F_BACKEND_UP       (1UL << 0)
#define ARMBLK_F_LINUX_UP         (1UL << 1)

#define ARMBLK_SLOT_FREE          0UL
#define ARMBLK_SLOT_REQ           1UL
#define ARMBLK_SLOT_DONE          2UL

#define ARMBLK_OP_GET_SIZE        1UL
#define ARMBLK_OP_READ            2UL
#define ARMBLK_OP_WRITE           3UL
#define ARMBLK_OP_FLUSH           4UL
#define ARMBLK_OP_RESET           5UL

#define ARMBLK_ST_OK               0L
#define ARMBLK_ST_EIO             -5L
#define ARMBLK_ST_EINVAL         -22L
#define ARMBLK_ST_EFBIG          -27L

struct armblk_u64 {
    ULONG hi;
    ULONG lo;
};

struct armblk_slot {
    ULONG state;
    ULONG op;
    ULONG id;
    ULONG status;
    struct armblk_u64 offset;
    ULONG length;
    ULONG reserved0;
    struct armblk_u64 result;
    UBYTE data[ARMBLK_MAX_BYTES];
};

struct armblk_shm {
    ULONG magic;
    ULONG version;
    ULONG total_size;
    ULONG flags;
    ULONG queue_depth;
    ULONG max_bytes;
    ULONG reserved0;
    ULONG reserved1;
    struct armblk_slot slot[ARMBLK_QUEUE_DEPTH];
};

static inline void armblk_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

static void armblk_set_u64(volatile struct armblk_u64 *v, ULONG hi, ULONG lo)
{
    v->hi = hi;
    v->lo = lo;
}

/* Internal mode of this same executable.  Linux starts a second copy of
 * itself as: Linux --armblk-backend <image>.  That process owns the backing
 * file for as long as Linux is running. */
static int armblk_backend(const char *image)
{
    struct Library *ExpansionBase;
    struct ConfigDev *cd;
    volatile struct armblk_shm *shm;
    FILE *fh;
    long end;
    ULONG size, i;

    ExpansionBase = OpenLibrary("expansion.library", 0);
    if (!ExpansionBase)
        return 20;

    cd = FindConfigDev(NULL, EMU68_MANUFACTURER, ARMSERVICE_PRODUCT);
    if (!cd) {
        CloseLibrary(ExpansionBase);
        return 20;
    }

    shm = (volatile struct armblk_shm *)
        ((volatile UBYTE *)cd->cd_BoardAddr + ARMBLK_OFF);
    CloseLibrary(ExpansionBase);

    if (sizeof(struct armblk_shm) > ARMBLK_SIZE)
        return 20;

    fh = fopen(image, "r+b");
    if (!fh)
        return 20;

    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        return 20;
    }
    end = ftell(fh);
    if (end <= 0 || (unsigned long)end >= 0x80000000UL || (end & 511L)) {
        fclose(fh);
        return 20;
    }
    size = (ULONG)end;
    rewind(fh);

    memset((void *)shm, 0, sizeof(*shm));
    shm->magic = ARMBLK_SHM_MAGIC;
    shm->version = ARMBLK_SHM_VERSION;
    shm->total_size = (ULONG)sizeof(*shm);
    shm->queue_depth = ARMBLK_QUEUE_DEPTH;
    shm->max_bytes = ARMBLK_MAX_BYTES;
    for (i = 0; i < ARMBLK_QUEUE_DEPTH; ++i)
        shm->slot[i].state = ARMBLK_SLOT_FREE;
    armblk_barrier();
    shm->flags = ARMBLK_F_BACKEND_UP;
    armblk_barrier();

    for (;;) {
        int did_work = 0;

        for (i = 0; i < ARMBLK_QUEUE_DEPTH; ++i) {
            volatile struct armblk_slot *s = &shm->slot[i];
            ULONG op, len, off_hi, off_lo;
            LONG status = ARMBLK_ST_OK;
            size_t n;

            if (s->state != ARMBLK_SLOT_REQ)
                continue;

            did_work = 1;
            armblk_barrier();
            op = s->op;
            len = s->length;
            off_hi = s->offset.hi;
            off_lo = s->offset.lo;
            s->status = 0;
            armblk_set_u64(&s->result, 0, 0);

            switch (op) {
            case ARMBLK_OP_GET_SIZE:
                armblk_set_u64(&s->result, 0, size);
                break;

            case ARMBLK_OP_READ:
                if (off_hi || len > ARMBLK_MAX_BYTES ||
                    off_lo > size || len > size - off_lo) {
                    status = ARMBLK_ST_EINVAL;
                    break;
                }
                if (off_lo & 0x80000000UL ||
                    fseek(fh, (long)off_lo, SEEK_SET) != 0) {
                    status = ARMBLK_ST_EFBIG;
                    break;
                }
                n = fread((void *)s->data, 1, len, fh);
                if (n != len) {
                    clearerr(fh);
                    status = ARMBLK_ST_EIO;
                    break;
                }
                armblk_set_u64(&s->result, 0, len);
                break;

            case ARMBLK_OP_WRITE:
                if (off_hi || len > ARMBLK_MAX_BYTES ||
                    off_lo > size || len > size - off_lo) {
                    status = ARMBLK_ST_EINVAL;
                    break;
                }
                if (off_lo & 0x80000000UL ||
                    fseek(fh, (long)off_lo, SEEK_SET) != 0) {
                    status = ARMBLK_ST_EFBIG;
                    break;
                }
                n = fwrite((const void *)s->data, 1, len, fh);
                if (n != len) {
                    clearerr(fh);
                    status = ARMBLK_ST_EIO;
                    break;
                }
                armblk_set_u64(&s->result, 0, len);
                break;

            case ARMBLK_OP_FLUSH:
            case ARMBLK_OP_RESET:
                if (fflush(fh) != 0)
                    status = ARMBLK_ST_EIO;
                break;

            default:
                status = ARMBLK_ST_EINVAL;
                break;
            }

            s->status = (ULONG)status;
            armblk_barrier();
            s->state = ARMBLK_SLOT_DONE;
            armblk_barrier();
        }

        /* Backend normally lives for the whole Linux session.  Stop manually
         * with Ctrl-C only when running the hidden mode from a shell. */
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            break;
        if (!did_work)
            Delay(1);
    }

    shm->flags &= ~ARMBLK_F_BACKEND_UP;
    armblk_barrier();
    fflush(fh);
    fclose(fh);
    return 0;
}

static int armblk_spawn_backend(const char *self, const char *disk,
                                volatile struct armblk_shm *shm)
{
    char cmd[768];
    ULONG ticks;
    int rc;

    /* Clear stale signature before spawning so BACKEND_UP is a real ack from
     * this invocation.  POC assumes no older armblk backend is still alive. */
    memset((void *)shm, 0, sizeof(*shm));
    armblk_barrier();

    if (strlen(self) + strlen(disk) + 48 >= sizeof(cmd)) {
        printf("STOP: armblk command line too long.\n");
        return 0;
    }

    sprintf(cmd, "Run >NIL: \"%s\" --armblk-backend \"%s\"", self, disk);
    rc = system(cmd);
    if (rc != 0) {
        printf("STOP: cannot start armblk backend (rc=%d).\n", rc);
        return 0;
    }

    /* 100 ticks = about two seconds on PAL. */
    for (ticks = 0; ticks < 100; ++ticks) {
        armblk_barrier();
        if (shm->magic == ARMBLK_SHM_MAGIC &&
            shm->version == ARMBLK_SHM_VERSION &&
            (shm->flags & ARMBLK_F_BACKEND_UP)) {
            printf("armblk backend UP: %s\n", disk);
            return 1;
        }
        Delay(1);
    }

    printf("STOP: armblk backend did not become ready: %s\n", disk);
    return 0;
}

/* Minimal FDT editor for a real Raspberry Pi DTB.
 *
 * We deliberately do not depend on libfdt on AmigaOS.  The editor rebuilds
 * only the structure/string blocks while preserving every unrelated node and
 * property byte-for-byte.  It changes:
 *   root memory-node /reg
 *   /chosen/bootargs
 *   /chosen/linux,initrd-start
 *   /chosen/linux,initrd-end
 *   armblk shared reserved-memory node /reg
 *   armblk platform-device node /reg
 *   armnet shared reserved-memory node /reg
 *   armnet platform-device node /reg
 *   armterm shared reserved-memory node /reg
 *   armterm platform-device node /reg
 */
#define FDT_MAGIC       0xd00dfeedUL
#define FDT_BEGIN_NODE  0x00000001UL
#define FDT_END_NODE    0x00000002UL
#define FDT_PROP        0x00000003UL
#define FDT_NOP         0x00000004UL
#define FDT_END         0x00000009UL

#define POC5_BOOTARGS "maxcpus=1 rdinit=/init loglevel=8 ignore_loglevel"

static ULONG be32_get(const unsigned char *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8) | (ULONG)p[3];
}

static void be32_put(unsigned char *p, ULONG v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void be64_put(unsigned char *p, unsigned long long v)
{
    be32_put(p, (ULONG)(v >> 32));
    be32_put(p + 4, (ULONG)v);
}

static ULONG align4(ULONG v) { return (v + 3UL) & ~3UL; }
static ULONG align8(ULONG v) { return (v + 7UL) & ~7UL; }

struct fdt_header_poc {
    ULONG magic;
    ULONG totalsize;
    ULONG off_dt_struct;
    ULONG off_dt_strings;
    ULONG off_mem_rsvmap;
    ULONG version;
    ULONG last_comp_version;
    ULONG boot_cpuid_phys;
    ULONG size_dt_strings;
    ULONG size_dt_struct;
};

struct fdt_info_poc {
    ULONG totalsize, off_struct, off_strings, off_rsv;
    ULONG size_struct, size_strings;
    ULONG version, last_comp, boot_cpuid;
};

static int fdt_header_read(const unsigned char *fdt, ULONG file_size,
                           struct fdt_info_poc *i)
{
    if (file_size < 40 || be32_get(fdt) != FDT_MAGIC) return 0;
    i->totalsize    = be32_get(fdt + 4);
    i->off_struct   = be32_get(fdt + 8);
    i->off_strings  = be32_get(fdt + 12);
    i->off_rsv      = be32_get(fdt + 16);
    i->version      = be32_get(fdt + 20);
    i->last_comp    = be32_get(fdt + 24);
    i->boot_cpuid   = be32_get(fdt + 28);
    i->size_strings = be32_get(fdt + 32);
    i->size_struct  = be32_get(fdt + 36);
    if (i->totalsize > file_size || i->totalsize < 40) return 0;
    if (i->off_struct + i->size_struct > i->totalsize) return 0;
    if (i->off_strings + i->size_strings > i->totalsize) return 0;
    if (i->off_rsv < 40 || i->off_rsv >= i->totalsize) return 0;
    return 1;
}

static const char *fdt_prop_name(const unsigned char *strings, ULONG strings_size,
                                 ULONG nameoff)
{
    ULONG n;
    if (nameoff >= strings_size) return NULL;
    for (n = nameoff; n < strings_size; ++n)
        if (strings[n] == 0) return (const char *)(strings + nameoff);
    return NULL;
}

static int starts_memory_node(const char *s)
{
    return !strcmp(s, "memory") || !strncmp(s, "memory@", 7);
}

static int out_bytes(unsigned char *out, ULONG cap, ULONG *op,
                     const void *src, ULONG n)
{
    if (*op > cap || n > cap - *op) return 0;
    memcpy(out + *op, src, n);
    *op += n;
    return 1;
}

static int out_u32(unsigned char *out, ULONG cap, ULONG *op, ULONG v)
{
    unsigned char b[4];
    be32_put(b, v);
    return out_bytes(out, cap, op, b, 4);
}

static int out_prop(unsigned char *out, ULONG cap, ULONG *op,
                    ULONG nameoff, const void *data, ULONG len)
{
    ULONG padded = align4(len), old;
    unsigned char zero[4] = {0,0,0,0};
    if (!out_u32(out, cap, op, FDT_PROP) ||
        !out_u32(out, cap, op, len) ||
        !out_u32(out, cap, op, nameoff) ||
        !out_bytes(out, cap, op, data, len)) return 0;
    old = *op;
    if (padded > len && !out_bytes(out, cap, op, zero, padded-len)) return 0;
    (void)old;
    return 1;
}

static ULONG add_string(unsigned char *extra, ULONG cap, ULONG *used,
                        const char *name, ULONG original_strings_size)
{
    ULONG n = (ULONG)strlen(name) + 1, off = original_strings_size + *used;
    if (*used > cap || n > cap - *used) return 0xffffffffUL;
    memcpy(extra + *used, name, n);
    *used += n;
    return off;
}

/* Rebuild a standard FDT in a temporary 2 MiB buffer, preserving everything
 * except the four properties named above.  Returns new DTB size or 0. */
static ULONG patch_real_fdt(unsigned char *dtb, ULONG file_size, ULONG capacity,
                            ULONG ram_phys, ULONG initrd_start, ULONG initrd_end)
{
    struct fdt_info_poc fi;
    unsigned char *tmp = NULL, *struct_out, *extra_strings;
    const unsigned char *sp, *send, *strings;
    ULONG struct_cap, op = 0, depth = 0;
    ULONG extra_used = 0, extra_cap = 256;
    ULONG no_bootargs, no_is, no_ie;
    ULONG rsv_len, strings_off, total;
    int in_memory = 0, in_chosen = 0;
    int in_armblk_shm = 0, in_armblk_dev = 0;
    int in_armnet_shm = 0, in_armnet_dev = 0;
    int in_armterm_shm = 0, in_armterm_dev = 0;
    int in_armfb_shm = 0, in_armfb_dev = 0;
    ULONG armblk_shm_depth = 0, armblk_dev_depth = 0;
    ULONG armnet_shm_depth = 0, armnet_dev_depth = 0;
    ULONG armterm_shm_depth = 0, armterm_dev_depth = 0;
    ULONG armfb_shm_depth = 0, armfb_dev_depth = 0;
    int saw_memory_reg = 0, saw_chosen = 0;
    int saw_armblk_shm_reg = 0, saw_armblk_dev_reg = 0;
    int saw_armnet_shm_reg = 0, saw_armnet_dev_reg = 0;
    int saw_armterm_shm_reg = 0, saw_armterm_dev_reg = 0;
    int saw_armfb_shm_reg = 0, saw_armfb_dev_reg = 0;

    if (!fdt_header_read(dtb, file_size, &fi)) {
        printf("STOP: malformed/unsupported FDT header.\n");
        return 0;
    }

    tmp = (unsigned char *)malloc(capacity);
    if (!tmp) {
        printf("STOP: cannot allocate temporary DTB buffer.\n");
        return 0;
    }
    memset(tmp, 0, capacity);

    /* Keep header + reservation map in their conventional positions. */
    rsv_len = fi.off_struct - fi.off_rsv;
    if (fi.off_struct < fi.off_rsv || fi.off_struct > capacity ||
        rsv_len > capacity - fi.off_rsv) goto fail;
    memcpy(tmp + fi.off_rsv, dtb + fi.off_rsv, rsv_len);

    struct_out = tmp + fi.off_struct;
    struct_cap = capacity - fi.off_struct;
    extra_strings = (unsigned char *)malloc(extra_cap);
    if (!extra_strings) goto fail;

    no_bootargs = add_string(extra_strings, extra_cap, &extra_used,
                             "bootargs", fi.size_strings);
    no_is = add_string(extra_strings, extra_cap, &extra_used,
                       "linux,initrd-start", fi.size_strings);
    no_ie = add_string(extra_strings, extra_cap, &extra_used,
                       "linux,initrd-end", fi.size_strings);
    if (no_bootargs == 0xffffffffUL || no_is == 0xffffffffUL || no_ie == 0xffffffffUL)
        goto fail_extra;

    sp = dtb + fi.off_struct;
    send = sp + fi.size_struct;
    strings = dtb + fi.off_strings;

    while (sp + 4 <= send) {
        ULONG tok = be32_get(sp); sp += 4;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)sp;
            ULONG rem = (ULONG)(send - sp), n = 0, padded;
            while (n < rem && sp[n]) ++n;
            if (n >= rem) goto fail_extra;
            padded = align4(n + 1);
            if (sp + padded > send) goto fail_extra;
            if (!out_u32(struct_out, struct_cap, &op, tok) ||
                !out_bytes(struct_out, struct_cap, &op, sp, padded)) goto fail_extra;
            sp += padded;
            depth++;
            if (depth == 2) {
                in_memory = starts_memory_node(name);
                in_chosen = !strcmp(name, "chosen");
                if (in_chosen) saw_chosen = 1;
                if (!strncmp(name, "armblk@", 7)) {
                    in_armblk_dev = 1;
                    armblk_dev_depth = depth;
                }
                if (!strncmp(name, "armnet@", 7)) {
                    in_armnet_dev = 1;
                    armnet_dev_depth = depth;
                }
                if (!strncmp(name, "armterm@", 8)) {
                    in_armterm_dev = 1;
                    armterm_dev_depth = depth;
                }
                if (!strncmp(name, "armfb@", 6)) {
                    in_armfb_dev = 1;
                    armfb_dev_depth = depth;
                }
            }
            if (!strncmp(name, "armblk-shm@", 11)) {
                in_armblk_shm = 1;
                armblk_shm_depth = depth;
            }
            if (!strncmp(name, "armnet-shm@", 11)) {
                in_armnet_shm = 1;
                armnet_shm_depth = depth;
            }
            if (!strncmp(name, "armterm-shm@", 12)) {
                in_armterm_shm = 1;
                armterm_shm_depth = depth;
            }
            if (!strncmp(name, "armfb-shm@", 10)) {
                in_armfb_shm = 1;
                armfb_shm_depth = depth;
            }
        } else if (tok == FDT_END_NODE) {
            if (depth == 2 && in_chosen) {
                unsigned char a64[8];
                const char ba[] = POC5_BOOTARGS;
                if (!out_prop(struct_out, struct_cap, &op, no_bootargs,
                              ba, (ULONG)sizeof(ba))) goto fail_extra;
                be64_put(a64, (unsigned long long)initrd_start);
                if (!out_prop(struct_out, struct_cap, &op, no_is, a64, 8)) goto fail_extra;
                be64_put(a64, (unsigned long long)initrd_end);
                if (!out_prop(struct_out, struct_cap, &op, no_ie, a64, 8)) goto fail_extra;
            }
            if (!out_u32(struct_out, struct_cap, &op, tok)) goto fail_extra;
            if (in_armblk_shm && depth == armblk_shm_depth)
                in_armblk_shm = 0;
            if (in_armblk_dev && depth == armblk_dev_depth)
                in_armblk_dev = 0;
            if (in_armnet_shm && depth == armnet_shm_depth)
                in_armnet_shm = 0;
            if (in_armnet_dev && depth == armnet_dev_depth)
                in_armnet_dev = 0;
            if (in_armterm_shm && depth == armterm_shm_depth)
                in_armterm_shm = 0;
            if (in_armterm_dev && depth == armterm_dev_depth)
                in_armterm_dev = 0;
            if (in_armfb_shm && depth == armfb_shm_depth)
                in_armfb_shm = 0;
            if (in_armfb_dev && depth == armfb_dev_depth)
                in_armfb_dev = 0;
            if (depth == 2) { in_memory = 0; in_chosen = 0; }
            if (!depth) goto fail_extra;
            depth--;
        } else if (tok == FDT_PROP) {
            ULONG len, nameoff, padded;
            const char *pname;
            if (sp + 8 > send) goto fail_extra;
            len = be32_get(sp); nameoff = be32_get(sp + 4); sp += 8;
            padded = align4(len);
            if (sp + padded > send) goto fail_extra;
            pname = fdt_prop_name(strings, fi.size_strings, nameoff);
            if (!pname) goto fail_extra;

            if (in_memory && !strcmp(pname, "reg")) {
                unsigned char regbuf[16];
                if (len == 8) {
                    be32_put(regbuf, ram_phys);
                    be32_put(regbuf + 4, SERVICE_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op, nameoff, regbuf, 8)) goto fail_extra;
                } else if (len == 16) {
                    be64_put(regbuf, (unsigned long long)ram_phys);
                    be64_put(regbuf + 8, (unsigned long long)SERVICE_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op, nameoff, regbuf, 16)) goto fail_extra;
                } else {
                    printf("STOP: /memory/reg has unexpected length %lu.\n", len);
                    goto fail_extra;
                }
                saw_memory_reg = 1;
            } else if ((in_armblk_shm || in_armblk_dev) && !strcmp(pname, "reg")) {
                unsigned char regbuf[16];
                ULONG armblk_phys = ram_phys + ARMBLK_OFF;

                if (len == 8) {
                    be32_put(regbuf, armblk_phys);
                    be32_put(regbuf + 4, ARMBLK_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op,
                                  nameoff, regbuf, 8)) goto fail_extra;
                } else if (len == 16) {
                    be64_put(regbuf, (unsigned long long)armblk_phys);
                    be64_put(regbuf + 8, (unsigned long long)ARMBLK_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op,
                                  nameoff, regbuf, 16)) goto fail_extra;
                } else {
                    printf("STOP: armblk /reg has unexpected length %lu.\n", len);
                    goto fail_extra;
                }

                if (in_armblk_shm) saw_armblk_shm_reg = 1;
                if (in_armblk_dev) saw_armblk_dev_reg = 1;
            } else if ((in_armnet_shm || in_armnet_dev) && !strcmp(pname, "reg")) {
                unsigned char regbuf[16];
                ULONG armnet_phys = ram_phys + ARMNET_OFF;

                if (len == 8) {
                    be32_put(regbuf, armnet_phys);
                    be32_put(regbuf + 4, ARMNET_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op,
                                  nameoff, regbuf, 8)) goto fail_extra;
                } else if (len == 16) {
                    be64_put(regbuf, (unsigned long long)armnet_phys);
                    be64_put(regbuf + 8, (unsigned long long)ARMNET_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op,
                                  nameoff, regbuf, 16)) goto fail_extra;
                } else {
                    printf("STOP: armnet /reg has unexpected length %lu.\n", len);
                    goto fail_extra;
                }

                if (in_armnet_shm) saw_armnet_shm_reg = 1;
                if (in_armnet_dev) saw_armnet_dev_reg = 1;
            } else if ((in_armterm_shm || in_armterm_dev) && !strcmp(pname, "reg")) {
                unsigned char regbuf[16];
                ULONG armterm_phys = ram_phys + ARMTERM_OFF;

                if (len == 8) {
                    be32_put(regbuf, armterm_phys);
                    be32_put(regbuf + 4, ARMTERM_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op,
                                  nameoff, regbuf, 8)) goto fail_extra;
                } else if (len == 16) {
                    be64_put(regbuf, (unsigned long long)armterm_phys);
                    be64_put(regbuf + 8, (unsigned long long)ARMTERM_SIZE);
                    if (!out_prop(struct_out, struct_cap, &op,
                                  nameoff, regbuf, 16)) goto fail_extra;
                } else {
                    printf("STOP: armterm /reg has unexpected length %lu.\n", len);
                    goto fail_extra;
                }

                if (in_armterm_shm) saw_armterm_shm_reg = 1;
                if (in_armterm_dev) saw_armterm_dev_reg = 1;
            } else if ((in_armfb_shm || in_armfb_dev) && !strcmp(pname, "reg")) {
                unsigned char regbuf[16];
                ULONG armfb_phys = ram_phys + ARMFB_OFF;
                if (len == 8) {
                    be32_put(regbuf, armfb_phys);
                    be32_put(regbuf + 4, ARMFB_SIZE);
                } else if (len == 16) {
                    be64_put(regbuf, (unsigned long long)armfb_phys);
                    be64_put(regbuf + 8, (unsigned long long)ARMFB_SIZE);
                } else {
                    printf("STOP: armfb /reg has unexpected length %lu.\n", len);
                    goto fail_extra;
                }
                if (!out_prop(struct_out, struct_cap, &op, nameoff, regbuf, len)) goto fail_extra;
                if (in_armfb_shm) saw_armfb_shm_reg = 1;
                if (in_armfb_dev) saw_armfb_dev_reg = 1;
            } else if (in_chosen &&
                      (!strcmp(pname, "bootargs") ||
                       !strcmp(pname, "linux,initrd-start") ||
                       !strcmp(pname, "linux,initrd-end"))) {
                /* Drop old value. Fresh values are emitted before /chosen ends. */
            } else {
                if (!out_u32(struct_out, struct_cap, &op, FDT_PROP) ||
                    !out_u32(struct_out, struct_cap, &op, len) ||
                    !out_u32(struct_out, struct_cap, &op, nameoff) ||
                    !out_bytes(struct_out, struct_cap, &op, sp, padded)) goto fail_extra;
            }
            sp += padded;
        } else if (tok == FDT_NOP) {
            if (!out_u32(struct_out, struct_cap, &op, tok)) goto fail_extra;
        } else if (tok == FDT_END) {
            if (!out_u32(struct_out, struct_cap, &op, tok)) goto fail_extra;
            break;
        } else {
            printf("STOP: unknown FDT token %08lx.\n", tok);
            goto fail_extra;
        }
    }

    if (!saw_memory_reg) {
        printf("STOP: no root /memory*/reg property found.\n");
        goto fail_extra;
    }
    if (!saw_chosen) {
        printf("STOP: no root /chosen node found.\n");
        goto fail_extra;
    }
    if (!saw_armblk_shm_reg || !saw_armblk_dev_reg) {
        printf("STOP: armblk DT nodes/reg properties not found.\n");
        goto fail_extra;
    }
    if (!saw_armnet_shm_reg || !saw_armnet_dev_reg) {
        printf("STOP: armnet DT nodes/reg properties not found.\n");
        goto fail_extra;
    }
    if (!saw_armterm_shm_reg || !saw_armterm_dev_reg) {
        printf("STOP: armterm DT nodes/reg properties not found.\n");
        goto fail_extra;
    }
    if (!saw_armfb_shm_reg || !saw_armfb_dev_reg) {
        printf("STOP: armfb DT nodes/reg properties not found.\n");
        goto fail_extra;
    }

    strings_off = align4(fi.off_struct + op);
    if (strings_off > capacity || fi.size_strings > capacity - strings_off ||
        extra_used > capacity - strings_off - fi.size_strings) goto fail_extra;
    memcpy(tmp + strings_off, strings, fi.size_strings);
    memcpy(tmp + strings_off + fi.size_strings, extra_strings, extra_used);
    total = align4(strings_off + fi.size_strings + extra_used);
    if (total > capacity) goto fail_extra;

    /* New FDT header, big-endian. */
    be32_put(tmp + 0,  FDT_MAGIC);
    be32_put(tmp + 4,  total);
    be32_put(tmp + 8,  fi.off_struct);
    be32_put(tmp + 12, strings_off);
    be32_put(tmp + 16, fi.off_rsv);
    be32_put(tmp + 20, fi.version);
    be32_put(tmp + 24, fi.last_comp);
    be32_put(tmp + 28, fi.boot_cpuid);
    be32_put(tmp + 32, fi.size_strings + extra_used);
    be32_put(tmp + 36, op);

    memcpy(dtb, tmp, total);
    free(extra_strings);
    free(tmp);
    return total;

fail_extra:
    free(extra_strings);
fail:
    free(tmp);
    return 0;
}

int main(int argc, char **argv)
{
    struct Library *ExpansionBase;
    struct ConfigDev *cd;
    FILE *f;
    unsigned char hdr[64];
    unsigned long long text_offset, image_size, flags;
    ULONG magic, phys, st;
    long file_size, dtb_size, initrd_size;
    unsigned char *board;
    int direct_fb = 0;

    SysBase = *(struct ExecBase **)4;

    if (argc == 3 && !strcmp(argv[1], "--armblk-backend"))
        return armblk_backend(argv[2]);

    if (argc == 6 && !strcmp(argv[5], "-fb"))
        direct_fb = 1;
    else if (argc != 5) {
        printf("Usage: Linux <Image> <dtb> <initramfs.cpio> <diskfile> [-fb]\n");
        printf("Example: Linux kernel8.img pinux.dtb initramfs.cpio Work:linux.img -fb\n");
        return 5;
    }

    ExpansionBase = OpenLibrary("expansion.library", 0);
    if (!ExpansionBase) return 20;

    cd = FindConfigDev(NULL, EMU68_MANUFACTURER, ARMSERVICE_PRODUCT);
    if (!cd) {
        printf("ARM service ZIII board not found.\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }

    board = (unsigned char *)cd->cd_BoardAddr;
    phys = read_cr(0x1e6);

    printf("ARM service: ZIII=%08lx phys=%08lx size=%lu MiB\n",
           (ULONG)cd->cd_BoardAddr, phys,
           (ULONG)cd->cd_BoardSize >> 20);

    /* Emu68 leaves LNX3 set after handing CPU1 to Linux. Reject a
     * second launch before touching the live armblk shared-memory queue. */
    st = read_cr(0x1e2);
    if (st == ST_LINUX) {
        printf("STOP: Linux is already running on ARM CPU1.\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }

    if (!phys || (phys & 0x001fffffUL)) {
        printf("STOP: invalid/unexpected service physical base %08lx\n", phys);
        printf("Expected a non-zero 2 MiB-aligned final carveout.\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }

    printf("[0/8] Starting armblk backend for %s...\n", argv[4]);
    fflush(stdout);
    if (!armblk_spawn_backend(argv[0], argv[4],
            (volatile struct armblk_shm *)(board + ARMBLK_OFF))) {
        CloseLibrary(ExpansionBase);
        return 20;
    }
    printf("[0/8] armblk backend ready.\n");

    f = fopen(argv[1], "rb");
    if (!f) {
        printf("Cannot open %s\n", argv[1]);
        CloseLibrary(ExpansionBase);
        return 20;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        printf("Image header too short.\n");
        fclose(f);
        CloseLibrary(ExpansionBase);
        return 20;
    }
    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fclose(f);

    magic = le32(hdr + 56);
    if (magic != 0x644d5241UL) {
        printf("Not an uncompressed AArch64 Linux Image (magic=%08lx).\n", magic);
        printf("Use Image/kernel8.img only if it is NOT gzip-compressed.\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }

    text_offset = le64(hdr + 8);
    image_size  = le64(hdr + 16);
    flags       = le64(hdr + 24);
    if (image_size == 0) {
        text_offset = 0x80000ULL;
        image_size = (unsigned long long)file_size;
    }

    if (text_offset >= ARMFB_OFF ||
        (unsigned long long)file_size > (unsigned long long)(ARMFB_OFF - (ULONG)text_offset)) {
        printf("Image overlaps ARMFB/shared-service area: offset=%08lx file=%ld\n",
               (ULONG)text_offset, file_size);
        CloseLibrary(ExpansionBase);
        return 20;
    }

    printf("Linux Image: %ld bytes text_offset=%08lx flags=%08lx%08lx (%s-endian)\n",
           file_size, (ULONG)text_offset,
           (ULONG)(flags >> 32), (ULONG)flags,
           (flags & 1ULL) ? "big" : "little");

    printf("[1/8] Loading Linux Image...\n");
    fflush(stdout);
    if (!load_at(argv[1], board + (ULONG)text_offset,
                 ARMFB_OFF - (ULONG)text_offset, NULL)) {
        printf("Failed loading Image.\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }
    printf("[1/8] Linux Image loaded (%ld bytes).\n", file_size);

    printf("[2/8] Loading Alpine initramfs...\n");
    fflush(stdout);
    if (!load_at(argv[3], board + INITRD_OFF, INITRD_MAX, &initrd_size)) {
        printf("Failed loading initramfs (max 32 MiB).\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }
    printf("[2/8] Alpine initramfs loaded (%ld bytes).\n", initrd_size);

    printf("[3/8] Loading device tree...\n");
    fflush(stdout);
    if (!load_at(argv[2], board + DTB_OFF, DTB_MAX, &dtb_size)) {
        printf("Failed loading DTB (max 2 MiB).\n");
        CloseLibrary(ExpansionBase);
        return 20;
    }
    printf("[3/8] Device tree loaded (%ld bytes).\n", dtb_size);

    if (be32_get(board + DTB_OFF) != FDT_MAGIC) {
        printf("Bad DTB magic at service offset %08lx.\n", DTB_OFF);
        CloseLibrary(ExpansionBase);
        return 20;
    }

    printf("[4/8] Patching device tree...\n");
    fflush(stdout);
    {
        ULONG patched_size = patch_real_fdt(board + DTB_OFF, (ULONG)dtb_size, DTB_MAX,
                                            phys, phys + INITRD_OFF,
                                            phys + INITRD_OFF + (ULONG)initrd_size);
        if (!patched_size) {
            CloseLibrary(ExpansionBase);
            return 20;
        }
        dtb_size = (long)patched_size;
    }
    printf("[4/8] Device tree patched.\n");

    printf("Loaded Image at +%08lx, initramfs %ld bytes at +%08lx, DTB %ld bytes at +%08lx\n",
           (ULONG)text_offset, initrd_size, INITRD_OFF, dtb_size, DTB_OFF);
    printf("Patched real FDT RAM=%08lx initrd=%08lx-%08lx\n",
           phys, phys + INITRD_OFF, phys + INITRD_OFF + (ULONG)initrd_size);
    printf("Patched armblk shared RAM=%08lx-%08lx (+%08lx)\n",
           phys + ARMBLK_OFF,
           phys + ARMBLK_OFF + ARMBLK_SIZE - 1,
           ARMBLK_OFF);
    printf("Patched armnet shared RAM=%08lx-%08lx (+%08lx)\n",
           phys + ARMNET_OFF,
           phys + ARMNET_OFF + ARMNET_SIZE - 1,
           ARMNET_OFF);
    printf("Patched armfb shared RAM=%08lx-%08lx (+%08lx) 800x600 RGB565\n",
           phys + ARMFB_OFF,
           phys + ARMFB_OFF + ARMFB_SIZE - 1,
           ARMFB_OFF);

    printf("[5/8] Checking ARM CPU1 service...\n");
    fflush(stdout);
    write_cr(0x1e1, 1);
    st = read_cr(0x1e2);
    if (st != ST_ALIVE || read_cr(0x1e3) != 1) {
        /* Give CPU1 a little time if the first read raced the service loop. */
        ULONG i;
        for (i = 0; i < 1000000UL && read_cr(0x1e2) != ST_ALIVE; ++i) {}
        st = read_cr(0x1e2);
    }
    if (st != ST_ALIVE) {
        printf("CPU1 ping failed: status=%08lx data=%08lx\n",
               st, read_cr(0x1e3));
        CloseLibrary(ExpansionBase);
        return 20;
    }

    printf("[5/8] ARM CPU1 service alive.\n");

    write_cr(0x1e4, (ULONG)text_offset);
    write_cr(0x1e5, DTB_OFF);

    printf("Handoff: CPU1 -> Linux phys=%08lx DTB=%08lx\n",
           phys + (ULONG)text_offset, phys + DTB_OFF);
    printf("After this point CPU1 belongs to Linux; AmigaOS remains on Emu68.\n");

    printf("[6/8] Starting Linux handoff...\n");
    fflush(stdout);
    write_cr(0x1e1, 3);

    {
        ULONG i;
        ULONG last = 0xffffffffUL;
        for (i = 0; i < 1000000UL; ++i) {
            st = read_cr(0x1e2);
            if (st != last) {
                if (st == ST_FLUSH) {
                    printf("[6/8] Synchronizing service RAM cache...\n");
                    fflush(stdout);
                } else if (st == ST_HANDOFF) {
                    printf("[6/8] Cache synchronized; preparing physical handoff...\n");
                    fflush(stdout);
                }
                last = st;
            }
            if (st == ST_LINUX) break;
        }
    }

    if (st != ST_LINUX) {
        printf("Linux handoff was not acknowledged: status=%08lx\n", st);
        CloseLibrary(ExpansionBase);
        return 20;
    }

    printf("[7/8] Linux handoff committed (entry phys=%08lx).\n", read_cr(0x1e3));

    if (direct_fb) {
        /* Linux now owns CPU1 and will probe armfb from the patched DTB.
         * First drain the Return key that launched this command; only then
         * switch HDMI to the shared framebuffer. */
        armfb_keywait_begin();
        if (armfb_hvs_enable(phys + ARMFB_OFF)) {
            armfb_wait_new_key();
            armfb_hvs_disable();
        }
        armfb_keywait_end();
    }

    CloseLibrary(ExpansionBase);
    return 0;
}
