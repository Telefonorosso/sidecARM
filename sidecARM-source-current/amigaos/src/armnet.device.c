/*
 * armnet.device POC1 -- SANA-II Ethernet over PiStorm ARM-service shared RAM.
 *
 * Amiga alias: cd_BoardAddr + 0x03b00000
 * Linux alias: armservice_phys_base + 0x03b00000
 *
 * RX is serviced by the VBlank interrupt (~50/60 Hz) for the first milestone.
 * No resident helper process and no TAP/user-space component are required.
 */
#include <proto/exec.h>
#include <proto/expansion.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <hardware/intbits.h>
#include <libraries/configvars.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>
#include "armnet_shm.h"

#define STR(s) #s
#define XSTR(s) STR(s)
#define DEVICE_NAME      "armnet.device"
#define DEVICE_DATE      "(26 Aug 2026)"
#define DEVICE_VERSION   0
#define DEVICE_REVISION  1
#define DEVICE_PRIORITY  0
#define DEVICE_ID_STRING "armnet " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE

#define EMU68_MANUFACTURER 0x6d73
#define ARMSERVICE_PRODUCT 0x11
#define ARMNET_OFF          0x03b00000UL
#define ETH_HLEN            14UL
#define ETH_FRAME_MAX       (ETH_HLEN + ARMNET_ETH_MTU)
#define PENDING_BIT         (1 << 4)

#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY 0x4000
#endif
#define NSDEVTYPE_SANA2 7

struct NSDeviceQueryResult {
    ULONG DevQueryFormat;
    ULONG SizeAvailable;
    UWORD DeviceType;
    UWORD DeviceSubType;
    const UWORD *SupportedCommands;
};

struct ExecBase *SysBase;
struct ExpansionBase *ExpansionBase;
static BPTR saved_seg_list;
static BOOL is_open;
static BOOL configured;
static BOOL online;
static volatile struct armnet_shm *shm;
static struct List read_q;
static struct Interrupt vblank_int;
static BOOL vblank_added;
static struct SignalSemaphore tx_sem;
static UBYTE tx_frame[ETH_FRAME_MAX];
static struct Sana2DeviceStats stats;

/* POC1 supports one SANA opener (MiamiDX). */
typedef APTR CopyFunc;
static CopyFunc copy_to_buff;
static CopyFunc copy_from_buff;
static APTR opener_cookie;

static const UBYTE amiga_mac[6] = {0x02,0x68,0x00,0x00,0x00,0x01};
static const UWORD supported_cmds[] = {
    CMD_READ, CMD_WRITE, S2_DEVICEQUERY, S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE, S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS,
    S2_MULTICAST, S2_BROADCAST, S2_TRACKTYPE, S2_UNTRACKTYPE,
    S2_GETTYPESTATS, S2_GETSPECIALSTATS, S2_GETGLOBALSTATS,
    S2_READORPHAN, S2_ONLINE, S2_OFFLINE, NSCMD_DEVICEQUERY, 0
};

static inline void init_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static inline void shared_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

/* SANA-II callbacks use a0=to, a1=from, d0=len and return BOOL in d0. */
static BOOL call_copy(CopyFunc fn, APTR to, APTR from, ULONG len)
{
    register APTR ra0 asm("a0") = to;
    register APTR ra1 asm("a1") = from;
    register ULONG rd0 asm("d0") = len;
    register APTR ra2 asm("a2") = fn;
    if (!fn) return FALSE;
    __asm__ volatile (
        "jsr (%3)"
        : "+r"(rd0)
        : "r"(ra0), "r"(ra1), "a"(ra2)
        : "d1", "a0", "a1", "cc", "memory");
    return rd0 ? TRUE : FALSE;
}

static BOOL parse_open_tags(struct TagItem *t)
{
    copy_to_buff = NULL;
    copy_from_buff = NULL;
    while (t) {
        ULONG tag = t->ti_Tag;
        if (tag == TAG_DONE) break;
        if (tag == TAG_IGNORE) { t++; continue; }
        if (tag == TAG_MORE) { t = (struct TagItem *)t->ti_Data; continue; }
        if (tag == TAG_SKIP) { t += (ULONG)t->ti_Data + 1; continue; }
        if (tag == S2_CopyToBuff) copy_to_buff = (CopyFunc)t->ti_Data;
        else if (tag == S2_CopyFromBuff) copy_from_buff = (CopyFunc)t->ti_Data;
        t++;
    }
    return copy_to_buff && copy_from_buff;
}

static BOOL discover_shared(void)
{
    struct ConfigDev *cd;
    if (shm) return TRUE;
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 0);
    if (!ExpansionBase) return FALSE;
    cd = FindConfigDev(NULL, EMU68_MANUFACTURER, ARMSERVICE_PRODUCT);
    if (!cd) {
        CloseLibrary((struct Library *)ExpansionBase); ExpansionBase = NULL;
        return FALSE;
    }
    shm = (volatile struct armnet_shm *)((volatile UBYTE *)cd->cd_BoardAddr + ARMNET_OFF);
    if (shm->magic != ARMNET_SHM_MAGIC || shm->version != ARMNET_SHM_VERSION) {
        shm = NULL;
        CloseLibrary((struct Library *)ExpansionBase); ExpansionBase = NULL;
        return FALSE;
    }
    CloseLibrary((struct Library *)ExpansionBase); ExpansionBase = NULL;
    return TRUE;
}

static BOOL mac_is_bcast(const UBYTE *p)
{
    int i; for (i=0;i<6;i++) if (p[i] != 0xff) return FALSE; return TRUE;
}
static BOOL mac_is_mcast(const UBYTE *p) { return (p[0] & 1) ? TRUE : FALSE; }
static void mac_copy(UBYTE *d, const UBYTE *s) { int i; for(i=0;i<6;i++) d[i]=s[i]; }
static void zero_mem(APTR p, ULONG n) { UBYTE *q=(UBYTE *)p; while(n--) *q++=0; }

static BOOL tx_publish(const UBYTE *frame, ULONG len)
{
    volatile struct armnet_ring *r;
    volatile struct armnet_slot *s;
    ULONG prod, cons, next;
    if (!shm || len > ARMNET_SLOT_DATA) return FALSE;
    r = &shm->a2l;
    prod = r->prod; cons = r->cons; next = (prod + 1) & ARMNET_RING_MASK;
    if (next == cons) return FALSE;
    s = &r->slot[prod];
    CopyMem((APTR)frame, (APTR)s->data, len);
    s->len = (UWORD)len;
    shared_barrier();
    r->prod = next;
    shared_barrier();
    return TRUE;
}

static void finish_io(struct IOSana2Req *io)
{
    io->ios2_Req.io_Flags &= ~PENDING_BIT;
    if (!(io->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&io->ios2_Req.io_Message);
}

static struct IOSana2Req *find_read_for(ULONG type, BOOL *orphan)
{
    struct Node *n;
    struct IOSana2Req *fallback = NULL;
    *orphan = FALSE;
    for (n = read_q.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct IOSana2Req *io = (struct IOSana2Req *)n;
        if (io->ios2_Req.io_Command == S2_READORPHAN) {
            if (!fallback) fallback = io;
        } else if (io->ios2_Req.io_Command == CMD_READ) {
            if ((io->ios2_Req.io_Flags & SANA2IOF_RAW) || io->ios2_PacketType == type)
                return io;
        }
    }
    if (fallback) *orphan = TRUE;
    return fallback;
}

static void deliver_frame(const UBYTE *frame, ULONG flen)
{
    struct IOSana2Req *io;
    ULONG type, len;
    BOOL orphan;
    APTR src;
    if (flen < ETH_HLEN) { stats.BadData++; return; }
    type = ((ULONG)frame[12] << 8) | frame[13];
    io = find_read_for(type, &orphan);
    if (!io) { stats.UnknownTypesReceived++; return; }
    Remove(&io->ios2_Req.io_Message.mn_Node);
    io->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    mac_copy(io->ios2_DstAddr, frame);
    mac_copy(io->ios2_SrcAddr, frame + 6);
    if (mac_is_bcast(frame)) io->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    else if (mac_is_mcast(frame)) io->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    io->ios2_PacketType = type;
    if (io->ios2_Req.io_Flags & SANA2IOF_RAW) { len=flen; src=(APTR)frame; }
    else { len=flen-ETH_HLEN; src=(APTR)(frame+ETH_HLEN); }
    io->ios2_DataLength = len;
    io->ios2_Req.io_Error = 0;
    io->ios2_WireError = 0;
    if (!call_copy(copy_to_buff, io->ios2_Data, src, len)) {
        io->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        io->ios2_WireError = S2WERR_BUFF_ERROR;
    } else {
        stats.PacketsReceived++;
        if (orphan) stats.UnknownTypesReceived++;
    }
    finish_io(io);
}

static ULONG __attribute__((used)) vblank_handler(APTR data asm("a1"))
{
    int budget = 16;
    (void)data;
    if (!shm || !online) return 0;
    while (budget-- > 0) {
        volatile struct armnet_ring *r = &shm->l2a;
        ULONG cons = r->cons, prod = r->prod, next, len;
        volatile struct armnet_slot *s;
        if (cons == prod) break;
        shared_barrier();
        s = &r->slot[cons]; len = s->len;
        if (len >= ETH_HLEN && len <= ARMNET_SLOT_DATA)
            deliver_frame((const UBYTE *)s->data, len);
        else stats.BadData++;
        next = (cons + 1) & ARMNET_RING_MASK;
        shared_barrier(); r->cons = next; shared_barrier();
    }
    return 0;
}

static void vblank_install(void)
{
    if (vblank_added) return;
    vblank_int.is_Node.ln_Type=NT_INTERRUPT;
    vblank_int.is_Node.ln_Pri=0;
    vblank_int.is_Node.ln_Name=(STRPTR)DEVICE_NAME;
    vblank_int.is_Data=NULL;
    vblank_int.is_Code=(void (*)())vblank_handler;
    AddIntServer(INTB_VERTB,&vblank_int);
    vblank_added=TRUE;
}
static void vblank_remove(void)
{
    if (vblank_added) { RemIntServer(INTB_VERTB,&vblank_int); vblank_added=FALSE; }
}

static void queue_read(struct IOSana2Req *io)
{
    Disable();
    io->ios2_Req.io_Flags |= PENDING_BIT;
    io->ios2_Req.io_Flags &= ~IOF_QUICK;
    AddTail(&read_q, &io->ios2_Req.io_Message.mn_Node);
    Enable();
}

static void do_write(struct IOSana2Req *io, BOOL bcast)
{
    ULONG len, flen, type;
    int i;
    io->ios2_Req.io_Error=0; io->ios2_WireError=0;
    if (!online) { io->ios2_Req.io_Error=S2ERR_OUTOFSERVICE; io->ios2_WireError=S2WERR_UNIT_OFFLINE; return; }
    ObtainSemaphore(&tx_sem);
    if (io->ios2_Req.io_Flags & SANA2IOF_RAW) {
        len=io->ios2_DataLength;
        if (len > ARMNET_SLOT_DATA || !call_copy(copy_from_buff, tx_frame, io->ios2_Data, len)) {
            io->ios2_Req.io_Error = (len > ARMNET_SLOT_DATA) ? S2ERR_MTU_EXCEEDED : S2ERR_NO_RESOURCES;
            io->ios2_WireError = S2WERR_BUFF_ERROR;
            ReleaseSemaphore(&tx_sem); return;
        }
        flen=len;
    } else {
        len=io->ios2_DataLength;
        if (len > ARMNET_ETH_MTU) { io->ios2_Req.io_Error=S2ERR_MTU_EXCEEDED; ReleaseSemaphore(&tx_sem); return; }
        if (bcast) for(i=0;i<6;i++) tx_frame[i]=0xff; else mac_copy(tx_frame,io->ios2_DstAddr);
        mac_copy(tx_frame+6,amiga_mac);
        type=io->ios2_PacketType;
        tx_frame[12]=(UBYTE)(type>>8); tx_frame[13]=(UBYTE)type;
        if (!call_copy(copy_from_buff,tx_frame+ETH_HLEN,io->ios2_Data,len)) {
            io->ios2_Req.io_Error=S2ERR_NO_RESOURCES; io->ios2_WireError=S2WERR_BUFF_ERROR;
            ReleaseSemaphore(&tx_sem); return;
        }
        flen=ETH_HLEN+len;
    }
    if (!tx_publish(tx_frame,flen)) {
        io->ios2_Req.io_Error=S2ERR_NO_RESOURCES; io->ios2_WireError=S2WERR_GENERIC_ERROR;
    } else stats.PacketsSent++;
    ReleaseSemaphore(&tx_sem);
}

static BPTR do_expunge(struct Library *dev)
{
    if (dev->lib_OpenCnt) { dev->lib_Flags |= LIBF_DELEXP; return 0; }
    return 0;
}

static void do_open(struct Library *dev, struct IORequest *req, ULONG unitnum, ULONG flags)
{
    struct IOSana2Req *io=(struct IOSana2Req *)req;
    (void)flags;
    req->io_Error=IOERR_OPENFAIL; req->io_Message.mn_Node.ln_Type=NT_REPLYMSG;
    if (req->io_Message.mn_Length < (UWORD)sizeof(struct IOSana2Req) || unitnum != 0) return;
    if (is_open) { req->io_Error=IOERR_UNITBUSY; return; }
    if (!io->ios2_BufferManagement || !parse_open_tags((struct TagItem *)io->ios2_BufferManagement)) return;
    opener_cookie=io->ios2_BufferManagement;
    if (!discover_shared()) return;
    init_list(&read_q); InitSemaphore(&tx_sem); zero_mem(&stats,sizeof(stats));
    configured=FALSE; online=FALSE; vblank_install(); is_open=TRUE;
    req->io_Unit=(struct Unit *)dev; dev->lib_OpenCnt++; req->io_Error=0;
}

static BPTR do_close(struct Library *dev, struct IORequest *req)
{
    struct Node *n;
    Disable();
    while ((n=RemHead(&read_q)) != NULL) {
        struct IOSana2Req *io=(struct IOSana2Req *)n;
        io->ios2_Req.io_Error=IOERR_ABORTED; io->ios2_Req.io_Flags &= ~PENDING_BIT;
        ReplyMsg(&io->ios2_Req.io_Message);
    }
    Enable();
    vblank_remove();
    if (shm) { shm->flags &= ~ARMNET_F_AMIGA_UP; shared_barrier(); }
    shm=NULL; online=FALSE; configured=FALSE; is_open=FALSE;
    copy_to_buff=copy_from_buff=NULL; opener_cookie=NULL;
    req->io_Device=NULL; req->io_Unit=NULL;
    if (dev->lib_OpenCnt) dev->lib_OpenCnt--;
    return 0;
}

static void do_begin_io(struct Library *dev, struct IORequest *req)
{
    struct IOSana2Req *io=(struct IOSana2Req *)req;
    (void)dev;
    req->io_Message.mn_Node.ln_Type=NT_MESSAGE; req->io_Error=0; io->ios2_WireError=0;
    if (io->ios2_BufferManagement != opener_cookie) {
        req->io_Error=S2ERR_BAD_ARGUMENT; io->ios2_WireError=S2WERR_BUFF_ERROR; goto done;
    }
    switch(req->io_Command) {
    case CMD_READ:
    case S2_READORPHAN:
        if (!online) { req->io_Error=S2ERR_OUTOFSERVICE; io->ios2_WireError=S2WERR_UNIT_OFFLINE; break; }
        queue_read(io); return;
    case CMD_WRITE: do_write(io,FALSE); break;
    case S2_MULTICAST: do_write(io,FALSE); break;
    case S2_BROADCAST: do_write(io,TRUE); break;
    case S2_DEVICEQUERY:
        if (io->ios2_StatData) {
            struct Sana2DeviceQuery *q=(struct Sana2DeviceQuery *)io->ios2_StatData;
            ULONG avail=q->SizeAvailable;
            if (avail < sizeof(*q)) { req->io_Error=S2ERR_BAD_ARGUMENT; io->ios2_WireError=S2WERR_BAD_STATDATA; break; }
            q->SizeSupplied=sizeof(*q); q->DevQueryFormat=0; q->DeviceLevel=0;
            q->AddrFieldSize=48; q->MTU=ARMNET_ETH_MTU; q->BPS=100000000UL;
            q->HardwareType=S2WireType_Ethernet;
        } else { req->io_Error=S2ERR_BAD_ARGUMENT; io->ios2_WireError=S2WERR_BAD_STATDATA; }
        break;
    case S2_GETSTATIONADDRESS:
        mac_copy(io->ios2_SrcAddr,amiga_mac); mac_copy(io->ios2_DstAddr,amiga_mac); break;
    case S2_CONFIGINTERFACE:
        if (configured) { req->io_Error=S2ERR_BAD_STATE; io->ios2_WireError=S2WERR_IS_CONFIGURED; break; }
        configured=TRUE; online=TRUE; mac_copy((UBYTE *)shm->amiga_mac,amiga_mac);
        shm->flags |= ARMNET_F_AMIGA_UP; shared_barrier(); stats.Reconfigurations++; break;
    case S2_ONLINE:
        if (!configured) { req->io_Error=S2ERR_BAD_STATE; io->ios2_WireError=S2WERR_NOT_CONFIGURED; break; }
        online=TRUE; shm->flags |= ARMNET_F_AMIGA_UP; shared_barrier(); break;
    case S2_OFFLINE:
        online=FALSE; shm->flags &= ~ARMNET_F_AMIGA_UP; shared_barrier(); break;
    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
        break;
    case S2_GETGLOBALSTATS:
        if (io->ios2_StatData) CopyMem(&stats,io->ios2_StatData,sizeof(stats));
        else { req->io_Error=S2ERR_BAD_ARGUMENT; io->ios2_WireError=S2WERR_BAD_STATDATA; }
        break;
    case S2_GETTYPESTATS:
        if (io->ios2_StatData) zero_mem(io->ios2_StatData,sizeof(struct Sana2PacketTypeStats));
        else { req->io_Error=S2ERR_BAD_ARGUMENT; io->ios2_WireError=S2WERR_BAD_STATDATA; }
        break;
    case S2_GETSPECIALSTATS:
        if (io->ios2_StatData) ((struct Sana2SpecialStatHeader *)io->ios2_StatData)->RecordCountSupplied=0;
        else { req->io_Error=S2ERR_BAD_ARGUMENT; io->ios2_WireError=S2WERR_BAD_STATDATA; }
        break;
    case NSCMD_DEVICEQUERY: {
        struct NSDeviceQueryResult *r=(struct NSDeviceQueryResult *)io->ios2_Data;
        if (!r || io->ios2_DataLength < sizeof(*r)) { req->io_Error=IOERR_BADLENGTH; break; }
        r->DevQueryFormat=0; r->SizeAvailable=sizeof(*r); r->DeviceType=NSDEVTYPE_SANA2;
        r->DeviceSubType=0; r->SupportedCommands=supported_cmds; io->ios2_DataLength=sizeof(*r); break;
    }
    default: req->io_Error=IOERR_NOCMD; break;
    }
done:
    if (!(req->io_Flags & IOF_QUICK)) ReplyMsg(&req->io_Message);
}

static ULONG do_abort_io(struct Library *dev, struct IORequest *req)
{
    struct IOSana2Req *io=(struct IOSana2Req *)req; (void)dev;
    Disable();
    if (!(req->io_Flags & PENDING_BIT)) { Enable(); return IOERR_NOCMD; }
    Remove(&req->io_Message.mn_Node); req->io_Flags &= ~PENDING_BIT; req->io_Error=IOERR_ABORTED;
    Enable();
    if (!(req->io_Flags & IOF_QUICK)) ReplyMsg(&req->io_Message);
    io->ios2_WireError=0; return 0;
}

int __attribute__((no_reorder)) _start(void) { return -1; }
asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");
char device_name[]=DEVICE_NAME;
char device_id_string[]=DEVICE_ID_STRING;

static struct Library __attribute__((used)) *
init_device(struct ExecBase *sys_base asm("a6"), BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
    SysBase=sys_base; saved_seg_list=seg_list;
    dev->lib_Node.ln_Type=NT_DEVICE; dev->lib_Node.ln_Name=device_name;
    dev->lib_Flags=LIBF_SUMUSED|LIBF_CHANGED; dev->lib_Version=DEVICE_VERSION;
    dev->lib_Revision=DEVICE_REVISION; dev->lib_IdString=(APTR)device_id_string;
    is_open=FALSE; shm=NULL; vblank_added=FALSE; return dev;
}
static BPTR __attribute__((used)) expunge(struct Library *dev asm("a6")) { return do_expunge(dev); }
static void __attribute__((used)) open(struct Library *dev asm("a6"),struct IORequest *io asm("a1"),ULONG unit asm("d0"),ULONG flags asm("d1")) { do_open(dev,io,unit,flags); }
static BPTR __attribute__((used)) close(struct Library *dev asm("a6"),struct IORequest *io asm("a1")) { return do_close(dev,io); }
static void __attribute__((used)) begin_io(struct Library *dev asm("a6"),struct IORequest *io asm("a1")) { do_begin_io(dev,io); }
static ULONG __attribute__((used)) abort_io(struct Library *dev asm("a6"),struct IORequest *io asm("a1")) { return do_abort_io(dev,io); }
static ULONG device_vectors[]={ (ULONG)open,(ULONG)close,(ULONG)expunge,0,(ULONG)begin_io,(ULONG)abort_io,(ULONG)-1 };
const ULONG auto_init_tables[4]={ sizeof(struct Library),(ULONG)device_vectors,0,(ULONG)init_device };
