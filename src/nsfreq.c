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
#include "nsftime.h"           /* nsf_now + nsf_elapsed_ge: the sweep's clock */
#include "nsfreqx.h"           /* NSFREQX_CL_* -- the verdicts, macros only:
                                * no function from nsfreqx.c is called here, so
                                * this adds no link dependency (nsfapp.c does
                                * the same). */

/* ---- transport: request queue + requestECB --------------------------------
 * g_reqxq is the CS-safe MPSC handoff (producers = app subtasks; consumer = the
 * executive). g_reqecb is the spec-5.3 requestECB the executive WAITs on. */
/* The Phase-2 transport, when one has registered (ADR-0041).  NULL = Phase 1,
 * and then every path below is byte-for-byte what it has been since M3-2: this
 * is a registration seam in the idiom of evt_set_request / nsfip_register_proto,
 * not a mode switch, so the 20 modules that link this file are unaffected.
 *
 * It is a single `call` op rather than a submit/wait pair because the SVC
 * transport does the POST and the WAIT inside one invocation (ADR-0038) --
 * synchronous by construction, so the two cannot be separated on that
 * transport.  Harmless: NSFEZA uses nsfreq_call at every call site, and there
 * is no async submit/wait split in the API to preserve. */
static void (*g_xtransport)(NSFRQE *r);

void nsfreq_set_transport(void (*fn)(NSFRQE *r))
{
    g_xtransport = fn;
}

/* The client-liveness classifier, NULL until a transport that can reach one
 * registers it (see the header).  Phase 1 leaves it NULL. */
static int (*g_classify)(UINT ascb, UINT asid);

static XQ     g_reqxq;
static NSFECB g_reqecb;

/* ---- app registry (RQ_INITAPI / RQ_TERMAPI scoping) ------------------------
 * A fixed table of app instances. Each RQ_SOCKET stamps the new socket's
 * apptok with the requesting app's token; RQ_TERMAPI destroys every socket
 * carrying that token (the mass-teardown path) and frees the slot. The token is
 * (gen<<16)|idx like a socket descriptor, so a stale token never matches a
 * reused slot. Static -- no pool, no runtime allocation (spec 3). */
/* 64, matching the two limits either side of it: the CSA request-slot pool is
 * NSFV_NSLOTS = 64 (ADR-0042), so up to 64 clients can have a request
 * outstanding, and the socket table is NSFSOC_MAX_DEFAULT = 64. At 16 this
 * registry was the tighter bound -- a 17th client could claim a CSA slot and
 * reach the dispatcher, only to be refused at INITAPI. Costs 12 bytes a slot,
 * so the whole table is 768 bytes static.
 *
 * IT BUYS TIME AND FIXES NOTHING.  Raising a bound does not reclaim a slot; it
 * only means more clients can leak one before the wall is hit.  What actually
 * fills this table is applications that end without TERMAPI, and the sweep
 * below reclaims those only for the narrow class it can prove dead -- never a
 * batch client.  The real answer is deferred to issue #88. */
#define NSFREQ_APP_MAX   64

typedef struct appreg {
    UCHAR  inuse;
    USHORT gen;                         /* bumped on free -> stale-token guard */
    /* WHO asked, recorded by the TRANSPORT at RQ_INITAPI (M5-2c1). Both halves,
     * because an ASCB alone cannot survive ASID reuse -- the pair is what
     * nsfreqx_classify needs. Zero in Phase 1 (no transport, no caller address
     * space) and that is a supported value meaning "no identity recorded". */
    UINT   ascb;
    UINT   asid;
} APPREG;

static APPREG g_apptab[NSFREQ_APP_MAX];

/* Sweep state (M5-2c1 stage b, ADR-0045). The stamp of the last sweep that
 * actually RAN, and the per-reclaim notification seam.
 *
 * {0,0} means "never swept" and is deliberately a LONG time ago on both
 * platforms (nsftime.h), so the first call sweeps rather than waiting out an
 * epoch. NOT a timer: arming one would keep the STIMER permanently armed and
 * reintroduce the idle floor ADR-0043 established is not needed. */
static NSFTIME g_lastsweep;
static void  (*g_sweepnotify)(UINT idx, UINT token, UINT ascb, UINT asid);
static void  (*g_sweepsummary)(int reclaimed);

/* Sweep counters, and they are the THIRD STATE FOR A WHOLE RUN (CLAUDE.md 8.5).
 * A sweep that reclaims nothing is SILENT, and so is a sweep that never
 * happened -- yet they mean opposite things: "looked, everything was alive"
 * (expected, and the normal case, since a batch client always reads LIVE) vs
 * "no pass, or a request was in service, or the interval had not elapsed".
 * Without these a null live run is uninterpretable, which is exactly what
 * ADR-0045 3 says the gate must avoid.
 *
 * PLAIN COUNTERS, NOT sts_register.  The NSFS build already registers ~46
 * counters and sts_render's fixed 512-byte buffer truncates the rendered reply
 * well before the end of the list, so a counter added here could be one that
 * never reaches the console -- evidence that silently does not exist. They are
 * reported through the STATS SUPPLEMENT instead (nsfsx_stats_extra), which
 * emits its own WTO lines after the rendered block and cannot be pushed out. */
static UINT g_sweeps_run;               /* sweeps that actually LOOKED         */
static UINT g_slots_reclaimed;          /* app slots reclaimed, cumulative     */

/* ---- protocol table (proto -> PROTOPS, for RQ_SOCKET) ---------------------- */
#define NSFREQ_PROTO_MAX  4

typedef struct protoent {
    UCHAR    inuse;
    UCHAR    proto;
    PROTOPS *ops;
} PROTOENT;

static PROTOENT g_prototab[NSFREQ_PROTO_MAX];

/* ---- RQ_SELECT handler (M4-5, ADR-0035) ------------------------------------
 * SELECT is one request over N sockets, so it is NOT a per-socket PROTOPS op; its
 * engine (NSFSEL) registers here. NULL until registered -> RQ_SELECT stays
 * NSF_EOPNOTSUPP (the M3-2 behaviour, so a build without NSFSEL is unchanged). */
static void (*g_select_handler)(NSFRQE *r);

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
    g_xtransport = NULL;                /* Phase 1 until a transport registers */
    g_classify   = NULL;                /* ... and until one supplies a guard  */
    g_sweepnotify  = NULL;              /* ... and until one wants to be told  */
    g_sweepsummary = NULL;
    g_lastsweep.hi = 0u;                /* "never swept" -> the first call runs */
    g_lastsweep.lo = 0u;
    g_sweeps_run      = 0u;
    g_slots_reclaimed = 0u;
    for (i = 0u; i < NSFREQ_APP_MAX; i++) {
        g_apptab[i].inuse = 0u;
        g_apptab[i].ascb  = 0u;
        g_apptab[i].asid  = 0u;
        g_apptab[i].gen   = 1u;         /* token 0 (gen 0, idx 0) never valid   */
    }
    for (i = 0u; i < NSFREQ_PROTO_MAX; i++) {
        g_prototab[i].inuse = 0u;
    }
    g_select_handler = NULL;                    /* NSFSEL re-registers if linked */
}

void nsfreq_register_select(void (*fn)(NSFRQE *r))
{
    g_select_handler = fn;
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

/* Allocate an app slot; returns its token, or 0 when the table is full. The
 * caller identity is stored with the slot and never read from the request. */
static UINT app_alloc(UINT ascb, UINT asid)
{
    UINT i;

    for (i = 0u; i < NSFREQ_APP_MAX; i++) {
        if (!g_apptab[i].inuse) {
            g_apptab[i].inuse = 1u;
            g_apptab[i].ascb  = ascb;
            g_apptab[i].asid  = asid;
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
    g_apptab[idx].ascb  = 0u;           /* a freed slot names nobody */
    g_apptab[idx].asid  = 0u;
    g_apptab[idx].gen   = (USHORT)(g_apptab[idx].gen + 1u);
    if (g_apptab[idx].gen == 0u) {
        g_apptab[idx].gen = 1u;         /* never wrap to 0 */
    }
}

void nsfreq_set_classifier(int (*fn)(UINT ascb, UINT asid))
{
    g_classify = fn;
}

void nsfreq_set_sweep_notify(void (*fn)(UINT idx, UINT token, UINT ascb,
                                        UINT asid))
{
    g_sweepnotify = fn;
}

void nsfreq_set_sweep_summary(void (*fn)(int reclaimed))
{
    g_sweepsummary = fn;
}

int nsfreq_app_classify(UINT idx)
{
    UINT ascb = 0u;
    UINT asid = 0u;

    if (!nsfreq_app_info(idx, NULL, &ascb, &asid)) {
        return NSFREQ_APPCL_FREE;
    }
    /* THE RED LINE, IN ONE PLACE (see the header).  No identity recorded means
     * there is no address space to ask about; no classifier means there is
     * nobody to ask.  Neither is a verdict, and neither may be turned into
     * one -- so the classifier is never called with a zero ASCB. */
    if (ascb == 0u || g_classify == NULL) {
        return NSFREQ_APPCL_NONE;
    }
    return g_classify(ascb, asid);
}

/* The one read window onto the registry (see the header). Bounds-checked, and
 * an idle slot reports 0 rather than handing back stale fields. */
int nsfreq_app_info(UINT idx, UINT *token, UINT *ascb, UINT *asid)
{
    if (idx >= NSFREQ_APP_MAX || !g_apptab[idx].inuse) {
        return 0;
    }
    if (token != NULL) *token = app_token(idx);
    if (ascb  != NULL) *ascb  = g_apptab[idx].ascb;
    if (asid  != NULL) *asid  = g_apptab[idx].asid;
    return 1;
}

UINT nsfreq_app_max(void)
{
    return (UINT)NSFREQ_APP_MAX;
}

/* ---- fn handlers (executive side) ------------------------------------------
 * Each handler either COMPLETES r (soc_complete -> the app wakes) or, for a
 * delegated op, lets the protocol callback complete or park it. A handler never
 * touches r after completing it (the app owns it again post-POST). */

static void do_initapi(NSFRQE *r, UINT caller_ascb, UINT caller_asid)
{
    UINT token = app_alloc(caller_ascb, caller_asid);

    if (token == 0u) {
        /* THE SECOND TRIGGER (ADR-0045).  The table is full, which is exactly
         * the moment a leaked slot costs something, so look for dead clients
         * NOW rather than waiting out the periodic interval -- 0 bypasses the
         * limiter.  Same function as the periodic caller, the cap as its
         * parameter: two paths that can drift is what this milestone has
         * already paid for once.
         *
         * THIS SWEEPS WHILE A REQUEST IS IN SERVICE, WHICH THE PERIODIC CALLER
         * DELIBERATELY WILL NOT DO -- so say why it is safe HERE, because the
         * two call sites look contradictory otherwise.  We are inside the
         * dispatch of this very request, so in Phase 2 g_busy is set BY
         * CONSTRUCTION and the periodic guard could never be satisfied at this
         * point; the guard exists to stop a scan from destroying a socket that
         * a DIFFERENT, parked request is waiting on, and there cannot be one:
         *
         *   Phase 2 -- ADR-0042 10 permits exactly ONE request in flight, and
         *     it is this INITAPI, which owns no socket and parks on nothing.
         *   Phase 1 -- no classifier is registered, so no slot is ever DEAD
         *     and the scan reclaims nothing whatever else is outstanding.
         *
         * THE PHASE-2 HALF OF THAT ARGUMENT DIES IF CONCURRENT SERVICE LANDS
         * (a named open item in ADR-0042 10): a second in-flight request could
         * then be parked on a socket this scan destroys.  Whoever implements
         * concurrent service must revisit this call site, not just the drain.
         *
         * Retry ONCE.  If the sweep reclaimed nothing the table is genuinely
         * full of live applications and EMFILE is the true answer; looping
         * would just be the same scan again. */
        (void)nsfreq_app_sweep(0u);
        token = app_alloc(caller_ascb, caller_asid);
    }
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

    if (s->apptok == token) {
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

/* ---- the best-effort reclamation sweep (M5-2c1 stage b, ADR-0045) ---------
 *
 * The contract, the limits and the reason this ships as best-effort are in
 * nsfreq.h; what follows is only how it does it.
 *
 * IT REUSES do_termapi's MACHINERY EXACTLY -- soc_foreach(term_one) then
 * app_free -- because a client that died IS an application that never got to
 * call TERMAPI, and it must die through the same checklist.  A second teardown
 * path here would be a second thing to keep correct (spec 10.5).
 *
 * THE TOKEN IS CAPTURED BEFORE app_free, exactly as do_termapi captures it: the
 * free bumps the slot generation, so a token read afterwards matches no socket
 * and the sweep would silently reclaim the slot while leaking every socket on
 * it.  Both statements below depend on that order.
 *
 * TWO CALLERS, ONE IMPLEMENTATION.  The executive drain passes the periodic
 * interval; do_initapi passes 0 when the table is full.
 */
int nsfreq_app_sweep(UINT min_secs)
{
    NSFTIME now;
    UINT    i;
    int     reclaimed = 0;

    nsf_now(&now);
    if (!nsf_elapsed_ge(&g_lastsweep, &now, min_secs)) {
        return NSFREQ_SWEEP_SKIPPED;    /* did NOT look -- see the header      */
    }
    g_lastsweep = now;                  /* stamped for a sweep that RAN only   */
    g_sweeps_run++;                     /* ... and counted on the same terms   */

    for (i = 0u; i < NSFREQ_APP_MAX; i++) {
        UINT token;
        UINT ascb;
        UINT asid;

        if (!g_apptab[i].inuse) {
            continue;
        }
        /* Through nsfreq_app_classify, never g_classify: that function is
         * where the zero-identity red line lives, so a Phase-1 slot (no
         * identity, hence no verdict) is NONE here and is never reclaimed.
         * UNKNOWN is not reclaimed either -- ADR-0040's rule survives contact
         * with the sweep, and DEAD is the only verdict that acts. */
        if (nsfreq_app_classify(i) != NSFREQX_CL_DEAD) {
            continue;
        }

        /* ALL THREE captured BEFORE app_free, not just the token: app_free
         * bumps the generation AND zeroes the identity, so reading either
         * afterwards reports a slot that names nobody -- a notification of
         * ASCB=0 ASID=0, which is precisely the identity a live run needs to
         * read to tell a reclaim from a reuse. */
        token = app_token(i);
        ascb  = g_apptab[i].ascb;
        asid  = g_apptab[i].asid;

        soc_foreach(term_one, &token);
        app_free(i);
        reclaimed++;
        g_slots_reclaimed++;

        if (g_sweepnotify != NULL) {
            /* After the reclaim, so the message describes something that has
             * already happened rather than something about to be attempted. */
            g_sweepnotify(i, token, ascb, asid);
        }
    }
    /* THE SUMMARY IS A PROPERTY OF THE SWEEP, NOT OF ONE CALLER.  It was
     * briefly the periodic caller's own line, and that silently exempted the
     * OTHER trigger -- a full table at INITAPI, which is the LOUDEST and most
     * consequential burst there is (up to 64 reclaims at once) and the one an
     * operator most needs the caveat beside.  Emitting it here means the two
     * triggers cannot drift, which is the same rule the sweep itself is built
     * on: one function, two callers.
     *
     * Only when something was actually reclaimed -- an empty sweep would be a
     * line every ten seconds, forever. */
    if (reclaimed > 0 && g_sweepsummary != NULL) {
        g_sweepsummary(reclaimed);
    }
    return reclaimed;
}

void nsfreq_sweep_stats(UINT *sweeps, UINT *reclaimed)
{
    if (sweeps    != NULL) *sweeps    = g_sweeps_run;
    if (reclaimed != NULL) *reclaimed = g_slots_reclaimed;
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
    s->apptok = r->apptok;                      /* scope it to the app      */
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

/* GETPEERNAME (M4-5): hand back the connected peer's name (spec 10.4), byte-wise
 * through the fn-specific words exactly as do_getsockname returns the local name. A
 * socket with no foreign name (a listener, an unconnected UDP socket, or a fresh
 * socket) is NSF_ENOTCONN -- s->faddr is set only by CONNECT / a completed ACCEPT,
 * so faddr == 0 is the reliable "no peer" signal. */
static void do_getpeername(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    if (s->faddr == 0u) {
        soc_complete(r, NSF_RETERR, NSF_ENOTCONN);
        return;
    }
    r->p1 = s->faddr;
    r->p2 = (UINT)s->fport;
    soc_complete(r, NSF_RETOK, 0);
}

/* SETSOCKOPT (M4-5, minimal set): accept SO_REUSEADDR and TCP_NODELAY (recorded at
 * the compatibility level -- SO_REUSEADDR's documented TIME_WAIT-bind effect needs
 * a TCP bind-time port-conflict check that v1 does not do, and TCP_NODELAY has no
 * effect until Nagle exists at M5; the SOCKCB stays spec-exact with no flags field,
 * so both are accepted no-ops). Any other option -> EOPNOTSUPP with the option in
 * the trace (docs/ezasoket-conformance.md §3). */
static void do_setsockopt(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    if ((r->p1 == (UINT)NSF_SOL_SOCKET && r->p2 == (UINT)NSF_SO_REUSEADDR) ||
        (r->p1 == (UINT)NSF_IPPROTO_TCP && r->p2 == (UINT)NSF_TCP_NODELAY)) {
        soc_complete(r, NSF_RETOK, 0);          /* accepted (no v1 effect)        */
        return;
    }
    TRC(SOCKET, "setsockopt unsupported level=%u opt=%u",
        (unsigned)r->p1, (unsigned)r->p2);
    soc_complete(r, NSF_RETERR, NSF_EOPNOTSUPP);
}

/* GETSOCKOPT (M4-5, minimal set): SO_RCVBUF / SO_SNDBUF return the fixed buffer
 * size (NSF_SO_DFLT_BUF, mirroring the TCP window / send budget) in p3. Any other
 * option -> EOPNOTSUPP with the option in the trace. */
static void do_getsockopt(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    if (r->p1 == (UINT)NSF_SOL_SOCKET &&
        (r->p2 == (UINT)NSF_SO_RCVBUF || r->p2 == (UINT)NSF_SO_SNDBUF)) {
        r->p3 = (UINT)NSF_SO_DFLT_BUF;          /* the actual (fixed in v1)       */
        soc_complete(r, NSF_RETOK, 0);
        return;
    }
    TRC(SOCKET, "getsockopt unsupported level=%u opt=%u",
        (unsigned)r->p1, (unsigned)r->p2);
    soc_complete(r, NSF_RETERR, NSF_EOPNOTSUPP);
}

/* FCNTL (M4-5): the persistent non-blocking flag (F_GETFL/F_SETFL of O_NONBLOCK,
 * IOCTL FIONBIO) lives entirely in the NSFEZA facade's per-app mapping table (the
 * SOCKCB has no flags field), so those never reach the executive. A raw RQ_FCNTL
 * that does arrive is an unsupported command: validate the socket, then
 * EOPNOTSUPP with the command in the trace. */
static void do_fcntl(NSFRQE *r)
{
    SOCKCB *s = req_socket(r);

    if (s == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EBADF);
        return;
    }
    TRC(SOCKET, "fcntl unsupported cmd=%u", (unsigned)r->p1);
    soc_complete(r, NSF_RETERR, NSF_EOPNOTSUPP);
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
    /* Phase 1 and every direct-call test: no transport, so no caller address
     * space, so no identity. Zero is the supported "none recorded" value -- see
     * the nsfreq_dispatch_id contract in the header. */
    nsfreq_dispatch_id(r, 0u, 0u);
}

void nsfreq_dispatch_id(NSFRQE *r, UINT caller_ascb, UINT caller_asid)
{
    if (r == NULL) {
        return;
    }
    reqc(req_recv);
    TRC(SOCKET, "req fn=%u fd=%08X flags=%04X", (unsigned)r->fn,
        (unsigned)r->sockdesc, (unsigned)r->flags);

    switch (r->fn) {
    /* -- protocol-independent, handled here -- */
    case RQ_INITAPI:     do_initapi(r, caller_ascb, caller_asid); break;
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

    /* -- SELECT (M4-5, ADR-0035): one request over N sockets, so it is routed to
     *    the registered NSFSEL engine (completes or parks r). NULL when NSFSEL is
     *    not linked -> the M3-2 NSF_EOPNOTSUPP behaviour, unchanged. -- */
    case RQ_SELECT:
        if (g_select_handler != NULL) {
            g_select_handler(r);
        } else {
            reqc(req_nosys);
            soc_complete(r, NSF_RETERR, NSF_EOPNOTSUPP);
        }
        break;

    /* -- M4-5 socket-level verbs handled here (GETPEERNAME reads the connected
     *    peer; SET/GETSOCKOPT the minimal documented option set; FCNTL's persistent
     *    non-blocking flag lives in the NSFEZA facade, so the dispatcher path is a
     *    no-op ack). -- */
    case RQ_GETPEERNAME: do_getpeername(r);  break;
    case RQ_SETSOCKOPT:  do_setsockopt(r);   break;
    case RQ_GETSOCKOPT:  do_getsockopt(r);   break;
    case RQ_FCNTL:       do_fcntl(r);        break;

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
    if (g_xtransport != NULL) {
        /* Cross-AS: the whole round trip happens here (see above), so the
         * paired nsfreq_wait has nothing left to do. */
        g_xtransport(r);
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
    if (g_xtransport != NULL) {
        return;                         /* already complete (see nsfreq_submit) */
    }
    nsfthr_wait((NSFECB *)&r->ecb);     /* block on this request's own ecb      */
}

void nsfreq_call(NSFRQE *r)
{
    if (r == NULL) {
        return;
    }
    if (g_xtransport != NULL) {
        g_xtransport(r);                /* one SVC = submit + wait             */
        return;
    }
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
