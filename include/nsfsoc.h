#ifndef NSFSOC_H
#define NSFSOC_H
/*
 * nsfsoc.h -- the protocol-independent Socket Layer (spec ch. 10).
 *
 * NSFSOC is the demultiplex point between the application world (requests,
 * blocking semantics) and the protocol world (events, callbacks). It owns:
 *   - the socket table + the SOCKCB object (one per open socket),
 *   - the (gen<<16)|id descriptor and its use-after-free (stale-fd) guard,
 *   - the PROTOPS vtable that lets one dispatch path drive UDP, TCP or a test
 *     protocol without NSFSOC knowing any of them,
 *   - the parked-request pattern (spec 10.3): the stack never blocks; a blocking
 *     request on an empty queue is PARKED on the socket and the app task stays in
 *     WAIT on its NSFRQE ecb until a protocol callback completes it,
 *   - soc_destroy: the ONE teardown checklist every close/reset/shutdown path
 *     runs (spec 10.5) -- the structural defense against the parked-request /
 *     ENQ / uncancelled-timer leak class.
 *
 * EXECUTIVE-SIDE, SINGLE-TASK. Every function here runs on the executive task,
 * run-to-completion, no locking (spec 3): the table and every SOCKCB are mainline
 * state. The only cross-task action is soc_complete's POST of the app's ecb --
 * a same-address-space plain POST (Phase 1), NOT the cross-AS wakeup machinery
 * (that is M5/Phase 2, ADR-0022). NSF stays unauthorized / problem-state.
 *
 * GENERATION LIVES IN THE TABLE SLOT, not (only) the SOCKCB (spec 10 refinement,
 * changelog v1.24). §10.5's teardown says "bump gen, mm_free" -- but a gen kept
 * solely in the SOCKCB is meaningless to bump right before freeing it, and a slot
 * reused by a later mm_alloc would collide with an old descriptor. So the stable
 * per-slot generation is the socket table's, surviving free/realloc; SOCKCB.gen
 * is a mirror stamped at alloc for the descriptor and the dump. This is what makes
 * the stale-fd guard actually reject a reused descriptor with EBADF.
 */

#include "nsf.h"
#include "nsfque.h"             /* QUEUE (rxq / acceptq embedded in SOCKCB) */
#include "nsfreq.h"             /* NSFRQE (parked requests, completion)     */
#include "nsfsts.h"             /* STSCTR (per-socket counter pointer)      */

/* Default socket table size (spec 10.2: "SOCKET pool, default 64"). */
#define NSFSOC_MAX_DEFAULT   64

/* Socket state (SOCKCB.state). Slot occupancy is the table's (slot->sock != NULL),
 * so these describe the protocol lifecycle, not allocation. */
#define SOC_ST_FREE       0     /* not a live socket                          */
#define SOC_ST_OPEN       1     /* created (attach done), not yet bound       */
#define SOC_ST_BOUND      2     /* a local name is assigned                   */
#define SOC_ST_LISTEN     3     /* accepting connections (TCP, M4)            */
#define SOC_ST_CONNECTED  4     /* connected / has a peer                     */
#define SOC_ST_CLOSING    5     /* teardown in progress                       */

/* Parked-request slot selector for soc_park. */
#define SOC_PEND_RECV     0
#define SOC_PEND_ACCEPT   1
#define SOC_PEND_CONNECT  2

/* Address family / socket type (EZASOKET/BSD numbering; stored bytes only in
 * M3-1 -- the protocol layers interpret them from M3-3). */
#define NSF_AF_INET       2
#define NSF_SOCK_STREAM   1
#define NSF_SOCK_DGRAM    2

/* Bounded-queue depths (spec 4.2, "reject rather than grow"). */
#define NSFSOC_RXQ_MAX    32    /* PBUFs held for a RECV before back-pressure */
#define NSFSOC_ACCEPTQ_MAX 8    /* listen backlog (TCP, M4)                   */

typedef struct sockcb SOCKCB;

/* Per-protocol callback table (spec 10.2). One static const instance per protocol
 * (UDP at M3-3, TCP at M4); the socket carries a pointer to it. A NULL callback
 * means "this protocol does not support that operation" -- the dispatch path maps
 * it to NSF_EOPNOTSUPP. Each callback runs on the executive task and either
 * completes its NSFRQE (soc_complete) or parks it (soc_park) -- never blocks. */
typedef struct protops {
    int (*attach) (SOCKCB *s);              /* SOCKET: bind the pcb            */
    int (*bind)   (SOCKCB *s);              /* BIND:  local name is set        */
    int (*connect)(SOCKCB *s, NSFRQE *r);   /* CONNECT                         */
    int (*listen) (SOCKCB *s, int backlog); /* LISTEN                          */
    int (*send)   (SOCKCB *s, NSFRQE *r);   /* SEND / SENDTO                   */
    int (*recv)   (SOCKCB *s, NSFRQE *r);   /* RECV / RECVFROM                 */
    int (*close)  (SOCKCB *s, NSFRQE *r);   /* CLOSE                           */
    int (*detach) (SOCKCB *s);              /* final resource release          */
    int (*accept) (SOCKCB *s, NSFRQE *r);   /* ACCEPT (TCP, M4-2)              */
} PROTOPS;
/* `accept` is the LAST member ON PURPOSE (M4-2): protocols and tests that use a
 * POSITIONAL PROTOPS initializer (UDP's g_udp_ops, the dummy PROTOPS in the M3
 * tests) omit the trailing member, which C zero-fills to NULL -- so adding accept
 * needs no edit to any non-TCP initializer, and a NULL accept maps to
 * NSF_EOPNOTSUPP through soc_dispatch exactly like the other unset ops. */

/* PROTOPS.close return convention (M4-2). A close op that has TAKEN OWNERSHIP of
 * the request -- completed it and is managing teardown itself, possibly in the
 * background (TCP: a graceful FIN, then TIME_WAIT) -- returns NSF_CLOSE_OWNED, and
 * the request dispatcher (do_close) does nothing further. Any OTHER return (0 = no
 * graceful teardown; the protocol just did local cleanup) lets do_close run the
 * default checklist: soc_destroy(s) + complete RETOK. UDP has no close op and
 * takes the default directly; a protocol WITHOUT a background phase never needs
 * the sentinel. Distinct from RETERR(-1) and every (positive) NSF_E* errno. */
#define NSF_CLOSE_OWNED   (-2)

/* The socket control block (spec 10.2). Target <=128 bytes; 72 on the S/370
 * target (SIZE_ASSERT below). Pool-allocated (SOCKET pool) at create, mm_freed by
 * soc_destroy. Addresses are UINTs with octet-1 in the MSB (the NSFIP convention).
 * `ctr` is reserved (spec 10.2: counters are global, not per-socket). */
struct sockcb {
    USHORT   id;                /*  2  @0   index into the socket table         */
    USHORT   gen;               /*  2  @2   generation mirror (slot is master)  */
    UCHAR    domain;            /*  1  @4   NSF_AF_*                             */
    UCHAR    type;              /*  1  @5   NSF_SOCK_*                           */
    UCHAR    proto;             /*  1  @6   IP protocol number                  */
    UCHAR    state;             /*  1  @7   SOC_ST_*                             */
    UINT     laddr;             /*  4  @8   local address  (UINT, octet-1 MSB)  */
    UINT     faddr;             /*  4  @12  foreign address                     */
    USHORT   lport;             /*  2  @16  local port  (host order)            */
    USHORT   fport;             /*  2  @18  foreign port                        */
    PROTOPS *ops;               /*  4  @20  the protocol vtable                 */
    void    *pcb;               /*  4  @24  TCB* / UDPPCB* (protocol-owned)     */
    QUEUE    rxq;               /* 12  @28  PBUFs ready for RECV (bounded)      */
    QUEUE    acceptq;           /* 12  @40  TCP: established, un-ACCEPTed        */
    NSFRQE  *pend_recv;         /*  4  @52  parked blocking RECV, or NULL       */
    NSFRQE  *pend_accept;       /*  4  @56  parked blocking ACCEPT              */
    NSFRQE  *pend_connect;      /*  4  @60  parked blocking CONNECT             */
    UINT     owner_ascb;        /*  4  @64  Phase-2 client identity / cleanup   */
    STSCTR  *ctr;               /*  4  @68  reserved (global counters, 10.2)    */
};                              /* 72 bytes */
NSF_SIZE_ASSERT(SOCKCB, 72);

/* asm() external-symbol aliases (CLAUDE.md 3, "External symbols"): cc370 folds an
 * external name to 8 chars after upcasing and '_'->'@', so e.g. soc_destroy and
 * soc_dispatch would both fold to SOC@DIS.../SOC@DES... collisions and ld370 would
 * silently keep one. Every export pins a unique 8-char name, scheme NSFSO*, clear
 * of NSFST* (stats) and NSFSTC (the STC):
 *   soc_reserve NSFSORSV   soc_init NSFSOINI   sock_alloc NSFSOALO
 *   sock_lookup NSFSOLKP   soc_desc NSFSODSC   soc_create NSFSOCRE
 *   soc_destroy NSFSODST   soc_dispatch NSFSODIS  soc_park NSFSOPRK
 *   soc_complete NSFSOCPL  soc_count NSFSOCNT   soc_debug_inuse NSFSODBI
 *   soc_foreach NSFSOFEA
 */

/* Create the SOCKET pool (`count` SOCKCBs, 0 => NSFSOC_MAX_DEFAULT, capped at the
 * table size). Init-window only -- calls mm_pool_create, so it runs between
 * mm_init and mm_init_complete. Returns 0 on success, non-zero if the pool could
 * not be created (the executive refuses to start). */
int      soc_reserve(UINT count) asm("NSFSORSV");

/* Reset the socket table (all slots free, generations reseeded) and register the
 * NSFSOC counters once. Safe at earliest init and idempotent; call before any
 * socket is created. */
void     soc_init(void) asm("NSFSOINI");

/* Allocate a SOCKCB into a free table slot: mm_alloc, zero, stamp id + the slot's
 * generation, init the bounded queues, publish it in the table. Returns the
 * SOCKCB*, or NULL when the table/pool is full (counted; the caller maps it to
 * NSF_EMFILE -- exhaustion is normal, never an abend). The caller sets
 * domain/type/proto/ops and calls the attach op (soc_create does both). */
SOCKCB  *sock_alloc(void) asm("NSFSOALO");

/* Resolve a descriptor to its live SOCKCB, verifying the generation: a descriptor
 * for a closed slot (slot freed) or a reused slot (generation advanced) returns
 * NULL -- the caller maps it to NSF_EBADF, never the wrong socket. This is the
 * use-after-free guard for application descriptors (spec 10.2). */
SOCKCB  *sock_lookup(UINT desc) asm("NSFSOLKP");

/* The application descriptor for `s`: (gen<<16)|id. */
UINT     soc_desc(const SOCKCB *s) asm("NSFSODSC");

/* Create a socket: sock_alloc, set domain/type/proto/ops, then run the protocol
 * attach callback. On attach failure the half-built socket is torn down
 * (soc_destroy) and NULL is returned. Returns the SOCKCB*, or NULL on table/pool
 * exhaustion or attach failure. This is the RQ_SOCKET machinery; the request
 * TRANSPORT that reaches it is NSFREQ (M3-2). */
SOCKCB  *soc_create(UCHAR domain, UCHAR type, UCHAR proto,
                    PROTOPS *ops) asm("NSFSOCRE");

/* Dispatch a request to the matching protocol callback by r->fn (BIND/CONNECT/
 * LISTEN/SEND/SENDTO/RECV/RECVFROM/CLOSE). This is pure mechanism: it invokes the
 * op and returns the op's result -- the op itself either completes r
 * (soc_complete) or parks it (soc_park); soc_dispatch never auto-completes. A
 * missing op returns NSF_EOPNOTSUPP; an unknown fn returns NSF_EINVAL. RQ_SOCKET
 * is handled by soc_create (no socket exists to dispatch on yet). */
int      soc_dispatch(SOCKCB *s, NSFRQE *r) asm("NSFSODIS");

/* Park a blocking request on the socket (spec 10.3): store r in the pend_recv /
 * pend_accept / pend_connect slot selected by `which` (SOC_PEND_*) and return to
 * the loop; the app task stays in WAIT on r->ecb until soc_complete posts it.
 * OWNERSHIP: a parked NSFRQE is owned by the socket until completed (Phase 1: the
 * app allocated it and waits on it -- NSF neither frees nor copies it). Returns 0,
 * or NSF_EINVAL if `which` is bad or that slot is already occupied. */
int      soc_park(SOCKCB *s, NSFRQE *r, UINT which) asm("NSFSOPRK");

/* Complete a request (spec 10.3): set r->retcode / r->errno_, clear whichever pend
 * slot on r's socket holds it (if any -- the immediate / non-blocking path is not
 * parked), and POST r->ecb so the WAITing app task wakes. The POST is a real
 * same-AS POST via the thread seam (nsfthr_post); see nsfsoc.c for why not the
 * evt-loop self-wake seam. After completion the app owns r again (Phase 1). */
void     soc_complete(NSFRQE *r, INT retcode, INT errno_) asm("NSFSOCPL");

/* THE teardown checklist (spec 10.5) -- never inlined ad hoc, so no close / reset
 * / shutdown / error path can skip a step: detach the pcb (cancel timers), flush
 * rxq + acceptq PBUFs, COMPLETE every parked NSFRQE with NSF_ECONNABORTED (else
 * the app task waits forever), release the pcb, bump the slot generation, and
 * mm_free the SOCKCB. Safe on a partially-built socket. */
void     soc_destroy(SOCKCB *s) asm("NSFSODST");

/* Number of sockets currently open (live table slots). */
UINT     soc_count(void) asm("NSFSOCNT");

/* Invoke `fn(s, arg)` for every live socket, in table order. The callback MAY
 * soc_destroy the socket it is handed (iteration is by ascending slot index, so
 * vacating the current slot never disturbs a slot not yet visited) -- this is
 * how NSFREQ's RQ_TERMAPI tears down every socket of one app, and how the §5.4
 * shutdown will abort all sockets. Executive-side, single-task (no locking). */
void     soc_foreach(void (*fn)(SOCKCB *s, void *arg), void *arg) asm("NSFSOFEA");

#if NSF_DEBUG
/* Leak-gate diagnostic (host tests): SOCKET-pool objects currently in use, which
 * returns to baseline after every socket is destroyed. Outside the production
 * interface (mirrors mm_debug_live_regions / buf_debug_pool). */
UINT     soc_debug_inuse(void) asm("NSFSODBI");
#endif

#endif /* NSFSOC_H */
