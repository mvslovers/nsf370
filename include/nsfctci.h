#ifndef NSFCTCI_H
#define NSFCTCI_H
/*
 * nsfctci.h -- the CTCI device driver (spec ch. 09.3; ADR-0019/0020/0021).
 *
 * NSFCTCI drives an emulated 3088 CTC read/write subchannel pair with plain
 * EXCP. There is NO I/O-completion exit and NO CHE appendage: EXCP starts the
 * channel program and IOS POSTs the IOB ECB (which is the device's own
 * completion ECB in the §5.3 ECBLIST) at channel-program termination (ADR-0019).
 * The driver is built in four cooperating pieces:
 *
 *   1. TOP HALF  (asm/nsfctcio.asm) -- C-callable EXCP primitives that open a
 *      subchannel and start/decode one raw READ/WRITE. Format-blind: it moves a
 *      block of bytes and reports completion (post code + residual). Swapped for
 *      src/nsfctcio_host.c on the host build so the bottom half is host-testable.
 *
 *   2. CHANNEL + SVC 99 seam (src/nsfctci.c, MVS-only) -- ctci_chan_open/close
 *      allocate the CUU pair via the libc370 SVC 99 seam and OPEN/CLOSE both
 *      subchannels. Swapped for src/nsfctci_host.c (a success no-op) on the host,
 *      so the portable bottom half brings a device up the same way on both.
 *
 *   3. CODEC (src/nsfctcif.c) -- the CTCIHDR/CTCISEG <-> raw-IP transform
 *      (nsfctcif.h). Driver-independent, portable, host-tested with byte vectors.
 *
 *   4. BOTTOM HALF (src/nsfctcib.c, portable) -- the DEVOPS + DEVIO
 *      implementation: reserve region storage, allocate a CTCIDEV, drive the
 *      ping-pong READ (re-drive-before-parse), demux read/write completion,
 *      decode inbound segments into PBUFs (EV_PACKET_RECEIVED), and drain the
 *      sendq into WRITEs (one outstanding). It calls 1/2/3 above.
 *
 * ==============  EXCP path VALIDATED on MVS (issue #16)  ============
 * ctci_chan_open .. ctci_chan_close (then M1-3's raw EXCP) ran live on MVSCE
 * against a real Hercules 3088 CTCI pair (CUU 500/501 on tun0): SVC 99 allocated
 * both subchannels (two distinct DDNAMEs), OPEN succeeded, WRITE completed post
 * X'7F' (the crafted ICMP echo reached the host in tcpdump), READ completed post
 * X'7F' (length = requested - IOB residual). The SVC 99 unit name is 3 hex
 * digits ("%03X"): 3.8j device numbers are 3 digits, so a 4-digit name is an
 * undefined unit (S99ERROR 021C). The READ block is ONE block of many CTCISEGs
 * with the leading hwOffset = end-of-data and NO terminator sent to the guest
 * (ADR-0020) -- the M1-4 codec (nsfctcif) decodes it.
 * ==============================================================
 */

#include "nsf.h"
#include "nsfbuf.h"             /* PBUF (the in-flight WRITE buffer)          */
#include "nsfctcif.h"           /* CTCIHDR / CTCISEG / CTCI_TYPE_IPV4 (codec) */

/* Forward: the device abstraction types (DEVOPS/NETDEV), pulled by the bottom
 * half; declared opaquely here so a consumer that only wants the lifecycle need
 * not include nsfdev.h. */
struct devops;

/* --- Buffer / MTU bounds (spec 9.3) ---------------------------------------
 * A CTCI device I/O buffer is min(configured, 0xFFFF); 0xFFFF is only the CCW
 * count-field width, so the Hercules default 0x5000 (20 KB) is the binding
 * limit in practice. Each device owns TWO read buffers (ping-pong) + one write
 * buffer, obtained once at init and never freed. */
#define CTCI_BUF_DEFAULT   0x5000u    /* 20 KB (Hercules default)             */
#define CTCI_BUF_MIN       0x4000u    /* 16 KB                                */
#define CTCI_BUF_MAX       0xFFFFu    /* CCW count-field width (hard cap)     */
#define CTCI_MTU_MAX       9000u      /* Hercules discards frames above this  */

/* Read subchannel post code for a normal channel-end/device-end completion
 * (spec 9.3; the IOB ECB completion code IOS stores at IOBECBCC). */
#define CTCI_POST_NORMAL   0x7Fu

/* --- CTCI device state (NETDEV.priv points here) --------------------------
 * Allocated once at device init from the CTCIDEV pool (never freed). The two
 * subchannel control blocks (rscb/wscb) and the three I/O buffers come from
 * their own init-window pools; the two ECBs are IOS post targets, so they sit
 * here on natural (word) boundaries. 68 bytes on the S/370 target. */
#define CTCI_S_DOWN   0              /* allocated, not opened                  */
#define CTCI_S_UP     1              /* both subchannels opened                */

typedef struct ctcidev {
    USHORT  cuu;                 /*  0  read subchannel address                */
    USHORT  wcuu;               /*  2  write subchannel address (= cuu + 1)   */
    USHORT  mtu;                /*  4  configured interface MTU               */
    USHORT  state;              /*  6  CTCI_S_*                               */
    UINT    bufsize;            /*  8  bytes per I/O buffer                    */
    void   *rscb;               /* 12  read subchannel control block (CTCISC) */
    void   *wscb;               /* 16  write subchannel control block         */
    UCHAR  *rbuf0;              /* 20  ping-pong read buffer A                 */
    UCHAR  *rbuf1;              /* 24  ping-pong read buffer B                 */
    UCHAR  *wbuf;               /* 28  write buffer                            */
    UINT    cur;                /* 32  buffer the outstanding READ targets (0/1) */
    UINT    recb;               /* 36  read  completion ECB (IOS post target) */
    UINT    wecb;               /* 40  write completion ECB (IOS post target) */
    PBUF   *txflight;           /* 44  PBUF held during an in-flight WRITE     */
    char    rddn[9];            /* 48  read  subchannel DDNAME (NUL-term)      */
    char    wddn[9];            /* 57  write subchannel DDNAME (NUL-term)      */
    char    rsvd[2];            /* 66                                          */
} CTCIDEV;                       /* 68 bytes */
NSF_SIZE_ASSERT(CTCIDEV, 68);

/* asm() external-symbol aliases (CLAUDE.md §3), all unique across the load
 * module. The top-half entries (NSFCI + verb) match their FUNHEAD names
 * character-for-character; the codec uses NSFCK* (nsfctcif.h).
 *   top half (nsfctcio.asm / nsfctcio_host.c):
 *     ctci_scb_size NSFCISZ   ctci_open_sub NSFCIOPN   ctci_read NSFCIRD
 *     ctci_write NSFCIWR      ctci_status NSFCIST      ctci_close_sub NSFCICL
 *   channel + SVC 99 seam (nsfctci.c / nsfctci_host.c):
 *     ctci_chan_open NSFCICHO   ctci_chan_close NSFCICHC
 *     ctci_alloc_unit NSFCIALU  ctci_free_ddn NSFCIFDN  svc99_call NSFCISVC
 *   bottom half (nsfctcib.c):
 *     ctci_reserve NSFCIRSV     ctci_devops NSFCIOPS
 */

/* ============================ TOP HALF =================================== */

/* Bytes the C layer must allocate per subchannel control block (CTCISC). */
UINT ctci_scb_size(void) asm("NSFCISZ");

/* Copy the model DCB into scb, patch DDNAME, OPEN INPUT (forwrite==0) or OUTPUT
 * (forwrite!=0). Returns 0 if the DCB opened, non-zero otherwise. */
int  ctci_open_sub(void *scb, UINT forwrite, const char *ddname8) asm("NSFCIOPN");

/* Start an inbound READ (CCW X'02'+SLI) / outbound WRITE (CCW X'01') of `len`
 * bytes to/from `buf`; the ECB is CLEARED then IOS POSTs *ecb at completion.
 * Returns 0 (started). */
int  ctci_read (void *scb, void *buf, UINT len, UINT *ecb) asm("NSFCIRD");
int  ctci_write(void *scb, void *buf, UINT len, UINT *ecb) asm("NSFCIWR");

/* After *ecb posts: *postcode = IOB completion code (0x7F = normal),
 * *residual = CSW residual count (bytes NOT transferred). */
void ctci_status(void *scb, UINT *postcode, UINT *residual) asm("NSFCIST");

/* CLOSE a subchannel (direction-agnostic). Returns 0. */
int  ctci_close_sub(void *scb) asm("NSFCICL");

/* ==================== CHANNEL + SVC 99 seam (MVS) ======================== */

/* Bring the CUU pair up: SVC 99 allocate both subchannels (3-hex-digit unit
 * names from d->cuu / d->wcuu, generated DDNAMEs into d->rddn / d->wddn), then
 * OPEN read INPUT and write OUTPUT. Returns 0 on success (subchannels open), or
 * non-zero on any failure (an NSF2xxE message is emitted and nothing is left
 * allocated). On the host this is a success no-op (src/nsfctci_host.c). */
int  ctci_chan_open(CTCIDEV *d) asm("NSFCICHO");

/* CLOSE both subchannels and unallocate both CUUs (SVC 99 S99VRBUN). Idempotent
 * on a device that never opened. On the host, a no-op. */
int  ctci_chan_close(CTCIDEV *d) asm("NSFCICHC");

/* --- Low-level SVC 99 seam (libc370), also TSTCTCM's direct proof path.
 * ctci_alloc_unit dynamically allocates the device named by UNIT `unit` (a
 * 3-hex-digit CUU string, e.g. "500"), asking the system to return a generated
 * DDNAME into `ddn8` (8 chars + NUL). Returns 0 on success; on failure returns
 * non-zero and fills *s99err / *s99info from the RB99 (S99ERROR / S99INFO).
 * ctci_free_ddn unallocates a DDNAME. Either s99err/s99info may be NULL. */
int  ctci_alloc_unit(const char *unit, char *ddn8,
                     short *s99err, short *s99info) asm("NSFCIALU");
int  ctci_free_ddn(const char *ddn8) asm("NSFCIFDN");

/* The RB99 wrapper behind ctci_alloc_unit / ctci_free_ddn, exported so a test
 * can drive SVC 99 directly (both failure and success) over OUR request-block
 * construction. `txt99` is a built text-unit array (a libc370 `TXT99 **`, passed
 * opaquely as void *); svc99_call marks its last entry, issues SVC 99 with
 * `request` (S99VRBAL / S99VRBUN / ...), and on a successful allocate copies the
 * returned DDNAME into `ddn8`. On failure it fills *s99err / *s99info. Returns
 * the SVC 99 rc. */
int  svc99_call(void *txt99, unsigned char request, char *ddn8,
                short *s99err, short *s99info) asm("NSFCISVC");

/* ========================= BOTTOM HALF (DEVOPS) ========================== */

/* Reserve NSFMM region storage for up to `ndev` CTCI devices with `bufsize`
 * I/O buffers. INIT-WINDOW ONLY (creates pools; call between mm_init and
 * mm_init_complete). Returns 0 on success, non-zero if a pool could not be
 * created. */
int  ctci_reserve(UINT ndev, UINT bufsize) asm("NSFCIRSV");

/* The DEVOPS vtable for the CTCI driver, to hand dev_register with a DEVCFG
 * whose cuu/mtu/ipaddr/name come from the PROFILE DEVICE/LINK/HOME statements.
 * ops->init allocates the CTCIDEV (from the reserved pools) and attaches the
 * DEVIO seam (ADR-0021); ops->start opens the channel and drives the first READ;
 * ops->send queues a PBUF (the executive loop's kick transmits it); ops->shutdown
 * closes the channel and frees any in-flight WRITE PBUF. */
struct devops *ctci_devops(void) asm("NSFCIOPS");

#if NSF_DEBUG
/* The NSFMM pool backing CTCI region storage (which: 0 = CTCIDEV, 1 = subchannel
 * blocks, 2 = I/O buffers), so a host test can prove device storage returns to
 * baseline after shutdown. Outside the production interface (mirrors
 * buf_debug_pool). */
struct mmpool;
struct mmpool *ctci_debug_pool(UCHAR which) asm("NSFCIDBP");
#endif

#endif /* NSFCTCI_H */
