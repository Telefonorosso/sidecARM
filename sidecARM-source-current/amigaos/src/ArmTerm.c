/*
 * ArmTerm-v8.c - AmigaOS frontend for Linux /dev/armterm.
 * Protocol v8: fixed reserved-memory control page + two 64 KiB rings.
 * Ctrl+Esc detaches locally; plain ESC is transparent.
 */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <libraries/configvars.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>
#include <proto/keymap.h>
#include <dos/dos.h>
#include <stdio.h>
#include <string.h>

struct ExecBase *SysBase;
struct Library *KeymapBase;

#define EMU68_MANUFACTURER 0x6d73
#define ARMSERVICE_PRODUCT 0x11
#define ARMTERM_OFF         0x03b80000UL
#define CTRL_SIZE           0x00001000UL
#define RING_SIZE           0x00010000UL
#define ARMTERM_SIZE        0x00021000UL
#define ARMTERM_VERSION     8UL

#define C_VERSION      8
#define C_TOTAL_SIZE  12
#define C_FLAGS       16
#define C_IN_HEAD     20
#define C_IN_TAIL     24
#define C_OUT_HEAD    28
#define C_OUT_TAIL    32
#define C_HEARTBEAT   36
#define C_GENERATION  40
#define C_OWNER       44
#define C_OWNER_HB    48
#define C_RX_BYTES    52
#define C_TX_BYTES    56
#define C_RX_OVERRUN  60
#define C_TX_FULL     64
#define C_OPENS       68
#define C_ROWS        72
#define C_COLS        76
#define C_INFO       128

#define F_DRIVER_UP   1UL
#define F_TTY_OPEN    2UL

#ifndef IEQUALIFIER_CONTROL
#define IEQUALIFIER_CONTROL 0x0008
#endif
#define RAWKEY_ESC 0x45

static ULONG rdle32(const volatile UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1]<<8) | ((ULONG)p[2]<<16) | ((ULONG)p[3]<<24);
}
static void wrle32(volatile UBYTE *p, ULONG v)
{
    p[0]=(UBYTE)v; p[1]=(UBYTE)(v>>8); p[2]=(UBYTE)(v>>16); p[3]=(UBYTE)(v>>24);
}
static int magic8(const volatile UBYTE *p,const char *m)
{
    int i; for(i=0;i<8;i++) if(p[i]!=(UBYTE)m[i]) return 0; return 1;
}
static ULONG ring_used(ULONG head,ULONG tail) { return head-tail; }
static ULONG ring_free(ULONG head,ULONG tail)
{
    ULONG used=ring_used(head,tail);
    return used>=RING_SIZE ? 0 : RING_SIZE-used;
}
static void ring_copy_out(volatile UBYTE *ring, ULONG pos, UBYTE *dst, ULONG n)
{
    ULONG off=pos&(RING_SIZE-1), first=RING_SIZE-off;
    if(first>n) first=n;
    memcpy(dst,(const void *)(ring+off),first);
    if(first<n) memcpy(dst+first,(const void *)ring,n-first);
}
static void ring_copy_in(volatile UBYTE *ring, ULONG pos, const UBYTE *src, ULONG n)
{
    ULONG off=pos&(RING_SIZE-1), first=RING_SIZE-off;
    if(first>n) first=n;
    memcpy((void *)(ring+off),src,first);
    if(first<n) memcpy((void *)ring,src+first,n-first);
}
static void barrier(void) { __asm__ volatile ("" ::: "memory"); }

static void touch_owner(volatile UBYTE *ctrl, ULONG cookie, ULONG *hb)
{
    if(rdle32(ctrl+C_OWNER)==cookie) wrle32(ctrl+C_OWNER_HB,++(*hb));
}
static int claim_owner(volatile UBYTE *ctrl, ULONG cookie)
{
    int tries;
    for(tries=0;tries<125;tries++) {
        int ok=0;
        Forbid();
        if(rdle32(ctrl+C_OWNER)==0) {
            wrle32(ctrl+C_OWNER,cookie);
            wrle32(ctrl+C_OWNER_HB,1);
            ok=1;
        }
        Permit();
        if(ok) return 1;
        Delay(1);
    }
    return 0;
}
static void release_owner(volatile UBYTE *ctrl, ULONG cookie)
{
    Forbid();
    if(rdle32(ctrl+C_OWNER)==cookie) {
        wrle32(ctrl+C_OWNER_HB,0);
        barrier();
        wrle32(ctrl+C_OWNER,0);
    }
    Permit();
}

static ULONG drain_output(BPTR out, volatile UBYTE *ctrl, volatile UBYTE *ring,
                          ULONG cookie, ULONG *hb)
{
    UBYTE buf[512]; ULONG total=0;
    for(;;) {
        ULONG head=rdle32(ctrl+C_OUT_HEAD),tail=rdle32(ctrl+C_OUT_TAIL);
        ULONG avail=ring_used(head,tail),n;
        if(avail>RING_SIZE) { wrle32(ctrl+C_OUT_TAIL,head); break; }
        if(!avail) break;
        n=avail>sizeof(buf)?sizeof(buf):avail;
        barrier();
        ring_copy_out(ring,tail,buf,n);
        touch_owner(ctrl,cookie,hb);
        if(Write(out,buf,n)<0) break;
        wrle32(ctrl+C_OUT_TAIL,tail+n);
        total+=n;
    }
    return total;
}

static int send_bytes(volatile UBYTE *ctrl, volatile UBYTE *ring,
                      const UBYTE *src, ULONG n)
{
    ULONG head=rdle32(ctrl+C_IN_HEAD),tail=rdle32(ctrl+C_IN_TAIL);
    if(ring_free(head,tail)<n) return 0;
    ring_copy_in(ring,head,src,n);
    barrier();
    wrle32(ctrl+C_IN_HEAD,head+n);
    return 1;
}
static int send_key_byte(volatile UBYTE *ctrl, volatile UBYTE *ring, UBYTE c)
{
    UBYTE b[2]; ULONG n=1;
    if(c==0x0d) b[0]=0x0a;
    else if(c==0x08) b[0]=0x7f;
    else if(c==0x9b) { b[0]=0x1b; b[1]='['; n=2; }
    else b[0]=c;
    return send_bytes(ctrl,ring,b,n);
}

static int parse_dec_field(const UBYTE **pp, const UBYTE *end, ULONG *vp)
{
    const UBYTE *p=*pp; ULONG v=0; int digits=0;
    while(p<end && *p>='0' && *p<='9') { v=v*10+(ULONG)(*p-'0'); p++; digits=1; }
    if(!digits) return 0; *pp=p; *vp=v; return 1;
}
static int parse_raw_report(const UBYTE *b, int n, UWORD *code, UWORD *qual)
{
    const UBYTE *p=b,*end=b+n; ULONG f[8]; int i;
    if(n<18 || p>=end || *p++!=0x9b) return 0;
    for(i=0;i<8;i++) {
        if(!parse_dec_field(&p,end,&f[i])) return 0;
        if(i<7 && (p>=end || *p++!=';')) return 0;
    }
    if(p>=end || *p++!='|' || p!=end) return 0;
    if(f[0]!=1 || f[2]>0xffffUL || f[3]>0xffffUL) return 0;
    *code=(UWORD)f[2]; *qual=(UWORD)f[3]; return 1;
}
static int read_raw_report(BPTR in, UBYTE *buf, int max)
{
    int n=0; UBYTE c;
    if(!WaitForChar(in,100)) return 0;
    while(n<max) {
        if(Read(in,&c,1)!=1) return 0;
        if(n==0) { if(c!=0x9b) continue; buf[n++]=c; continue; }
        if(c==0x9b) { n=0; buf[n++]=c; continue; }
        buf[n++]=c;
        if(c=='|') return n;
        if(!WaitForChar(in,1000)) return 0;
    }
    return 0;
}

static void diag(volatile UBYTE *c)
{
    printf("\n[ArmTerm v8 diag]\n");
    printf("flags=%08lx heartbeat=%lu gen=%lu opens=%lu owner=%08lx info=%s\n",
           (unsigned long)rdle32(c+C_FLAGS),(unsigned long)rdle32(c+C_HEARTBEAT),
           (unsigned long)rdle32(c+C_GENERATION),(unsigned long)rdle32(c+C_OPENS),
           (unsigned long)rdle32(c+C_OWNER),(char *)(c+C_INFO));
    printf("IN %lu/%lu rx=%lu overrun=%lu  OUT %lu/%lu tx=%lu full=%lu\n",
           (unsigned long)rdle32(c+C_IN_HEAD),(unsigned long)rdle32(c+C_IN_TAIL),
           (unsigned long)rdle32(c+C_RX_BYTES),(unsigned long)rdle32(c+C_RX_OVERRUN),
           (unsigned long)rdle32(c+C_OUT_HEAD),(unsigned long)rdle32(c+C_OUT_TAIL),
           (unsigned long)rdle32(c+C_TX_BYTES),(unsigned long)rdle32(c+C_TX_FULL));
}

int main(void)
{
    struct Library *ExpansionBase;
    struct ConfigDev *cd;
    volatile UBYTE *base,*ctrl,*inring,*outring;
    ULONG size,cookie,hb=0;
    BPTR in,out; LONG rawok; int raw_events=0,detached=0;
    UBYTE report[160],mapped[80];

    SysBase=*(struct ExecBase **)4;
    ExpansionBase=OpenLibrary("expansion.library",0);
    if(!ExpansionBase) return 20;
    KeymapBase=OpenLibrary("keymap.library",36);
    if(!KeymapBase) { CloseLibrary(ExpansionBase); return 20; }
    cd=FindConfigDev(NULL,EMU68_MANUFACTURER,ARMSERVICE_PRODUCT);
    if(!cd) { printf("ArmTerm: ARM service board not found.\n"); goto fail; }
    base=(volatile UBYTE *)cd->cd_BoardAddr; size=(ULONG)cd->cd_BoardSize;
    if(size<ARMTERM_OFF+ARMTERM_SIZE) { printf("ArmTerm: service RAM too small.\n"); goto fail; }
    ctrl=base+ARMTERM_OFF; inring=ctrl+CTRL_SIZE; outring=inring+RING_SIZE;
    if(!magic8(ctrl,"ARMT0008") || rdle32(ctrl+C_VERSION)!=ARMTERM_VERSION ||
       rdle32(ctrl+C_TOTAL_SIZE)!=ARMTERM_SIZE || !(rdle32(ctrl+C_FLAGS)&F_DRIVER_UP)) {
        printf("ArmTerm: kernel /dev/armterm transport v8 is not ready.\n"); goto fail;
    }

    cookie=((ULONG)FindTask(NULL)) ^ ((ULONG)base) ^ 0x41544d08UL; if(!cookie) cookie=1;
    if(!claim_owner(ctrl,cookie)) {
        printf("ArmTerm: another frontend owns the console (owner=%08lx).\n",
               (unsigned long)rdle32(ctrl+C_OWNER)); goto fail;
    }

    in=Input(); out=Output(); rawok=SetMode(in,1);
    { static const UBYTE s[3]={0x9b,'1','{'}; if(Write(out,(APTR)s,3)==3) raw_events=1; }
    drain_output(out,ctrl,outring,cookie,&hb);

    for(;;) {
        int did=0,n; UWORD code,qual;
        touch_owner(ctrl,cookie,&hb);
        if(rdle32(ctrl+C_OWNER)!=cookie) { printf("\n[ArmTerm: ownership lost]\n"); break; }
        if(!(rdle32(ctrl+C_FLAGS)&F_DRIVER_UP)) { printf("\n[ArmTerm: Linux driver went down]\n"); break; }
        if(drain_output(out,ctrl,outring,cookie,&hb)) did=1;

        n=read_raw_report(in,report,sizeof(report));
        if(n>0 && parse_raw_report(report,n,&code,&qual)) {
            struct InputEvent ie; WORD m,i; did=1;
            if(((code&0x7f)==RAWKEY_ESC) && !(code&0x80) && (qual&IEQUALIFIER_CONTROL)) {
                detached=1; break;
            }
            if(code&0x80) continue;
            ie.ie_NextEvent=NULL; ie.ie_Class=IECLASS_RAWKEY; ie.ie_SubClass=0;
            ie.ie_Code=code; ie.ie_Qualifier=qual; ie.ie_EventAddress=NULL;
            m=MapRawKey(&ie,(STRPTR)mapped,sizeof(mapped),NULL);
            for(i=0;i<m;i++) {
                while(!send_key_byte(ctrl,inring,mapped[i])) {
                    if(rdle32(ctrl+C_OWNER)!=cookie || !(rdle32(ctrl+C_FLAGS)&F_DRIVER_UP)) break;
                    if(drain_output(out,ctrl,outring,cookie,&hb)) did=1;
                    Delay(1); touch_owner(ctrl,cookie,&hb);
                }
            }
        }
        if(!did) Delay(1);
    }

    if(raw_events) { static const UBYTE s[3]={0x9b,'1','}'}; Write(out,(APTR)s,3); }
    if(rawok) SetMode(in,0);
    release_owner(ctrl,cookie); diag(ctrl);
    if(detached) printf("[ArmTerm detached with Ctrl+Esc; Linux tty stays alive]\n");
    CloseLibrary(KeymapBase); CloseLibrary(ExpansionBase); return detached?0:5;
fail:
    CloseLibrary(KeymapBase); CloseLibrary(ExpansionBase); return 5;
}
