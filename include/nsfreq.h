#ifndef NSFREQ_H
#define NSFREQ_H
/*
 * nsfreq.h -- NSFRQE, the application<->stack request block (spec ch. 10.4).
 *
 * PHASE-BOUNDARY CONTRACT -- FROZEN AT THE END OF M3; CHANGING IT NEEDS AN ADR.
 *
 * NSFRQE is the ONE structure that crosses the boundary between the application
 * world (EZASOKET calls, blocking semantics) and the stack. It is deliberately
 * transport-independent so the two deployment phases share it byte-for-byte:
 *   Phase 1 (M0-M4): the stack is an ATTACHed subtask in the app's address
 *                    space -- the request travels via an in-address-space queue
 *                    + POST, and `ubuf` is a plain same-space pointer.
 *   Phase 2 (M5+):   the stack is its own subsystem address space (NSFS) -- the
 *                    request travels via SSI/SVC with a cross-memory POST, and
 *                    `ubuf`/`ulen` describe a KEYED cross-memory move (MVCK /
 *                    MVCSK) out of the client's storage.  That move is gated by
 *                    the CALLER IDENTITY the transport records in its own slot,
 *                    never by anything in this block or in the SOCKCB -- see
 *                    M5-2c1, which renamed SOCKCB.owner_ascb to `apptok`
 *                    because it has only ever held an app-instance token.
 * Protocols and sockets never learn which transport delivered the block, so the
 * Phase-2 fields (`ubuf`, `ulen`, and the SOCKCB `apptok`) are defined NOW
 * even though Phase 1 uses `ubuf` only as a same-space pointer -- the layout must
 * not have to change when Phase 2 lands (spec 10.4).
 *
 * NSFREQ (M3-2) is the Request Manager -- the Phase-1 transport (an
 * in-address-space request queue + requestECB) plus the fn dispatcher that
 * receives, validates and drives NSFRQEs through NSFSOC. Its manager API is
 * declared at the end of this header (with 8-char asm() aliases, CLAUDE.md 3);
 * the NSFRQE contract itself needs none (pure layout).
 */

#include "nsf.h"
#include "nsfque.h"             /* QELEM (queue linkage embedded in NSFRQE) */

struct protops;                 /* fwd decl: nsfreq_register_proto (nsfsoc.h) */

/* Request function codes (NSFRQE.fn). The M3 set is INITAPI/SOCKET/BIND/SENDTO/
 * RECVFROM/CLOSE/TERMAPI/GETSOCKNAME (spec 15.2); the remaining codes are defined
 * now so the dispatch switch and the frozen vocabulary are complete -- fn VALUES
 * are not part of the frozen LAYOUT, but keeping them stable avoids churn. */
enum {
    RQ_NONE        = 0,
    RQ_INITAPI     = 1,
    RQ_SOCKET      = 2,
    RQ_BIND        = 3,
    RQ_CONNECT     = 4,
    RQ_LISTEN      = 5,
    RQ_ACCEPT      = 6,
    RQ_SEND        = 7,
    RQ_SENDTO      = 8,
    RQ_RECV        = 9,
    RQ_RECVFROM    = 10,
    RQ_CLOSE       = 11,
    RQ_SHUTDOWN    = 12,
    RQ_SELECT      = 13,
    RQ_GETSOCKNAME = 14,
    RQ_GETPEERNAME = 15,
    RQ_SETSOCKOPT  = 16,
    RQ_GETSOCKOPT  = 17,
    RQ_FCNTL       = 18,
    RQ_TERMAPI     = 19
};

/* Request flags (NSFRQE.flags). The non-blocking bit lives here rather than on
 * the SOCKCB: SOCKCB stays spec-exact (10.2 has no flags field), and every
 * request already carries its own mode. FCNTL/FIONBIO (which would make it a
 * persistent socket attribute) is M4 (spec 15.2); until then a caller sets the
 * bit per request and the protocol completes immediately with EWOULDBLOCK rather
 * than parking (spec 10.3). */
#define RQ_F_NONBLOCK   0x0001u

/* EZASOKET ERRNO values placed in NSFRQE.errno_ on a failed completion. These
 * follow the IBM EZASOKET / classic-BSD numbering (spec 15.2: "not Unix errno.h
 * values where they differ"), matching libc370 <errno.h> -- but NSF cannot just
 * include <errno.h>, because the host test build's native <errno.h> carries the
 * PLATFORM's values (Linux ENOBUFS 105, ESHUTDOWN 108, ...), which differ from
 * the target's. So the contract values are pinned here, NSF_-prefixed to never
 * collide with a <errno.h> macro, and are used through these names EVERYWHERE so
 * no test hardcodes a number. The values are PROVISIONAL for M3-1 -- the frozen
 * NSFRQE contract is the LAYOUT, not errno_ (a plain INT); the authoritative
 * reconciliation is docs/ezasoket-conformance.md, the M6 acceptance artifact. */
#define NSF_EBADF            9      /* stale / invalid socket descriptor        */
#define NSF_EINVAL          22      /* invalid argument / unsupported request   */
#define NSF_EMFILE          24      /* socket table full (no free descriptor)   */
#define NSF_EWOULDBLOCK     35      /* == EAGAIN: op would block, non-blocking   */
#define NSF_EINPROGRESS     36      /* non-blocking CONNECT started (M4-5)       */
#define NSF_EDESTADDRREQ    39      /* SEND with no peer (UDP: SENDTO required)  */
#define NSF_EMSGSIZE        40      /* datagram larger than the interface MTU   */
#define NSF_EPROTONOSUPPORT 43      /* protocol not supported                   */
#define NSF_EOPNOTSUPP      45      /* operation not supported on this socket   */
#define NSF_EAFNOSUPPORT    47      /* address family not supported             */
#define NSF_EADDRINUSE      48      /* local (addr,port) already bound          */
#define NSF_ECONNABORTED    53      /* socket torn down under a parked request  */
#define NSF_ECONNRESET      54      /* connection reset by peer (RST, M4-2)     */
#define NSF_ENOBUFS         55      /* buffer/pool exhaustion (drop, not abend)  */
#define NSF_EISCONN         56      /* CONNECT on an already-connected socket    */
#define NSF_ENOTCONN        57      /* socket is not connected                  */
#define NSF_ESHUTDOWN       58      /* stack is shutting down                   */
#define NSF_ETIMEDOUT       60      /* connection timed out before completing    */
#define NSF_ECONNREFUSED    61      /* connection refused (RST in SYN_SENT, M4-2)*/
#define NSF_EHOSTUNREACH    65      /* no route to the datagram's destination   */
/* NO NSF_ENOSYS: M3-2 used 78 for the not-implemented stub verbs (SELECT/
 * SET|GETSOCKOPT/FCNTL/GETPEERNAME), but IBM Table 67 has no ENOSYS and 78 is
 * EDEADLK (docs/ezasoket-conformance.md 4). Per ADR-0029 the stub verbs now
 * complete with NSF_EOPNOTSUPP (45) -- the classic "operation not supported"
 * value the M6 relink audit expects. Do NOT reintroduce 78 as ENOSYS. */

/* EZASOKET RETCODE convention: 0 (or a byte count) on success, -1 on error with
 * errno_ set (spec 15.1). */
#define NSF_RETOK       0
#define NSF_RETERR    (-1)

/* SETSOCKOPT/GETSOCKOPT level + option names NSF honours (M4-5, BSD numbering,
 * docs/ezasoket-conformance.md §3). The request carries the level in p1, the
 * option in p2, and the integer optval in p3 (SET) / returns it in p3 (GET).
 * Everything outside this minimal set completes NSF_EOPNOTSUPP with the option in
 * the trace. NSF_SO_DFLT_BUF is the fixed byte size GET reports for SO_RCVBUF /
 * SO_SNDBUF -- it mirrors the TCP receive window / send budget (both 4096;
 * NSFTCP_RCVWND_DEFAULT / NSFTCP_SNDBUF), fixed until TCPCONFIG makes them tunable
 * (M5). */
#define NSF_SOL_SOCKET     0xFFFF   /* level: socket-level options              */
#define NSF_SO_REUSEADDR   0x0004   /* accepted (TIME_WAIT bind); no v1 effect  */
#define NSF_SO_SNDBUF      0x1001   /* GET: the send budget                     */
#define NSF_SO_RCVBUF      0x1002   /* GET: the receive window                  */
#define NSF_IPPROTO_TCP    6        /* level: TCP options                       */
#define NSF_TCP_NODELAY    0x0001   /* accepted no-op (no Nagle until M5)       */
#define NSF_SO_DFLT_BUF    4096

/* The request block (spec 10.4). 64-byte core on the S/370 target; the request
 * pool object size is 96 (NSFRQE_OBJSIZE) so the block has room to grow inside a
 * pooled object without disturbing the frozen 64-byte core -- the Phase-2 SSI
 * transport (M5) that copies a client request into an NSF-side pool object uses
 * that objsize. Every multi-byte field is a project fixed-width type; on the host
 * `ubuf` is 8 bytes wide, so sizeof(NSFRQE) differs there -- which is exactly why
 * NSF_SIZE_ASSERT only fires under __MVS__ (nsf.h). */
typedef struct nsfrqe {
    QELEM   q;                  /*  8  @0   queue linkage (request queue)        */
    UCHAR   eye[4];             /*  4  @8   "RQE " eyecatcher                    */
    USHORT  fn;                 /*  2  @12  RQ_* function code                   */
    USHORT  flags;              /*  2  @14  RQ_F_* (RQ_F_NONBLOCK)               */
    UINT    sockdesc;           /*  4  @16  (gen<<16)|id -- the target socket    */
    void   *ubuf;               /*  4  @20  user buffer (P1 same-space pointer;  */
    UINT    ulen;               /*  4  @24   P2 keyed cross-memory move length)  */
    UINT    p1;                 /*  4  @28  fn-specific (addr / backlog / ...)   */
    UINT    p2;                 /*  4  @32  fn-specific (port / option / ...)    */
    UINT    p3;                 /*  4  @36  fn-specific                          */
    INT     retcode;            /*  4  @40  EZASOKET RETCODE (0/count or -1)     */
    INT     errno_;             /*  4  @44  EZASOKET ERRNO (NSF_E*); '_' dodges  */
                                /*          the <errno.h> `errno` macro          */
    UINT    ecb;                /*  4  @48  completion ECB (app WAITs on it)     */
    UINT    reqid;              /*  4  @52  trace correlation id                 */
    UINT    apptok;             /*  4  @56  app-instance token: RQ_INITAPI writes */
                                /*          it (output); RQ_SOCKET / RQ_TERMAPI  */
                                /*          carry it (input). Defined out of the */
                                /*          @56 reserved word at M3-2 -- the     */
                                /*          64-byte core is UNCHANGED, so the    */
                                /*          M3 freeze holds (spec 10.4). It is   */
                                /*          the SAME value SOCKCB.apptok holds,  */
                                /*          which is what scopes a socket to its */
                                /*          app -- NOT an ASCB (M5-2c1)          */
    UINT    rsvd;               /*  4  @60  reserved -> 64-byte frozen core      */
} NSFRQE;                       /* 64 bytes */
NSF_SIZE_ASSERT(NSFRQE, 64);

/* Request eyecatcher and the pooled-object size (see the struct comment). */
#define NSFRQE_EYE       "RQE "
#define NSFRQE_OBJSIZE   96

/* ==========================================================================
 * NSFREQ -- the Request Manager (M3-2, spec 10.1 / 10.4).
 *
 * Phase-1 transport: an in-address-space request queue (the NSFXQ handoff, so
 * app subtasks on other TCBs enqueue CS-safely) + a requestECB that the
 * executive WAITs in its ECBLIST (spec 5.3). App tasks own their NSFRQE storage
 * (same-space); NSF never allocates or copies it in Phase 1. Ownership across
 * the round-trip: the app builds an NSFRQE and OWNS it, submits it (the request
 * queue now references it), then WAITs on r->ecb; the executive/handler READ it
 * and may complete it (soc_complete POSTs r->ecb); after that POST the app owns
 * it again and the executive must not touch it. `ubuf` is a plain same-space
 * pointer in Phase 1 (no copy). Phase 2 (M5) swaps ONLY the transport (SSI/SVC,
 * cross-memory POST, keyed ubuf move) -- the NSFRQE format never changes.
 *
 * asm() external-symbol aliases (CLAUDE.md 3, "External symbols"): cc370 folds
 * an external name to 8 chars, so every export pins a unique 8-char name, scheme
 * NSFRQ*, clear of NSFRQE (a struct, no symbol):
 *   nsfreq_init NSFRQINI   nsfreq_submit NSFRQSUB   nsfreq_wait NSFRQWT
 *   nsfreq_call NSFRQCAL   nsfreq_dispatch NSFRQDSP nsfreq_drain NSFRQDRN
 *   nsfreq_pending NSFRQPND nsfreq_ecb NSFRQECB
 *   nsfreq_register_proto NSFRQRPT  nsfreq_register_select NSFRQRSL
 *   nsfreq_set_transport NSFRQSXT
 *   nsfreq_dispatch_id NSFRQDSI     nsfreq_app_info NSFRQAPI
 *   nsfreq_app_max NSFRQAPM       nsfreq_set_classifier NSFRQSCL
 *   nsfreq_app_classify NSFRQACL
 * ========================================================================== */

/* Reset the request transport (empty the queue, clear requestECB) and the app
 * registry + protocol table. Safe at earliest init and idempotent; no pool
 * (Phase 1 allocates nothing -- the app owns its NSFRQE). Call before any
 * request is submitted or dispatched. */
void     nsfreq_init(void) asm("NSFRQINI");

/* Register the PROTOPS a socket of IP protocol `proto` gets at RQ_SOCKET
 * (spec 10.2). M3-3 registers UDP (17); a test registers its dummy protocol.
 * Returns 0 on success, non-zero if the small protocol table is full. */
int      nsfreq_register_proto(UCHAR proto, struct protops *ops) asm("NSFRQRPT");

/* Register the RQ_SELECT handler (M4-5, ADR-0035). SELECT is one request over N
 * sockets -- its own engine (NSFSEL) rather than a per-socket PROTOPS op -- so
 * NSFREQ routes RQ_SELECT to the handler registered here if present, else completes
 * it NSF_EOPNOTSUPP (the M3-2 behaviour, so a build without NSFSEL is unchanged).
 * The handler either completes r (immediately ready / poll / timeout) or parks it
 * in the SELECT engine; it never leaves r hanging. `fn` NULL unregisters. */
void     nsfreq_register_select(void (*fn)(NSFRQE *r)) asm("NSFRQRSL");

/* The requestECB the executive adds to its ECBLIST (spec 5.3). Owned by NSFREQ;
 * nsfreq_drain resets it before draining (reset-before-WAIT, ADR-0022). */
UINT    *nsfreq_ecb(void) asm("NSFRQECB");

/* APP SIDE (may run on another TCB). Submit an NSFRQE: enqueue it CS-safely and
 * POST the requestECB via the thread seam (a real SVC 2 POST on MVS) so a
 * WAITing executive wakes. ONE POST call site -> M5 swaps it for the cross-AS
 * seam. The app then WAITs on r->ecb (nsfreq_wait); it must keep r alive and
 * not touch it again until the wait returns. */
/* Install a Phase-2 transport (ADR-0041).  NULL restores the Phase-1 same-
 * address-space path, which is also the state nsfreq_init leaves.  A program
 * that never calls this is byte-for-byte unaffected -- the registration-seam
 * idiom, not a mode switch.  See include/nsfreqc.h for the client side. */
void     nsfreq_set_transport(void (*fn)(NSFRQE *r)) asm("NSFRQSXT");

void     nsfreq_submit(NSFRQE *r) asm("NSFRQSUB");

/* APP SIDE. Block the calling task on r->ecb until the executive completes r. */
void     nsfreq_wait(NSFRQE *r) asm("NSFRQWT");

/* APP SIDE convenience: submit + wait (the whole blocking round-trip). */
void     nsfreq_call(NSFRQE *r) asm("NSFRQCAL");

/* EXECUTIVE SIDE. Drain the request queue and dispatch each request. Resets the
 * requestECB BEFORE taking the queue, then double-checks (drain; dispatch; loop
 * while the queue is non-empty) so a request enqueued in the reset window is
 * never lost (ADR-0022; the exact class of the #27 flake and the spec 5.3 WAIT
 * warning). Called once per loop pass by the executive (evt_set_request). */
void     nsfreq_drain(void) asm("NSFRQDRN");

/* EXECUTIVE SIDE. Side-effect-free probe: is a request queued? The loop rechecks
 * it before committing to WAIT so a submit racing the ECB reset is serviced on
 * the same pass, not parked (ADR-0025, mirroring nsfdev_work_pending). */
int      nsfreq_pending(void) asm("NSFRQPND");

/* EXECUTIVE SIDE. Dispatch ONE request by r->fn (spec 10.4 verb set). The
 * protocol-independent verbs (INITAPI/TERMAPI/SOCKET/BIND/GETSOCKNAME/CLOSE) are
 * handled here; the socket-protocol verbs (CONNECT/LISTEN/ACCEPT/SEND/SENDTO/
 * RECV/RECVFROM/SHUTDOWN) delegate to soc_dispatch (the protocol op completes or
 * parks r); the M3-2-unimplemented verbs (SELECT/SET|GETSOCKOPT/FCNTL/
 * GETPEERNAME) complete r with NSF_EOPNOTSUPP; an unknown fn completes r with
 * NSF_EINVAL. Every non-parked path completes r exactly once (the app always
 * wakes); a parked request (soc_park) is completed later by a protocol callback
 * or by soc_destroy. Exposed for the direct-call tests. */
void     nsfreq_dispatch(NSFRQE *r) asm("NSFRQDSP");

/* The same dispatch, told WHO asked (M5-2c1).
 *
 * `caller_ascb` / `caller_asid` identify the address space the request came
 * from.  RQ_INITAPI records the pair against the new app instance and nothing
 * else looks at it, so every other verb behaves identically to nsfreq_dispatch.
 * The pair is what a later reclamation step classifies through
 * nsfreqx_classify: an ASCB alone cannot survive ASID reuse.
 *
 * WHERE IT COMES FROM, and this is the load-bearing part: the TRANSPORT, never
 * the request.  In Phase 2 the SVC routine records the caller's ASCB and ASID
 * in its own CSA slot at the claim, before any client data is staged, and the
 * STC passes that pair down to here.  A request-supplied identity is exactly
 * what a liveness guard must never trust, and keeping it out of the NSFRQE is
 * why the block stays frozen at 64 bytes (ADR-0040, ADR-0041).
 *
 * PHASE 1 PASSES ZERO, and that is a supported value, not a degenerate one:
 * there is no transport and no caller address space, so there is no identity to
 * record.  nsfreq_dispatch is exactly this call with (0, 0), so the Phase-1
 * path and every direct-call test are unchanged.  A zero ASCB means "no
 * identity recorded" and every consumer must treat it as unclassifiable rather
 * than feeding it to the classifier. */
void     nsfreq_dispatch_id(NSFRQE *r, UINT caller_ascb,
                            UINT caller_asid) asm("NSFRQDSI");

/* Read one app-registry slot: 1 and *out filled when `idx` is in use, else 0.
 * `idx` runs 0 .. NSFREQ_APP_MAX-1 (see nsfreq_app_max); any out-of-range index
 * is 0, not an error.  Every out-parameter is optional.
 *
 * The registry is static and private on purpose; this is the one read window,
 * so the operator diagnostic and the tests observe exactly what the dispatcher
 * stored rather than each keeping a shadow copy of it. */
int      nsfreq_app_info(UINT idx, UINT *token, UINT *ascb,
                         UINT *asid) asm("NSFRQAPI");

/* How many app-registry slots exist (the bound for nsfreq_app_info). */
UINT     nsfreq_app_max(void) asm("NSFRQAPM");

/* Register the client-liveness classifier (M5-2c1).  `fn` takes the caller
 * identity recorded at RQ_INITAPI and returns one of NSFREQX_CL_LIVE / _DEAD /
 * _UNKNOWN; on MVS the Phase-2 STC registers a wrapper around the ASVT lookup
 * of ADR-0040.  NULL is the default and the Phase-1 state: no transport, so no
 * address space to ask about.
 *
 * It is a seam rather than a direct call because the ASVT exists only on MVS,
 * while the app registry and everything that consumes a verdict are portable
 * and host-tested. */
void     nsfreq_set_classifier(int (*fn)(UINT ascb,
                                         UINT asid)) asm("NSFRQSCL");

/* Classify the app instance in slot `idx`.  Returns the registered
 * classifier's own verdict (NSFREQX_CL_LIVE / _DEAD / _UNKNOWN, which are
 * 0 / 1 / 2), or one of the two answers that need no classifier:            */
#define NSFREQ_APPCL_FREE   (-1)    /* the slot holds no app instance        */
#define NSFREQ_APPCL_NONE   (-2)    /* nothing to ask about, or nobody to ask */
/*
 * NSFREQ_APPCL_NONE IS THE RED LINE, AND IT IS ENFORCED HERE SO NO CONSUMER
 * HAS TO REMEMBER IT.  A slot whose caller ASCB is zero -- every Phase-1 slot,
 * because nsfreq_dispatch supplies no identity -- is NOT handed to the
 * classifier at all.  Passing it would be asking "is address space 0 alive?",
 * a question with no true answer, and answering it either way is a bug: LIVE
 * would be a fabricated liveness claim, DEAD would authorise tearing down the
 * sockets of a perfectly healthy Phase-1 application.  The same answer covers
 * "no classifier registered", because both mean the verdict has no evidence
 * behind it -- which is exactly what ADR-0040's UNKNOWN row exists to keep
 * separate from a verdict that does. */
int      nsfreq_app_classify(UINT idx) asm("NSFRQACL");

#endif /* NSFREQ_H */
