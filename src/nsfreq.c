/*
 * nsfreq.c -- the Request Manager NSFREQ (see nsfreq.h, spec ch. 10.1 / 10.4).
 *
 * The backbone the socket API rides on: it moves NSFRQE request blocks from the
 * application world to the executive, dispatches each by fn to the socket layer
 * (NSFSOC), and completes it (retcode/errno + POST). M3-1's NSFSOC is the object
 * model this drives.
 *
 * PHASE 1 (M0-M4): same address space, problem state. The stack is an ATTACHed
 * subtask in the app's address space, so the transport is an in-address-space
 * queue + POST and `ubuf` is a plain same-space pointer. App tasks (same-AS
 * subtasks on other TCBs) are producers; the single executive task is the sole
 * consumer -- so enqueue is CS-safe (the NSFXQ handoff, exactly as the async
 * device I/O exit hands work up) and completion wakes the app with a real SVC 2
 * POST (nsfthr_post -- the SAME seam soc_complete uses). NO cross-AS machinery
 * here (that is Phase 2 / M5, ADR-0022); ONE POST call site so M5 swaps it.
 *
 * EXECUTIVE-SIDE dispatch is single-task, run-to-completion, no locking (spec 3):
 * every soc_* call and every handler below runs on the executive task. The only
 * cross-task points are the CS-safe xq_push (app -> queue) and the two POSTs
 * (app -> requestECB, executive -> r->ecb).
 */
#include "nsfreq.h"
#include "nsfsoc.h"             /* SOCKCB, PROTOPS, soc_* (the object model)  */
#include "nsfxq.h"              /* XQ: the CS-safe app -> executive handoff    */
#include "nsfthr.h"             /* nsfthr_post / nsfthr_wait (the POST seam)   */
#include "nsfevtp.h"            /* NSFECB (requestECB + the app completion ecb)*/
#include "nsftrc.h"

/* ---- transport: request queue + requestECB --------------------------------
 * g_reqxq is the CS-safe MPSC handoff (producers = app subtasks; consumer = the
 * executive). g_reqecb is the spec-5.3 requestECB the executive WAITs on. */
static XQ     g_reqxq;
static NSFECB g_reqecb;

/* ---- app registry (RQ_INITAPI / RQ_TERMAPI scoping) ------------------------
 * A fixed table of app instances. Each RQ_SOCKET stamps the new socket's
 * owner_ascb with the requesting app's token; RQ_TERMAPI destroys every socket
 * carrying that token (the mass-teardown path) and frees the slot. The token is
 * (gen<<16)|idx like a socket descriptor, so a stale token never matches a
 * reused slot. Static -- no pool, no runtime allocation (spec 3). */
#define NSFREQ_APP_MAX   16

typedef struct appreg {
    UCHAR  inuse;
    USHORT gen;                         /* bumped on free -> stale-token guard */
} APPREG;

static APPREG g_apptab[NSFREQ_APP_MAX];

/* ---- protocol table (proto -> PROTOPS, for RQ_SOCKET) ---------------------- */
#define NSFREQ_PROTO_MAX  4

typedef struct protoent {
    UCHAR    inuse;
    UCHAR    proto;
    PROTOPS *ops;
} PROTOENT;

static PROTOENT g_prototab[NSFREQ_PROTO_MAX];

/* ---- statistics (spec 8, message range 600-699) --------------------------- */
static STSCTR *req_recv, *req_bad, *req_nosys;
static int     req_stats_ready;

static void reqc(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

static void req_stats_init(void)
{
    if (req_stats_ready) {
        return;
    }
    req_recv  = sts_register("NSFREQ", "recv");     /* requests dispatched      */
    req_bad   = sts_register("NSFREQ", "badfn");    /* unknown fn (EINVAL)      */
    req_nosys = sts_register("NSFREQ", "nosys");    /* stub verb (EOPNOTSUPP)   */
    req_stats_ready = 1;
}

/* ---- init ----------------------------------------------------------------- */

void nsfreq_init(void)
{
    UINT i;

    req_stats_init();
    xq_init(&g_reqxq);
    g_reqecb = 0u;
    for (i = 0u; i < NSFREQ_APP_MAX; i++) {
        g_apptab[i].inuse = 0u;
        g_apptab[i].gen   = 1u;         /* token 0 (gen 0, idx 0) never valid   */
    }
    for (i = 0u; i < NSFREQ_PROTO_MAX; i++) {
        g_prototab[i].inuse = 0u;
    }
}

int nsfreq_register_proto(UCHAR proto, struct protops *ops)
{
    UINT i;

    /* Replace an existing registration for the same proto (idempotent re-init). */
    for (i = 0u; i < NSFREQ_PROTO_MAX; i++) {
        if (g_prototab[i].inuse && g_prototab[i].proto == proto) {
            g_prototab[i].ops = (PROTOPS *)ops;
            return 0;
        }
    }
    for (i = 0u; i < NSFREQ_PROTO_MAX; i++) {
        if (!g_prototab[i].inuse) {
            g_prototab[i].inuse = 1u;
            g_prototab[i].proto = proto;
            g_prototab[i].ops   = (PROTOPS *)ops;
            return 0;
        }
    }
    return 1;                           /* table full */
}

static PROTOPS *proto_lookup(UCHAR proto)
{
    UINT i;

    for (i = 0u; i < NSFREQ_PROTO_MAX; i++) {
        if (g_prototab[i].inuse && g_prototab[i].proto == proto) {
            return g_prototab[i].ops;
        }
    }
    return NULL;
}

UINT *nsfreq_ecb(void)
{
    return (UINT *)&g_reqecb;
}

/* ---- app registry helpers -------------------------------------------------- */

static UINT app_token(UINT idx)
{
    return ((UINT)g_apptab[idx].gen << 16) | (idx & 0xFFFFu);
}

/* Allocate an app slot; returns its token, or 0 when the table is full. */
static UINT app_alloc(void)
{
    UINT i;

    for (i = 0u; i < NSFREQ_APP_MAX; i++) {
        if (!g_apptab[i].inuse) {
            g_apptab[i].inuse = 1u;
            return app_token(i);
        }
    }
    return 0u;
}

/* Resolve a token to a live slot index, or -1 (stale / never-registered). */
static int app_index(UINT token)
{
    UINT idx = token & 0xFFFFu;
    UINT gen = (token >> 16) & 0xFFFFu;

    if (idx >= NSFREQ_APP_MAX || !g_apptab[idx].inuse) {
        return -1;
    }
    if ((UINT)g_apptab[idx].gen != gen) {
        return -1;                      /* stale token -> reused/freed slot     */
    }
    return (int)idx;
}

static void app_free(UINT idx)
{
    g_apptab[idx].inuse = 0u;
    g_apptab[idx].gen   = (USHORT)(g_apptab[idx].gen + 1u);
    if (g_apptab[idx].gen == 0u) {
        g_apptab[idx].gen = 1u;         /* never wrap to 0 */
    }
}

/* ---- fn handlers (executive side) ------------------------------------------
 * Each handler either COMPLETES r (soc_complete -> the app wakes) or, for a
 * delegated op, lets the protocol callback complete or park it. A handler never
 * touches r after completing it (the app owns it again post-POST). */

static void do_initapi(NSFRQE *r)
{
    UINT token = app_alloc();

    if (token == 0u) {
        soc_complete(r, NSF_RETERR, NSF_EMFILE);    /* no free app slot         */
        return;
    }
    r->apptok = token;                              /* handed back to the app   */
    soc_complete(r, NSF_RETOK, 0);
}

/* dev_foreach-style callback: destroy a socket iff it belongs to `*token`. */
static void term_one(SOCKCB *s, void *arg)
{
    UINT token = *(const UINT *)arg;

    if (s->owner_ascb == token) {
        soc_destroy(s);                 /* the ONE teardown checklist (10.5):   */
                                        /* a parked request -> ECONNABORTED     */
    }
}

static void do_termapi(NSFRQE *r)
{
    int idx = app_index(r->apptok);

    if (idx < 0) {
        soc_complete(r, NSF_RETERR, NSF_EINVAL);    /* stale / bad token        */
        return;
    }
    /* Mass teardown: every socket of this app, through the ONE destroy path. */
    soc_foreach(term_one, &r->apptok);
    app_free((UINT)idx);
    soc_complete(r, NSF_RETOK, 0);
}

static void do_socket(NSFRQE *r)
{
    SOCKCB  *s;
    PROTOPS *ops;
    UCHAR    domain = (UCHAR)r->p1;
    UCHAR    type   = (UCHAR)r->p2;
    UCHAR    proto  = (UCHAR)r->p3;

    /* The app must have INITAPI'd first (r->apptok scopes the socket for
     * TERMAPI). A bad token is EINVAL, not a silently-orphaned socket. */
    if (app_index(r->apptok) < 0) {
        soc_complete(r, NSF_RETERR, NSF_EINVAL);
        return;
    }
    ops = proto_lookup(proto);
    if (ops == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EPROTONOSUPPORT);
        return;
    }
    s = soc_create(domain, type, proto, ops);
    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EMFILE);    /* table/pool/attach failed */
        return;
    }
    s->owner_ascb = r->apptok;                      /* scope it to the app      */
    soc_complete(r, (INT)soc_desc(s), 0);           /* retcode = the descriptor */
}

/* Resolve the request's socket; NULL-complete with EBADF is the caller's job. */
static SOCKCB *req_socket(NSFRQE *r)
{
    return sock_lookup(r->sockdesc);
}

static void do_bind(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);
    int     rc;

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    /* Protocol-independent part: record the local name (spec 10.2). p1 = local
     * address (UINT, octet-1 MSB); p2 = local port (host order). */
    s->laddr = r->p1;
    s->lport = (USHORT)r->p2;
    s->state = SOC_ST_BOUND;
    /* Let the protocol react (a stub is fine). A missing bind op is not an error
     * for the protocol-independent bind -- the name is already recorded. */
    rc = soc_dispatch(s, r);
    if (rc == NSF_EOPNOTSUPP) {
        rc = 0;
    }
    soc_complete(r, (rc == 0) ? NSF_RETOK : NSF_RETERR, (rc == 0) ? 0 : rc);
}

/* LISTEN is SYNCHRONOUS like BIND (the listen op returns an rc and does NOT
 * receive r, so it cannot complete it): complete r here from the op's result.
 * This is why RQ_LISTEN is NOT in the do_delegate group -- do_delegate leaves an
 * rc==0 op uncompleted (correct for CONNECT/ACCEPT/SEND/RECV, which complete or
 * park r themselves; wrong for the r-less listen op). Latent until M4-2's real
 * listen op first returned 0 (before that, TCP listen was NULL -> EOPNOTSUPP). */
static void do_listen(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);
    int     rc;

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    rc = soc_dispatch(s, r);            /* -> s->ops->listen(s, backlog)          */
    soc_complete(r, (rc == 0) ? NSF_RETOK : NSF_RETERR, (rc == 0) ? 0 : rc);
}

static void do_getsockname(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    /* Hand the local name back through the fn-specific words (spec 10.4). */
    r->p1 = s->laddr;
    r->p2 = (UINT)s->lport;
    soc_complete(r, NSF_RETOK, 0);
}

static void do_close(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    /* A protocol with a CLOSE op is given first refusal: if it TAKES OWNERSHIP
     * (returns NSF_CLOSE_OWNED -- TCP: a graceful FIN teardown that completes r
     * immediately and finishes in the background, or destroys a LISTEN/half-open
     * socket now), do_close does nothing further. Otherwise (a close op that only
     * did local cleanup, or no close op at all -- UDP) RQ_CLOSE goes through the
     * ONE destroy checklist (spec 10.5): r is not parked on s, so soc_destroy
     * (which ECONNABORTs s's PARKED requests) never touches r; complete it after.
     * This keeps every M3 dummy close op (which returns 0 = "not owned") working
     * unchanged while letting TCP own its background teardown. */
    if (s->ops != NULL && s->ops->close != NULL) {
        if (soc_dispatch(s, r) == NSF_CLOSE_OWNED) {
            return;                     /* the op owns r + teardown               */
        }
    }
    soc_destroy(s);
    soc_complete(r, NSF_RETOK, 0);
}

/* Socket-protocol verbs: delegate to soc_dispatch, which invokes the protocol op
 * (the op completes or parks r) and returns non-zero ONLY when it did NEITHER
 * (missing op / unknown fn). So complete r here iff soc_dispatch returned an
 * error -- never double-complete a handled/parked request. */
static void do_delegate(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);
    int     rc;

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    rc = soc_dispatch(s, r);
    if (rc != 0) {
        soc_complete(r, NSF_RETERR, rc);
    }
}

/* ---- the dispatcher (the complete, frozen verb set) ----------------------- */

void nsfreq_dispatch(NSFRQE *r)
{
    if (r == NULL) {
        return;
    }
    reqc(req_recv);
    TRC(SOCKET, "req fn=%u fd=%08X flags=%04X", (unsigned)r->fn,
        (unsigned)r->sockdesc, (unsigned)r->flags);

    switch (r->fn) {
    /* -- protocol-independent, handled here -- */
    case RQ_INITAPI:     do_initapi(r);      break;
    case RQ_TERMAPI:     do_termapi(r);      break;
    case RQ_SOCKET:      do_socket(r);       break;
    case RQ_BIND:        do_bind(r);         break;
    case RQ_LISTEN:      do_listen(r);       break;
    case RQ_GETSOCKNAME: do_getsockname(r);  break;
    case RQ_CLOSE:       do_close(r);        break;

    /* -- socket-protocol verbs, delegated to the PROTOPS op (M3-3 UDP, M4 TCP;
     *    the M3-1 dummy protocol proves the park/complete round-trip). LISTEN is
     *    NOT here: it is synchronous + r-less, handled by do_listen above. -- */
    case RQ_CONNECT:
    case RQ_ACCEPT:
    case RQ_SEND:
    case RQ_SENDTO:
    case RQ_RECV:
    case RQ_RECVFROM:
    case RQ_SHUTDOWN:    do_delegate(r);     break;

    /* -- frozen but not implemented in M3-2: complete cleanly, never crash.
     *    ERRNO is NSF_EOPNOTSUPP (45) -- IBM Table 67 has no ENOSYS and 78 is
     *    EDEADLK, so the classic "operation not supported" value is correct
     *    (ADR-0029; the M3-4 errno correction). -- */
    case RQ_SELECT:
    case RQ_SETSOCKOPT:
    case RQ_GETSOCKOPT:
    case RQ_FCNTL:
    case RQ_GETPEERNAME:
        reqc(req_nosys);
        soc_complete(r, NSF_RETERR, NSF_EOPNOTSUPP);
        break;

    /* -- unknown fn: clean error, never a fall-through -- */
    default:
        reqc(req_bad);
        soc_complete(r, NSF_RETERR, NSF_EINVAL);
        break;
    }
}

/* ---- the app side (may run on another TCB) -------------------------------- */

void nsfreq_submit(NSFRQE *r)
{
    if (r == NULL) {
        return;
    }
    xq_push(&g_reqxq, &r->q);           /* CS-safe: multiple app TCBs may push  */
    nsfthr_post(&g_reqecb, 0u);         /* real SVC 2 POST -> wake the executive */
}

void nsfreq_wait(NSFRQE *r)
{
    if (r == NULL) {
        return;
    }
    nsfthr_wait((NSFECB *)&r->ecb);     /* block on this request's own ecb      */
}

void nsfreq_call(NSFRQE *r)
{
    nsfreq_submit(r);
    nsfreq_wait(r);
}

/* ---- the executive side (one call per loop pass) -------------------------- */

int nsfreq_pending(void)
{
    return (g_reqxq.head != NULL) ? 1 : 0;
}

void nsfreq_drain(void)
{
    do {
        QELEM *chain, *prev, *cur;

        /* Reset the requestECB BEFORE taking the queue: a submit that arrives
         * after this reset re-POSTs it, and its element is caught either by the
         * xq_drain below or by the do/while non-empty recheck -- never lost
         * (ADR-0022 reset-before-WAIT + double-check-drain; the #27 class). */
        g_reqecb = 0u;
        chain = xq_drain(&g_reqxq);     /* atomic take-all; LIFO order          */

        /* Reverse LIFO -> FIFO so requests dispatch in submission order (the
         * executive's fairness rule, exactly as evt_drain_handoff does). */
        prev = NULL;
        while (chain != NULL) {
            QELEM *nx = chain->next;
            chain->next = prev;
            prev = chain;
            chain = nx;
        }
        for (cur = prev; cur != NULL; ) {
            QELEM  *nx = cur->next;                  /* capture BEFORE dispatch: */
            NSFRQE *r  = Q_ENTRY(cur, NSFRQE, q);    /* a completed r may be     */
            nsfreq_dispatch(r);                      /* reused by the app        */
            cur = nx;
        }
    } while (g_reqxq.head != NULL);
    /* Terminates: each blocking app subtask has <=1 outstanding request (it
     * WAITs on r->ecb before issuing another), so the producer fan-in is
     * bounded -- the recheck loop drains a finite backlog and stops. */
}
