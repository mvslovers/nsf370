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
 *                    MVCSK) out of the client's storage, gated by `owner_ascb`
 *                    on the owning SOCKCB (spec 10.2).
 * Protocols and sockets never learn which transport delivered the block, so the
 * Phase-2 fields (`ubuf`, `ulen`, and the SOCKCB `owner_ascb`) are defined NOW
 * even though Phase 1 uses `ubuf` only as a same-space pointer -- the layout must
 * not have to change when Phase 2 lands (spec 10.4).
 *
 * This header carries NO functions (the Request Manager NSFREQ -- the transport
 * that receives, validates and drives NSFRQEs -- is M3-2); it is pure layout +
 * the request/completion vocabulary, so it needs no asm() aliases (CLAUDE.md 3).
 */

#include "nsf.h"
#include "nsfque.h"             /* QELEM (queue linkage embedded in NSFRQE) */

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
#define NSF_EPROTONOSUPPORT 43      /* protocol not supported                   */
#define NSF_EOPNOTSUPP      45      /* operation not supported on this socket   */
#define NSF_EAFNOSUPPORT    47      /* address family not supported             */
#define NSF_ECONNABORTED    53      /* socket torn down under a parked request  */
#define NSF_ENOBUFS         55      /* buffer/pool exhaustion (drop, not abend)  */
#define NSF_ESHUTDOWN       58      /* stack is shutting down                   */

/* EZASOKET RETCODE convention: 0 (or a byte count) on success, -1 on error with
 * errno_ set (spec 15.1). */
#define NSF_RETOK       0
#define NSF_RETERR    (-1)

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
    UINT    rsvd[2];            /*  8  @56  reserved -> 64-byte frozen core      */
} NSFRQE;                       /* 64 bytes */
NSF_SIZE_ASSERT(NSFRQE, 64);

/* Request eyecatcher and the pooled-object size (see the struct comment). */
#define NSFRQE_EYE       "RQE "
#define NSFRQE_OBJSIZE   96

#endif /* NSFREQ_H */
