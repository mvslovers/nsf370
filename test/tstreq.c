/*
 * tstreq.c -- NSFREQ request-transport + dispatcher tests (spec ch. 10, M3-2).
 *
 * Two layers, mirroring tstevt.c's dual model:
 *
 *   PORTABLE (host + MVS) -- direct-call dispatcher tests, no threads, no loop:
 *     - dispatch: every implemented verb reaches its handler; a delegated verb
 *       reaches the protocol op; an unimplemented verb -> EOPNOTSUPP; an unknown fn
 *       -> a clean EINVAL (no fall-through, no crash); bad descriptor -> EBADF;
 *       bad app token -> EINVAL; unknown proto -> EPROTONOSUPPORT.
 *     - TERMAPI mass teardown: an app with N sockets (one with a parked RECV) ->
 *       TERMAPI destroys them all through soc_destroy, completes the parked
 *       request with ECONNABORTED, and returns SOCKET + BUF pools to baseline.
 *     - app registry exhaustion -> EMFILE.
 *
 *   HOST-ONLY (#ifndef __MVS__) -- the real transport round-trip over pthreads:
 *     - round-trip: an executive thread runs evt_mainloop with the request seam
 *       wired; an app thread does INITAPI -> SOCKET -> a blocking RECVFROM that
 *       PARKS (app still WAITing, ecb not posted) -> a SENDTO trigger completes
 *       the parked recv on the executive -> the app wakes exactly once -> CLOSE
 *       -> TERMAPI. Asserts the app actually blocked (not spun) and woke once.
 *     - lost-request guard: many producer threads fire batches of requests into
 *       the executive's ECB-reset window; every one is dispatched, none lost
 *       (the reset-before-drain + double-check-drain proof, ADR-0022 / #27).
 *   (The MVS transport round-trip -- main task executive + a cthread app subtask
 *    over the real queue+POST+requestECB -- is test/mvs/tstreqm.c.)
 */
#include "nsfreq.h"
#include "nsfapp.h"
#include "nsfreqx.h"
#include "nsfmsg.h"
#include "nsfsoc.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsfque.h"
#include "nsfsts.h"
#include "nsfevt.h"
#include "nsfevtp.h"            /* NSFECB_POSTED */
#include "nsfthr.h"             /* nsfthr_setup (host no-op; MVS IDENTIFY)     */
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>
#ifndef __MVS__
#include <pthread.h>            /* host-only: the executive + app + producers  */
#endif

/* A distinct experimental IP protocol number for the test-only dummy protocol,
 * clear of UDP (17) and TCP (6) so it never shadows a real M3-3/M4 registration. */
#define DUMMY_PROTO   254u

/* ---- test-only dummy protocol (the M3-1 PROTOPS shape) --------------------- */
typedef struct dummyrec {
    int attach, bind, connect, listen, send, recv, close, detach;
} DUMMYREC;
static DUMMYREC g_d;

static int d_attach(SOCKCB *s)             { (void)s; g_d.attach++;  return 0; }
static int d_bind(SOCKCB *s)               { (void)s; g_d.bind++;    return 0; }
static int d_connect(SOCKCB *s, NSFRQE *r) { (void)s; (void)r; g_d.connect++; return 0; }
static int d_listen(SOCKCB *s, int bl)     { (void)s; (void)bl; g_d.listen++; return 0; }
static int d_close(SOCKCB *s, NSFRQE *r)   { (void)s; (void)r; g_d.close++; return 0; }
static int d_detach(SOCKCB *s)             { (void)s; g_d.detach++;  return 0; }

#ifndef __MVS__
/* Round-trip completion handshake: d_recv signals when it parks; the SENDTO
 * trigger's d_send completes the parked recv (the "data arrived" moment). */
static pthread_mutex_t g_hs_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_hs_cv  = PTHREAD_COND_INITIALIZER;
static int             g_parked;

static void signal_parked(void)
{
    pthread_mutex_lock(&g_hs_mtx);
    g_parked = 1;
    pthread_cond_broadcast(&g_hs_cv);
    pthread_mutex_unlock(&g_hs_mtx);
}
static void wait_parked(void)
{
    pthread_mutex_lock(&g_hs_mtx);
    while (!g_parked) {
        pthread_cond_wait(&g_hs_cv, &g_hs_mtx);
    }
    pthread_mutex_unlock(&g_hs_mtx);
}
#endif

static int d_recv(SOCKCB *s, NSFRQE *r)
{
    g_d.recv++;
    if (!Q_EMPTY(&s->rxq)) {                        /* data queued: deliver it   */
        QELEM *e = q_deq(&s->rxq);
        PBUF  *b = Q_ENTRY(e, PBUF, q);
        INT    n = (INT)b->len;
        buf_free(b);
        soc_complete(r, n, 0);
        return 0;
    }
    if (r->flags & RQ_F_NONBLOCK) {                 /* empty + non-blocking       */
        soc_complete(r, NSF_RETERR, NSF_EWOULDBLOCK);
        return 0;
    }
    {
        int rc = soc_park(s, r, SOC_PEND_RECV);     /* empty + blocking: park     */
#ifndef __MVS__
        if (rc == 0) {
            signal_parked();                        /* tell the round-trip driver */
        }
#endif
        return rc;
    }
}

/* SEND delivers to a parked recv (simulating data arrival) then completes the
 * send itself -- the round-trip's "later soc_complete wakes the app". */
static int d_send(SOCKCB *s, NSFRQE *r)
{
    g_d.send++;
    if (s->pend_recv != NULL) {
        soc_complete(s->pend_recv, 7, 0);           /* 7 bytes "delivered"        */
    }
    soc_complete(r, NSF_RETOK, 0);
    return 0;
}

static PROTOPS dummy_ops = {
    .attach = d_attach, .bind = d_bind, .connect = d_connect, .listen = d_listen,
    .send = d_send, .recv = d_recv, .close = d_close, .detach = d_detach
};

/* ---- helpers --------------------------------------------------------------- */
static void rqe_init(NSFRQE *r, UINT fn, UINT desc, UINT flags)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->flags    = (USHORT)flags;
    r->sockdesc = desc;
}

static int posted(const NSFRQE *r)
{
    return (r->ecb & NSFECB_POSTED) != 0u;
}

static UINT sock_inuse(void)
{
#if NSF_DEBUG
    return soc_debug_inuse();
#else
    return 0u;
#endif
}

static UINT bufsmall_inuse(void)
{
#if NSF_DEBUG
    MMSTATS s;
    mm_stats(buf_debug_pool(NSFBUF_CLASS_SMALL), &s);
    return s.inuse;
#else
    return 0u;
#endif
}

/* Fresh socket layer + request manager + the dummy protocol registered. */
static void reset_req(void)
{
    soc_init();
    nsfreq_init();
    CHECK_EQ((long)nsfreq_register_proto((UCHAR)DUMMY_PROTO, &dummy_ops), 0L,
             "register dummy protocol");
    memset(&g_d, 0, sizeof(g_d));
}

/* ---- dispatch coverage (portable, direct calls) ---------------------------- */
static void test_dispatch(void)
{
    NSFRQE  r;
    SOCKCB *s;
    UINT    token, desc;

    reset_req();

    /* INITAPI -> a token */
    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "INITAPI completed (app ecb posted)");
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, "INITAPI retcode ok");
    CHECK(r.apptok != 0u, "INITAPI returned a non-zero app token");
    token = r.apptok;

    /* SOCKET with a bad token -> EINVAL (no orphan socket) */
    rqe_init(&r, RQ_SOCKET, 0u, 0u);
    r.apptok = 0xDEADBEEFu; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_DGRAM; r.p3 = DUMMY_PROTO;
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EINVAL, "SOCKET with a bad token -> EINVAL");
    CHECK_EQ((long)soc_count(), 0L, "no socket created for a bad token");

    /* SOCKET with an unregistered proto -> EPROTONOSUPPORT */
    rqe_init(&r, RQ_SOCKET, 0u, 0u);
    r.apptok = token; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_DGRAM; r.p3 = 99u;
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EPROTONOSUPPORT,
             "SOCKET with an unknown proto -> EPROTONOSUPPORT");

    /* SOCKET ok -> a descriptor, scoped to the app */
    rqe_init(&r, RQ_SOCKET, 0u, 0u);
    r.apptok = token; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_DGRAM; r.p3 = DUMMY_PROTO;
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "SOCKET completed");
    desc = (UINT)r.retcode;
    s = sock_lookup(desc);
    CHECK(s != NULL, "SOCKET returned a valid descriptor");
    CHECK_EQ((long)g_d.attach, 1L, "SOCKET ran the protocol attach");
    CHECK(s != NULL && s->apptok == token, "socket scoped to the app token");

    /* BIND records the local name and reaches the protocol stub */
    rqe_init(&r, RQ_BIND, desc, 0u);
    r.p1 = 0xC0A8C801u;                         /* 192.168.200.1 */
    r.p2 = 12345u;
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "BIND completed");
    CHECK(s->laddr == 0xC0A8C801u && s->lport == 12345u, "BIND recorded the local name");
    CHECK_EQ((long)g_d.bind, 1L, "BIND ran the protocol bind stub");

    /* GETSOCKNAME returns the bound name */
    rqe_init(&r, RQ_GETSOCKNAME, desc, 0u);
    nsfreq_dispatch(&r);
    CHECK(r.p1 == 0xC0A8C801u && r.p2 == 12345u, "GETSOCKNAME returned the bound name");

    /* a bad descriptor -> EBADF */
    rqe_init(&r, RQ_GETSOCKNAME, 0x00BADBADu, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EBADF, "bad descriptor -> EBADF");

    /* SEND is delegated to the protocol op */
    rqe_init(&r, RQ_SEND, desc, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)g_d.send, 1L, "SEND reached the protocol op");
    CHECK(posted(&r), "SEND completed");

    /* non-blocking RECV on an empty rxq -> EWOULDBLOCK (delegated, not parked) */
    rqe_init(&r, RQ_RECV, desc, RQ_F_NONBLOCK);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EWOULDBLOCK, "non-blocking RECV empty -> EWOULDBLOCK");
    CHECK(s->pend_recv == NULL, "non-blocking RECV did not park");

    /* unimplemented verbs -> EOPNOTSUPP (ADR-0029: no ENOSYS; 78 is EDEADLK) */
    rqe_init(&r, RQ_SELECT, 0u, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EOPNOTSUPP, "RQ_SELECT -> EOPNOTSUPP");
    rqe_init(&r, RQ_SETSOCKOPT, desc, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EOPNOTSUPP, "RQ_SETSOCKOPT -> EOPNOTSUPP");
    rqe_init(&r, RQ_FCNTL, desc, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EOPNOTSUPP, "RQ_FCNTL -> EOPNOTSUPP");

    /* unknown fn -> a clean EINVAL (no fall-through) */
    rqe_init(&r, 4242u, 0u, 0u);
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "unknown fn still completes (no hang)");
    CHECK_EQ((long)r.errno_, (long)NSF_EINVAL, "unknown fn -> EINVAL");

    /* CLOSE destroys the socket */
    rqe_init(&r, RQ_CLOSE, desc, 0u);
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "CLOSE completed");
    CHECK(sock_lookup(desc) == NULL, "CLOSE destroyed the socket");
    CHECK_EQ((long)g_d.detach, 1L, "CLOSE ran the protocol detach");

    /* TERMAPI frees the app slot; a second TERMAPI with the stale token -> EINVAL */
    rqe_init(&r, RQ_TERMAPI, 0u, 0u);
    r.apptok = token;
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "TERMAPI completed");
    rqe_init(&r, RQ_TERMAPI, 0u, 0u);
    r.apptok = token;
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EINVAL, "TERMAPI with a stale token -> EINVAL");
}

/* ---- TERMAPI mass teardown + leak gate (portable, direct calls) ------------ */
static void test_termapi_teardown(void)
{
    NSFRQE  r, recv;
    SOCKCB *s0;
    UINT    token, d[4];
    UINT    sbase, bbase;
    int     i;

    reset_req();
    sbase = sock_inuse();
    bbase = bufsmall_inuse();

    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch(&r);
    token = r.apptok;

    for (i = 0; i < 4; i++) {
        rqe_init(&r, RQ_SOCKET, 0u, 0u);
        r.apptok = token; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_DGRAM; r.p3 = DUMMY_PROTO;
        nsfreq_dispatch(&r);
        d[i] = (UINT)r.retcode;
        CHECK(sock_lookup(d[i]) != NULL, "socket created under the app");
    }
    CHECK_EQ((long)soc_count(), 4L, "four sockets open under the app");

    /* park a blocking RECV on socket 0 */
    s0 = sock_lookup(d[0]);
    rqe_init(&recv, RQ_RECVFROM, d[0], 0u);
    nsfreq_dispatch(&recv);
    CHECK(!posted(&recv), "parked RECV on socket 0 not completed");
    CHECK(s0->pend_recv == &recv, "RECV parked in pend_recv");

    /* TERMAPI: destroy every socket of the app; complete the parked request */
    rqe_init(&r, RQ_TERMAPI, 0u, 0u);
    r.apptok = token;
    nsfreq_dispatch(&r);
    CHECK(posted(&r), "TERMAPI completed");
    CHECK_EQ((long)soc_count(), 0L, "TERMAPI destroyed every app socket");
    CHECK(posted(&recv), "TERMAPI completed the parked RECV");
    CHECK_EQ((long)recv.errno_, (long)NSF_ECONNABORTED, "parked RECV -> ECONNABORTED");
    CHECK_EQ((long)g_d.detach, 4L, "detach ran for every destroyed socket");
    CHECK_EQ((long)sock_inuse(), (long)sbase, "SOCKET pool back to baseline (leak gate)");
    CHECK_EQ((long)bufsmall_inuse(), (long)bbase, "BUF pool back to baseline (leak gate)");
}

/* ---- app registry exhaustion (portable) ------------------------------------ */
static void test_app_full(void)
{
    NSFRQE r;
    int    n = 0;

    reset_req();
    for (;;) {
        rqe_init(&r, RQ_INITAPI, 0u, 0u);
        nsfreq_dispatch(&r);
        if (r.apptok == 0u) {
            break;
        }
        n++;
        if (n > 1000) {                         /* safety: never spin forever   */
            break;
        }
    }
    CHECK(n > 0, "at least one app instance registered");
    CHECK(n <= 1000, "app registry is bounded (INITAPI eventually fails)");
    CHECK_EQ((long)r.errno_, (long)NSF_EMFILE, "app registry full -> EMFILE");
}

/* ==========================================================================
 * HOST-ONLY threaded scenarios: the real transport round-trip + lost-request.
 * ========================================================================== */
#ifndef __MVS__

/* Fresh loop + request seam wired, for a threaded scenario. */
static void setup_loop(void)
{
    nsfevt_init();
    reset_req();
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);
}

static void *exec_thread(void *arg)
{
    (void)arg;
    evt_mainloop();                             /* runs until nsfevt_stop()     */
    return NULL;
}

/* ---- round-trip ------------------------------------------------------------ */
static volatile UINT g_desc;
static NSFRQE        g_recv_rqe;                /* inspected by the driver      */
static volatile int  g_woke;
static volatile INT  g_recv_ret;

static void *app_thread(void *arg)
{
    NSFRQE ri, rs, rc, rt;
    UINT   token;

    (void)arg;
    rqe_init(&ri, RQ_INITAPI, 0u, 0u);
    nsfreq_call(&ri);
    token = ri.apptok;

    rqe_init(&rs, RQ_SOCKET, 0u, 0u);
    rs.apptok = token; rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_DGRAM; rs.p3 = DUMMY_PROTO;
    nsfreq_call(&rs);
    g_desc = (UINT)rs.retcode;

    /* blocking RECVFROM: parks on the executive; completed by the SENDTO trigger */
    rqe_init(&g_recv_rqe, RQ_RECVFROM, g_desc, 0u);
    nsfreq_call(&g_recv_rqe);                   /* blocks until completed        */
    g_recv_ret = g_recv_rqe.retcode;
    g_woke++;

    rqe_init(&rc, RQ_CLOSE, g_desc, 0u);
    nsfreq_call(&rc);

    rqe_init(&rt, RQ_TERMAPI, 0u, 0u);
    rt.apptok = token;
    nsfreq_call(&rt);
    return NULL;
}

static void test_roundtrip(void)
{
    pthread_t exec, app;
    NSFRQE    snd;

    setup_loop();
    g_parked = 0; g_woke = 0; g_desc = 0u; g_recv_ret = 0;

    pthread_create(&exec, NULL, exec_thread, NULL);
    pthread_create(&app, NULL, app_thread, NULL);

    wait_parked();                              /* the RECVFROM has parked       */
    CHECK(!posted(&g_recv_rqe), "round-trip: app blocked in RECVFROM (ecb not posted)");
    CHECK_EQ((long)g_woke, 0L, "round-trip: app has not woken yet (truly parked)");

    /* deliver: a SENDTO whose dummy op completes the parked recv on the executive */
    rqe_init(&snd, RQ_SENDTO, g_desc, 0u);
    nsfreq_call(&snd);
    CHECK(posted(&snd), "round-trip: SENDTO trigger completed");

    pthread_join(app, NULL);
    CHECK_EQ((long)g_woke, 1L, "round-trip: app woke exactly once");
    CHECK_EQ((long)g_recv_ret, 7L, "round-trip: RECVFROM returned the delivered count");

    nsfevt_stop();
    pthread_join(exec, NULL);
    CHECK_EQ((long)soc_count(), 0L, "round-trip: no sockets left open");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "round-trip: EVT pool at baseline");
}

/* ---- lost-request guard ---------------------------------------------------- */
#define STRESS_PRODUCERS 4
#define STRESS_PERPROD   200
#define STRESS_ROUNDS    10
#define STRESS_TOTAL     (STRESS_PRODUCERS * STRESS_PERPROD * STRESS_ROUNDS)

static volatile long g_done;                   /* requests fully completed      */
static volatile long g_lost;                   /* requests that never completed  */
static volatile long g_badret;                 /* completed with the wrong errno */

static void *producer(void *arg)
{
    NSFRQE r[STRESS_PERPROD];
    int    i;

    (void)arg;
    /* Fire the whole batch WITHOUT waiting between submits, to maximise the
     * chance a push lands in the executive's requestECB-reset window. */
    for (i = 0; i < STRESS_PERPROD; i++) {
        rqe_init(&r[i], RQ_SELECT, 0u, 0u);    /* immediate EOPNOTSUPP: no alloc/sock */
        nsfreq_submit(&r[i]);
    }
    for (i = 0; i < STRESS_PERPROD; i++) {
        /* Bounded wait so a hypothetical LOST request is counted, not a hang. */
        nsfthr_timed_wait((NSFECB *)&r[i].ecb, 100u);   /* up to 10 s           */
        if (!posted(&r[i])) {
            (void)__sync_fetch_and_add(&g_lost, 1L);
        } else {
            (void)__sync_fetch_and_add(&g_done, 1L);
            if (r[i].errno_ != NSF_EOPNOTSUPP) {
                (void)__sync_fetch_and_add(&g_badret, 1L);
            }
        }
    }
    return NULL;
}

static void test_lost_request(void)
{
    pthread_t exec;
    int       round, p;

    setup_loop();
    g_done = 0L; g_lost = 0L; g_badret = 0L;

    pthread_create(&exec, NULL, exec_thread, NULL);
    for (round = 0; round < STRESS_ROUNDS; round++) {
        pthread_t prod[STRESS_PRODUCERS];
        for (p = 0; p < STRESS_PRODUCERS; p++) {
            pthread_create(&prod[p], NULL, producer, NULL);
        }
        for (p = 0; p < STRESS_PRODUCERS; p++) {
            pthread_join(prod[p], NULL);
        }
    }
    nsfevt_stop();
    pthread_join(exec, NULL);

    printf("lost-request stress: %ld done, %ld lost, %ld bad (target %d)\n",
           g_done, g_lost, g_badret, STRESS_TOTAL);
    CHECK_EQ(g_lost, 0L, "lost-request guard: ZERO requests lost");
    CHECK_EQ(g_done, (long)STRESS_TOTAL, "lost-request guard: every request dispatched");
    CHECK_EQ(g_badret, 0L, "lost-request guard: every request got its EOPNOTSUPP completion");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "lost-request guard: EVT pool at baseline");
}
#endif /* !__MVS__ */

/* -------------------------------------------------------------------------
 * M5-2c1: the caller identity travels from the transport to the app registry.
 *
 * Two halves, and the second is the red line: Phase 1 dispatches with a ZERO
 * identity, so a slot registered that way must read back as "none recorded"
 * and must never be handed to a liveness classifier as if it named an address
 * space.  Here we assert the value; the consumer-side rule is asserted where
 * the consumers are.
 * ------------------------------------------------------------------------- */
static int app_slot_of(UINT token)
{
    UINT i, n = nsfreq_app_max();
    UINT t;

    for (i = 0u; i < n; i++) {
        if (nsfreq_app_info(i, &t, NULL, NULL) && t == token) {
            return (int)i;
        }
    }
    return -1;
}

static UINT app_token_at(UINT idx)
{
    UINT t = 0u;

    (void)nsfreq_app_info(idx, &t, NULL, NULL);
    return t;
}

static void test_caller_identity(void)
{
    NSFRQE r;
    UINT   tok_id, tok_zero, ascb, asid;
    int    idx;

    reset_req();

    CHECK_EQ((long)nsfreq_app_max(), 16L, "app registry bound is 16 slots");

    /* (a) An identified caller: the pair is stored verbatim against the slot. */
    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch_id(&r, 0x00F1E2D3u, 0x0025u);
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, "INITAPI with an identity ok");
    tok_id = r.apptok;
    idx    = app_slot_of(tok_id);
    CHECK(idx >= 0, "identified app instance is findable in the registry");
    ascb = asid = 0xFFFFFFFFu;
    CHECK(idx >= 0 && nsfreq_app_info((UINT)idx, NULL, &ascb, &asid),
          "nsfreq_app_info reports the slot in use");
    CHECK_EQ((long)ascb, (long)0x00F1E2D3u, "caller ASCB stored verbatim");
    CHECK_EQ((long)asid, (long)0x0025u, "caller ASID stored verbatim");

    /* (b) THE PHASE-1 PATH. nsfreq_dispatch is nsfreq_dispatch_id(r, 0, 0):
     * the slot is fully usable and names nobody. */
    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, "INITAPI without an identity ok");
    tok_zero = r.apptok;
    CHECK(tok_zero != 0u && tok_zero != tok_id, "a second, distinct app token");
    idx  = app_slot_of(tok_zero);
    ascb = asid = 0xFFFFFFFFu;
    CHECK(idx >= 0 && nsfreq_app_info((UINT)idx, NULL, &ascb, &asid),
          "Phase-1 app instance is registered like any other");
    CHECK_EQ((long)ascb, 0L, "Phase-1 caller ASCB is zero (no identity)");
    CHECK_EQ((long)asid, 0L, "Phase-1 caller ASID is zero (no identity)");

    /* (c) A REUSED slot carries no stale identity.  Release the identified
     * instance, then take a slot again with a zero identity: it lands on the
     * same index (lowest free wins), and it must read back as naming nobody --
     * not as still naming the address space that used to live there. */
    idx = app_slot_of(tok_id);
    rqe_init(&r, RQ_TERMAPI, 0u, 0u);
    r.apptok = tok_id;
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, "TERMAPI released the slot");
    CHECK(app_slot_of(tok_id) < 0, "the freed token no longer resolves");

    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)app_slot_of(r.apptok), (long)idx, "the freed slot is reused");
    ascb = asid = 0xFFFFFFFFu;
    CHECK(nsfreq_app_info((UINT)idx, NULL, &ascb, &asid),
          "the reused slot is in use");
    CHECK_EQ((long)ascb, 0L, "a reused slot carries no stale caller ASCB");
    CHECK_EQ((long)asid, 0L, "a reused slot carries no stale caller ASID");
    rqe_init(&r, RQ_TERMAPI, 0u, 0u);
    r.apptok = app_token_at((UINT)idx);
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, "TERMAPI released the reused slot");

    /* (d) Out of range is 0, not an error and not a read. */
    CHECK_EQ((long)nsfreq_app_info(nsfreq_app_max(), NULL, NULL, NULL), 0L,
             "an out-of-range app index reports not-in-use");

    rqe_init(&r, RQ_TERMAPI, 0u, 0u);
    r.apptok = tok_zero;
    nsfreq_dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, "TERMAPI released the second slot");
}

/* -------------------------------------------------------------------------
 * M5-2c1: nsfreq_app_classify and the operator report.
 *
 * THE RED LINE IS THE POINT OF THIS SCENARIO. A Phase-1 app instance records
 * no caller identity, and a registered classifier must NEVER be asked about
 * it -- so the fake classifier below counts its calls, and the assertion is
 * that the count does not move. "It returned NO-ID" would also be true of a
 * classifier that ran and happened to answer that way; only the call count
 * distinguishes "not asked" from "asked and forgiven".
 * ------------------------------------------------------------------------- */
static UINT g_cls_calls;
static UINT g_cls_last_ascb, g_cls_last_asid;
static int  g_cls_answer;

static int fake_classify(UINT ascb, UINT asid)
{
    g_cls_calls++;
    g_cls_last_ascb = ascb;
    g_cls_last_asid = asid;
    return g_cls_answer;
}

#ifndef __MVS__
/* Does any captured operator line contain `needle`?  HOST ONLY: the capture
 * ring lives in src/nsfmsg_host.c; on MVS the emit seam is a real WTO and
 * there is nothing to read back. */
static int cap_contains(const char *needle)
{
    UINT i, n = nsfmsg_cap_count();

    for (i = 0u; i < n; i++) {
        const char *l = nsfmsg_cap_line(i);
        if (l != NULL && strstr(l, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}
#endif /* !__MVS__ */

static void test_app_classify_and_report(void)
{
    NSFRQE r;
    UINT   tok_live, tok_phase1;
    int    idx_live, idx_p1;

    reset_req();
    nsfreq_set_classifier(NULL);

    /* No classifier registered: an identified slot is still NOT a verdict. */
    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch_id(&r, 0x00ABCDEFu, 0x0031u);
    tok_live = r.apptok;
    idx_live = app_slot_of(tok_live);
    CHECK(idx_live >= 0, "identified app instance registered");
    CHECK_EQ((long)nsfreq_app_classify((UINT)idx_live), (long)NSFREQ_APPCL_NONE,
             "no classifier registered -> NO-ID, never a fabricated verdict");

    /* A Phase-1 instance alongside it: no identity at all. */
    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch(&r);
    tok_phase1 = r.apptok;
    idx_p1     = app_slot_of(tok_phase1);
    CHECK(idx_p1 >= 0, "Phase-1 app instance registered");

    /* Register a classifier that would answer DEAD for anything it is asked. */
    g_cls_calls  = 0u;
    g_cls_answer = NSFREQX_CL_DEAD;
    nsfreq_set_classifier(fake_classify);

    CHECK_EQ((long)nsfreq_app_classify((UINT)idx_live), (long)NSFREQX_CL_DEAD,
             "an identified slot is classified");
    CHECK_EQ((long)g_cls_calls, 1L, "the classifier was asked exactly once");
    CHECK_EQ((long)g_cls_last_ascb, (long)0x00ABCDEFu,
             "the classifier got the recorded ASCB");
    CHECK_EQ((long)g_cls_last_asid, (long)0x0031u,
             "the classifier got the recorded ASID");

    /* THE RED LINE: the Phase-1 slot is not offered to it at all. */
    CHECK_EQ((long)nsfreq_app_classify((UINT)idx_p1), (long)NSFREQ_APPCL_NONE,
             "a zero-identity slot answers NO-ID");
    CHECK_EQ((long)g_cls_calls, 1L,
             "a zero-identity slot is NEVER handed to the classifier");

    /* A free slot needs no classifier either. */
    CHECK_EQ((long)nsfreq_app_classify(nsfreq_app_max() - 1u),
             (long)NSFREQ_APPCL_FREE, "an idle slot answers FREE");
    CHECK_EQ((long)nsfreq_app_classify(nsfreq_app_max()),
             (long)NSFREQ_APPCL_FREE, "an out-of-range slot answers FREE");
    CHECK_EQ((long)g_cls_calls, 1L, "neither asked the classifier");

    /* Verdict words, so the console lines say something a reader can quote. */
    CHECK(strcmp(nsfapp_verdict_name(NSFREQX_CL_LIVE), "LIVE") == 0, "LIVE");
    CHECK(strcmp(nsfapp_verdict_name(NSFREQX_CL_DEAD), "DEAD") == 0, "DEAD");
    CHECK(strcmp(nsfapp_verdict_name(NSFREQX_CL_UNKNOWN), "UNKNOWN") == 0,
          "UNKNOWN");
    CHECK(strcmp(nsfapp_verdict_name(NSFREQ_APPCL_NONE), "NO-ID") == 0, "NO-ID");
    CHECK(strcmp(nsfapp_verdict_name(NSFREQ_APPCL_FREE), "FREE") == 0, "FREE");
    CHECK(strcmp(nsfapp_verdict_name(99), "?") == 0,
          "an unrecognised verdict is not trusted into a word");

    /* The report itself: heading, one line per IN-USE slot, and a summary.
     * HOST ONLY -- see cap_contains. Everything above this point, the red line
     * included, is asserted on the target too. */
#ifndef __MVS__
    nsfmsg_cap_reset();
    nsfapp_report();
    CHECK_EQ((long)nsfmsg_cap_count(), 4L,
             "report = heading + 2 in-use slots + summary (idle slots silent)");
    CHECK(cap_contains("NSF814I APP REGISTRY:"), "report has its heading");
    CHECK(cap_contains("TOKEN=") && cap_contains("ASCB=00ABCDEF") &&
          cap_contains("ASID=0031"), "the identified slot reports its identity");
    CHECK(cap_contains("DEAD"), "the identified slot reports its verdict");
    CHECK(cap_contains("NO-ID"), "the Phase-1 slot reports NO-ID");
    CHECK(cap_contains("NSF816I APP REGISTRY: 2 OF 16 SLOTS IN USE, 1 DEAD"),
          "the summary counts slots in use and, of those, the dead ones");

    /* Read-only: the report classified twice more and reaped nothing. */
    CHECK_EQ((long)g_cls_calls, 2L, "the report asked only about the identified slot");
    CHECK(app_slot_of(tok_live) == idx_live && app_slot_of(tok_phase1) == idx_p1,
          "the report changed no registry state");

    /* An empty registry still says so, in one line plus its brackets. */
    rqe_init(&r, RQ_TERMAPI, 0u, 0u); r.apptok = tok_live;   nsfreq_dispatch(&r);
    rqe_init(&r, RQ_TERMAPI, 0u, 0u); r.apptok = tok_phase1; nsfreq_dispatch(&r);
    nsfmsg_cap_reset();
    nsfapp_report();
    CHECK_EQ((long)nsfmsg_cap_count(), 2L, "an empty registry reports heading + summary");
    CHECK(cap_contains("NSF816I APP REGISTRY: 0 OF 16 SLOTS IN USE, 0 DEAD"),
          "an empty registry says so");
#else
    /* On the target, still release the two instances so the suite's leak gate
     * sees the registry as it found it. */
    rqe_init(&r, RQ_TERMAPI, 0u, 0u); r.apptok = tok_live;   nsfreq_dispatch(&r);
    rqe_init(&r, RQ_TERMAPI, 0u, 0u); r.apptok = tok_phase1; nsfreq_dispatch(&r);
#endif /* !__MVS__ */

    nsfreq_set_classifier(NULL);        /* leave the suite as we found it */
}

int main(void)
{
    printf("=== nsf370 NSFREQ tests ===\n");

    sts_init();
    mm_init(NULL);
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    CHECK(nsfevt_init() == 0, "nsfevt_init (EVT pool)");
    mm_init_complete();
    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup");

    test_dispatch();
    test_termapi_teardown();
    test_app_full();
    test_caller_identity();
    test_app_classify_and_report();
#ifndef __MVS__
    test_roundtrip();
    test_lost_request();
#endif

    /* Final leak gate: nothing outstanding across the whole suite. */
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open");
    CHECK_EQ((long)sock_inuse(), 0L, "SOCKET pool fully returned");
    CHECK_EQ((long)bufsmall_inuse(), 0L, "BUFSMALL pool fully returned");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "EVT pool fully returned");

    mm_shutdown();
    return mbt_test_summary("TSTREQ");
}
