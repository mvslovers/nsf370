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
 *     §3 single-task storage). The read re-arm is SEQUENCED BEHIND THE WRITE
 *     PIPELINE (ADR-0025, issue #21): service only marks the release (rhold);
 *     kick POSTs returnecb when no WRITE is queued or outstanding. The pair
 *     shares one channel and a WRITE SIO issued while the blocking READ is
 *     outstanding queues at the IOS level until the next inbound frame
 *     completes that READ (live-measured: the reply's RTT tracks the sender's
 *     interval, 90 s+ for a burst tail) -- so the driver keeps the READ parked
 *     until outbound work has drained. The window is lossless: Hercules
 *     buffers/back-pressures inbound frames with no READ outstanding (§9.3,
 *     verified vs ctc_ctci.c). Ping-pong + a free-buffer queue (keeping a READ
 *     always outstanding) remains the documented throughput follow-on -- now
 *     explicitly conditional on solving the same channel serialization (HIO or
 *     an attention-driven protocol), see ADR-0025.
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
#include "nsfsts.h"             /* STSCTR (driver-private CTCI counters)      */

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

/* Read completion post codes (ADR-0027; the post code, NOT the residual, is the
 * discriminator -- Stage-0 probe test/mvs/tsthio.c). Three classes:
 *   X'7F' normal channel-end/device-end -- a data completion; decode the block.
 *   X'48' purged -- the outstanding EXCP was made available after a halt/purge
 *         (GC26-3830-4 Fig 13; residual "does not apply"). Expected when WE
 *         requested the halt to park the read; do NOT decode, count `rpurge`.
 *   anything else -- a genuine device error; count `ierr`. */
#define CTCI_POST_NORMAL   0x7Fu
#define CTCI_POST_PURGED   0x48u

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
 * over by a POST, so there is no concurrent access and no lock (§3). 128 bytes
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
    UCHAR   rhold;             /* 106  read re-arm held until writes drain
                                       (executive-only; pair sequencing,
                                       ADR-0025 -- a WRITE SIO queues behind an
                                       outstanding blocking READ)              */
    UCHAR   halting;           /* 107  IOHALT issued to park the READ for a
                                       locally-originated WRITE, not yet
                                       completed (executive-only; double-halt
                                       guard + "our purge" tag, ADR-0027)      */
    UINT    wpost;             /* 108  write completion post code (wsub-set)    */
    UINT    rucb;              /* 112  cached read-subchannel UCB address (the
                                       IOHALT target; MVS init-time, ADR-0027;
                                       0/unused on host)                       */
    STSCTR *ctr_nonip;         /* 116  non-IPv4/malformed codec drops (expected
                                       real-link traffic, NOT a device error)  */
    STSCTR *ctr_rpurge;        /* 120  purged read completions (X'48')          */
    UCHAR   rarmed;            /* 124  a READ EXCP is provably OUTSTANDING (set
                                       by read_sub after ctci_read, cleared
                                       after the completion; read_sub is the
                                       sole writer, executive read-only). kick
                                       IOHALTs to park the read ONLY when this
                                       is set -- else the halt would hit no
                                       outstanding read and stall the WRITE
                                       (issue #28, ADR-0030).                   */
} CTCIDEV;                       /* 128 bytes (rarmed + 3 pad) */
NSF_SIZE_ASSERT(CTCIDEV, 128);

/* asm() external-symbol aliases (CLAUDE.md §3), all unique across the load
 * module. The top-half entries (NSFCI + verb) match their FUNHEAD names
 * character-for-character; the codec uses NSFCK* (nsfctcif.h).
 *   top half (nsfctcio.asm / nsfctcio_host.c):
 *     ctci_scb_size NSFCISZ   ctci_open_sub NSFCIOPN   ctci_read NSFCIRD
 *     ctci_write NSFCIWR      ctci_status NSFCIST      ctci_close_sub NSFCICL
 *     ctci_halt_read NSFCIHLT
 *   channel + SVC 99 seam (nsfctci.c / nsfctci_host.c):
 *     ctci_chan_alloc NSFCIALC   ctci_chan_unalloc NSFCIUNA
 *     ctci_alloc_unit NSFCIALU   ctci_free_ddn NSFCIFDN   svc99_call NSFCISVC
 *     ctci_read_ucb NSFCIUCB
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

/* Actively park the READ by halting its outstanding EXCP (IOHALT, SVC 33;
 * ADR-0027). Issued ON THE EXECUTIVE TASK from the write-kick path when a
 * locally-originated WRITE is pending, the READ is armed, and no inbound frame
 * is in flight -- so the WRITE need not wait for unrelated inbound traffic to
 * free the shared channel. The read subtask's single-ECB wait then completes
 * with X'48' (purged) or, if a frame raced the halt, X'7F' with data -- EITHER
 * parks the READ into the ADR-0025 rhold path, then the WRITE issues and
 * returnecb re-arms. IOHALT is UCB-scoped, so it halts exactly the read
 * subchannel regardless of which TCB started the EXCP (ADR-0027's reason to
 * prefer it over a job-step PURGE). Executive-only caller, so the asm entry uses
 * one static save area (concurrency-safe -- unlike the two-subtask OPEN/EXCP/
 * CLOSE entries).
 *   rscb -- the read subchannel control block (the host shim completes ITS
 *           pending read with X'48'; unused on MVS).
 *   rucb -- the cached read UCB (the SVC 33 target; unused on host). */
void ctci_halt_read(void *rscb, UINT rucb) asm("NSFCIHLT");

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

/* Chase the read subchannel's UCB address for the IOHALT target (ADR-0027):
 * DCB+44 (DCBDEBAD) -> DEB+32 (DEBSUCBA), read byte-wise (no struct overlay on a
 * control block NSF does not own -- ADR-0024, the Stage-0 probe's discipline),
 * then sanity-check UCBNAME (UCB+13, 3 EBCDIC chars) against the device's own
 * "%03X" CUU text before trusting the computed address. `rscb` must already be
 * OPEN (OPEN sets DCBDEBAD). Returns 0 with *ucb_out set on success; non-zero
 * (nothing stored) if the name check fails -- the caller emits an NSF2xxE and
 * refuses to start the device, same policy as an allocation failure. On the host
 * a success no-op (there is no real DCB/DEB/UCB; the host halt uses rscb). */
int  ctci_read_ucb(const void *rscb, USHORT cuu, UINT *ucb_out) asm("NSFCIUCB");

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
