#ifndef NSFCTCI_H
#define NSFCTCI_H
/*
 * nsfctci.h -- the CTCI device driver (spec ch. 09.3; ADR-0019/0020/0022).
 *
 * NSFCTCI drives an emulated 3088 CTC read/write subchannel pair with plain
 * EXCP. There is NO I/O-completion exit and NO CHE appendage (ADR-0019): EXCP
 * starts the channel program and IOS POSTs the IOB ECB at channel-program
 * termination.
 *
 * COMPLETION MODEL (ADR-0022, M1-4b -- supersedes ADR-0019's WAIT premise). The
 * executive must NOT wait on the raw IOB ECB in its multi-ECB WAIT (issue #18
 * hung the STC when IOS posted it out of phase). Instead each subchannel is owned
 * by its own I/O SUBTASK (libc370 cthread, via the nsfthr seam): the subtask does
 * a single-ECB `nsfthr_wait` on the IOB ECB (the path TSTCTCM proved safe) and,
 * on completion, wakes the executive with a plain SVC-2 POST of `dev->ecb`
 * (same address space -- ADR-0022). The executive WAITs only on `dev->ecb`.
 *
 *   READ  (single-block-synchronous): the read subtask OPENs its subchannel,
 *     then loops { EXCP READ into the ONE read buffer; wait recb; store len/post;
 *     POST dev->ecb; wait returnecb }. The executive DEVIO `service` decodes the
 *     filled buffer into PBUFs (EV_PACKET_RECEIVED) ON THE EXECUTIVE TASK (§9.2,
 *     §3 single-task storage) and POSTs returnecb so the subtask reads again.
 *     No READ is outstanding only for the microseconds of the decode -- lossless,
 *     because Hercules buffers/back-pressures inbound frames with no READ
 *     outstanding (§9.3, verified vs ctc_ctci.c). Ping-pong + a free-buffer queue
 *     (keeping a READ always outstanding) is the documented throughput follow-on,
 *     deferred exactly as the CHE appendage is (ADR-0019).
 *   WRITE (one outstanding): the executive encodes a queued PBUF into the write
 *     buffer (buf_copyout stays on the executive -- the PBUF never crosses to the
 *     subtask), sets txbusy, and POSTs txgoecb; the write subtask EXCPs the WRITE
 *     and waits wecb; on completion it POSTs dev->ecb and the executive frees the
 *     PBUF exactly once (§3: only the executive frees).
 *
 * The four cooperating pieces:
 *   1. TOP HALF  (asm/nsfctcio.asm)  -- C-callable EXCP primitives (OPEN a
 *      subchannel, EXCP one raw READ/WRITE, read completion status, CLOSE).
 *      Swapped for src/nsfctcio_host.c on the host (a channel MODEL).
 *   2. CHANNEL + SVC 99 seam (src/nsfctci.c, MVS-only) -- ctci_chan_alloc /
 *      ctci_chan_unalloc: dynamically allocate / free the CUU pair (SVC 99). The
 *      OPEN/CLOSE now live on the owning SUBTASK (same TCB as its EXCP, so CLOSE
 *      purges its own outstanding EXCP at shutdown). Swapped for a no-op host stub.
 *   3. CODEC (src/nsfctcif.c) -- the CTCIHDR/CTCISEG <-> raw-IP transform.
 *   4. BOTTOM HALF (src/nsfctcib.c, portable) -- DEVOPS + the (repurposed) DEVIO
 *      seam + the two subtask functions (read_sub / write_sub). Host-testable via
 *      the nsfthr host stand-in so the SAME subtask logic runs host + MVS.
 *
 * ==============  EXCP path VALIDATED on MVS (issue #16)  ============
 * The raw EXCP READ/WRITE channel path ran live on MVSCE against a real Hercules
 * 3088 CTCI pair (CUU 500/501 on tun0): SVC 99 allocated both subchannels (two
 * distinct DDNAMEs), OPEN, WRITE post X'7F' (a crafted ICMP echo reached the host
 * in tcpdump), READ post X'7F' (length = requested - IOB residual). The SVC 99
 * unit name is 3 hex digits ("%03X") -- a 4-digit name is undefined (S99ERROR
 * 021C, ADR-0020). The READ block is ONE block of many CTCISEGs, leading hwOffset
 * = end-of-data, NO terminator to the guest (ADR-0020).
 * ==============================================================
 */

#include "nsf.h"
#include "nsfbuf.h"             /* PBUF (the in-flight WRITE buffer)          */
#include "nsfctcif.h"           /* CTCIHDR / CTCISEG / CTCI_TYPE_IPV4 (codec) */
#include "nsfevtp.h"            /* NSFECB (the completion/handoff ECB words)  */
#include "nsfthr.h"             /* NSFTHR (the I/O subtask handle)            */

/* Forward: the device abstraction types (DEVOPS/NETDEV), pulled by the bottom
 * half; declared opaquely here so a consumer that only wants the lifecycle need
 * not include nsfdev.h. */
struct devops;

/* --- Buffer / MTU bounds (spec 9.3) ---------------------------------------
 * A CTCI device I/O buffer is min(configured, 0xFFFF); 0xFFFF is only the CCW
 * count-field width, so the Hercules default 0x5000 (20 KB) is the binding
 * limit in practice. Each device owns ONE read buffer (single-block-synchronous,
 * ADR-0022) + one write buffer, obtained once at init and never freed. */
#define CTCI_BUF_DEFAULT   0x5000u    /* 20 KB (Hercules default)             */
#define CTCI_BUF_MIN       0x4000u    /* 16 KB                                */
#define CTCI_BUF_MAX       0xFFFFu    /* CCW count-field width (hard cap)     */
#define CTCI_MTU_MAX       9000u      /* Hercules discards frames above this  */

/* Read subchannel post code for a normal channel-end/device-end completion. */
#define CTCI_POST_NORMAL   0x7Fu

/* Bounded waits (100 ms ticks). A subtask polls its stop flag on each timed wait,
 * so shutdown latency is at most one interval; also the MIH-on-idle window. */
#define CTCI_POLL_TICKS    5u         /* 0.5 s subtask stop-poll / MIH interval */
#define CTCI_JOIN_TICKS    50u        /* 5 s bound to join a subtask at teardown */
#define CTCI_START_TICKS   50u        /* 5 s bound to await a subtask's OPEN     */

/* --- CTCI device state (NETDEV.priv points here) --------------------------
 * Allocated once at device start from the CTCIDEV pool (never freed until
 * shutdown). Holds the two subchannel control blocks, the two I/O buffers, the
 * two subtask handles, and the handoff ECBs/slots. Every cross-task field notes
 * its access discipline: each is written by exactly one side per phase, handed
 * over by a POST, so there is no concurrent access and no lock (§3). 108 bytes
 * on the S/370 target. */
#define CTCI_S_DOWN   0              /* allocated, not opened                  */
#define CTCI_S_UP     1              /* both subchannels opened                */

typedef struct ctcidev {
    USHORT  cuu;                /*   0  read subchannel address                */
    USHORT  wcuu;              /*   2  write subchannel address (= cuu + 1)   */
    USHORT  mtu;               /*   4  configured interface MTU               */
    USHORT  state;             /*   6  CTCI_S_*                               */
    UINT    bufsize;           /*   8  bytes per I/O buffer                    */
    void   *rscb;              /*  12  read subchannel control block (CTCISC) */
    void   *wscb;              /*  16  write subchannel control block         */
    UCHAR  *rbuf;              /*  20  the ONE read block buffer               */
    UCHAR  *wbuf;              /*  24  write block buffer                      */

    NSFTHR *rsub;              /*  28  read  subtask (owns OPEN+EXCP+CLOSE)    */
    NSFTHR *wsub;              /*  32  write subtask (owns OPEN+EXCP+CLOSE)    */

    /* read handoff: recb IOS->rsub; rlen/rpost rsub->exec; returnecb exec->rsub */
    NSFECB  recb;              /*  36  read completion  (IOS -> read subtask)  */
    NSFECB  returnecb;         /*  40  block decoded    (exec -> read subtask) */
    UINT    rlen;              /*  44  bytes read (requested - residual)       */
    UINT    rpost;             /*  48  read completion post code               */

    /* write handoff (mirrors the read handoff so the executive NEVER touches the
     * write subtask's IOB ECB, ADR-0023): wecb IOS->wsub, owned by wsub end to
     * end (wsub waits it AND reads its status); txgoecb+txlen exec->wsub;
     * wpost/wready wsub->exec (the reap keys off wready, never wecb); txpbuf
     * exec-only. Reaping on wecb directly would let the executive clear it out
     * from under wsub's wait -> a stolen completion that hangs wsub after one
     * WRITE (the live #2-write stall). */
    NSFECB  wecb;              /*  52  write completion (IOS -> write subtask)  */
    NSFECB  txgoecb;           /*  56  work ready       (exec -> write subtask) */
    UINT    txlen;             /*  60  bytes to WRITE from wbuf                 */
    PBUF   *txpbuf;            /*  64  in-flight PBUF (executive-owned)         */

    /* startup: each subtask reports its OPEN result back to ctci_op_start */
    NSFECB  rstartecb;         /*  68  read  subtask OPENed (-> executive)     */
    NSFECB  wstartecb;         /*  72  write subtask OPENed (-> executive)     */
    INT     rstartrc;          /*  76  read  subtask OPEN rc (0 = opened)      */
    INT     wstartrc;          /*  80  write subtask OPEN rc                   */

    UCHAR   rready;            /*  84  a read block is filled, awaiting decode  */
    UCHAR   txbusy;            /*  85  a WRITE is outstanding (executive-only)  */
    UCHAR   stop;              /*  86  shutdown requested (subtasks poll it)    */
    UCHAR   wready;            /*  87  a WRITE completed, awaiting reap (wsub->exec) */

    char    rddn[9];           /*  88  read  subchannel DDNAME (NUL-term)      */
    char    wddn[9];           /*  97  write subchannel DDNAME (NUL-term)      */
    char    rsvd2[2];          /* 106                                          */
    UINT    wpost;             /* 108  write completion post code (wsub-set)    */
} CTCIDEV;                       /* 112 bytes */
NSF_SIZE_ASSERT(CTCIDEV, 112);

/* asm() external-symbol aliases (CLAUDE.md §3), all unique across the load
 * module. The top-half entries (NSFCI + verb) match their FUNHEAD names
 * character-for-character; the codec uses NSFCK* (nsfctcif.h).
 *   top half (nsfctcio.asm / nsfctcio_host.c):
 *     ctci_scb_size NSFCISZ   ctci_open_sub NSFCIOPN   ctci_read NSFCIRD
 *     ctci_write NSFCIWR      ctci_status NSFCIST      ctci_close_sub NSFCICL
 *   channel + SVC 99 seam (nsfctci.c / nsfctci_host.c):
 *     ctci_chan_alloc NSFCIALC   ctci_chan_unalloc NSFCIUNA
 *     ctci_alloc_unit NSFCIALU   ctci_free_ddn NSFCIFDN   svc99_call NSFCISVC
 *   bottom half (nsfctcib.c):
 *     ctci_reserve NSFCIRSV     ctci_devops NSFCIOPS
 */

/* ============================ TOP HALF =================================== */

/* Bytes the C layer must allocate per subchannel control block (CTCISC). */
UINT ctci_scb_size(void) asm("NSFCISZ");

/* Copy the model DCB into scb, patch DDNAME, OPEN INPUT (forwrite==0) or OUTPUT
 * (forwrite!=0). Returns 0 if the DCB opened, non-zero otherwise. Called BY THE
 * OWNING SUBTASK (so the DEB is on the subtask TCB and its CLOSE purges its EXCP). */
int  ctci_open_sub(void *scb, UINT forwrite, const char *ddname8) asm("NSFCIOPN");

/* Start an inbound READ (CCW X'02'+SLI) / outbound WRITE (CCW X'01') of `len`
 * bytes to/from `buf`; the ECB is CLEARED then IOS POSTs *ecb at completion.
 * Returns 0 (started). Issued BY THE OWNING SUBTASK. */
int  ctci_read (void *scb, void *buf, UINT len, UINT *ecb) asm("NSFCIRD");
int  ctci_write(void *scb, void *buf, UINT len, UINT *ecb) asm("NSFCIWR");

/* After *ecb posts: *postcode = IOB completion code (0x7F = normal),
 * *residual = CSW residual count (bytes NOT transferred). */
void ctci_status(void *scb, UINT *postcode, UINT *residual) asm("NSFCIST");

/* CLOSE a subchannel (direction-agnostic). Returns 0. Called BY THE OWNING
 * SUBTASK, so it purges any EXCP that subtask still has outstanding. */
int  ctci_close_sub(void *scb) asm("NSFCICL");

/* ==================== CHANNEL + SVC 99 seam (MVS) ======================== */

/* Dynamically ALLOCATE the CUU pair (SVC 99): read = d->cuu, write = d->wcuu;
 * 3-hex-digit unit names; the system-returned DDNAMEs land in d->rddn/d->wddn.
 * Address-space scoped (not TCB), so it stays on the executive. Returns 0 on
 * success, non-zero on failure (an NSF2xxE is emitted, nothing left allocated).
 * On the host this is a success no-op (src/nsfctci_host.c). */
int  ctci_chan_alloc(CTCIDEV *d) asm("NSFCIALC");

/* UNALLOCATE both CUUs (SVC 99 S99VRBUN). Idempotent. Executive-side (after the
 * subtasks have CLOSEd + joined). On the host, a no-op. */
int  ctci_chan_unalloc(CTCIDEV *d) asm("NSFCIUNA");

/* --- Low-level SVC 99 seam (libc370), also a direct proof path.
 * ctci_alloc_unit dynamically allocates the device named by UNIT `unit` (a
 * 3-hex-digit CUU string, e.g. "500"), returning a generated DDNAME into `ddn8`
 * (8 chars + NUL). Returns 0 on success; on failure non-zero + fills
 * s99err / s99info from the RB99. ctci_free_ddn unallocates a DDNAME. */
int  ctci_alloc_unit(const char *unit, char *ddn8,
                     short *s99err, short *s99info) asm("NSFCIALU");
int  ctci_free_ddn(const char *ddn8) asm("NSFCIFDN");

/* The RB99 wrapper behind ctci_alloc_unit / ctci_free_ddn, exported so a test
 * can drive SVC 99 directly (both failure and success) over OUR request block. */
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
 * ops->start allocates the CTCIDEV, SVC 99 allocates the CUU pair, and creates
 * the two I/O subtasks (each OPENs its subchannel + drives its EXCP loop);
 * ops->send queues a PBUF (the executive kick encodes + hands it to the write
 * subtask); ops->shutdown stops + joins the subtasks, CLOSE having purged their
 * EXCPs, then unallocates and frees storage. */
struct devops *ctci_devops(void) asm("NSFCIOPS");

#if NSF_DEBUG
/* The NSFMM pool backing CTCI region storage (which: 0 = CTCIDEV, 1 = subchannel
 * blocks, 2 = I/O buffers), so a host test can prove device storage returns to
 * baseline after shutdown. */
struct mmpool;
struct mmpool *ctci_debug_pool(UCHAR which) asm("NSFCIDBP");
#endif

#endif /* NSFCTCI_H */
