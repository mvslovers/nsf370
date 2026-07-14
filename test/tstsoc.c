/*
 * tstsoc.c -- NSFSOC socket-layer host unit tests (spec ch. 10, M3-1).
 *
 * White-box, direct-call tests over a TEST-ONLY dummy protocol (a PROTOPS whose
 * callbacks record which fired), with no event loop and no real transport -- the
 * socket object model is exercised as plain executive-side functions:
 *
 *   - table:      alloc to capacity, EMFILE past it, free returns slots;
 *   - descriptor: (gen<<16)|id, stale-after-close -> EBADF, gen bump, reuse
 *                 yields a different descriptor (the use-after-free guard);
 *   - dispatch:   each PROTOPS callback fires for its request; missing op ->
 *                 EOPNOTSUPP; unknown fn -> EINVAL;
 *   - park/complete: a blocking RECV on an empty rxq parks (app ecb NOT posted);
 *                 soc_complete posts it, sets retcode/errno, clears the slot;
 *                 non-blocking completes immediately with EWOULDBLOCK;
 *   - soc_destroy: the ONE teardown checklist -- detach, flush rxq PBUFs,
 *                 complete parked NSFRQEs with an error, bump gen, free -- with a
 *                 leak gate (SOCKET + BUF pools back to baseline) and a per-slot
 *                 sweep proving no pend slot is skipped.
 *
 * The completion ecb is posted by soc_complete via the same-AS POST seam; here
 * there is no WAITing app task, so the test just checks the POSTED bit.
 */
#include "nsfsoc.h"
#include "nsfreq.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsfque.h"
#include "nsfsts.h"
#include "nsfevtp.h"            /* NSFECB_POSTED */
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

/* ---- test-only dummy protocol ----------------------------------------------
 * Records which callback fired. recv() embodies the real parked-request pattern
 * (satisfy from rxq / park when blocking / EWOULDBLOCK when non-blocking) so the
 * park/complete path is provable through dispatch; the other callbacks are pure
 * recorders. */
typedef struct dummyrec {
    int attach, bind, connect, listen, send, recv, close, detach;
    int last_backlog;
    int fail_attach;                    /* when set, attach returns non-zero */
} DUMMYREC;
static DUMMYREC g_d;

static int d_attach(SOCKCB *s)             { (void)s; g_d.attach++;  return g_d.fail_attach ? -1 : 0; }
static int d_bind(SOCKCB *s)               { (void)s; g_d.bind++;    return 0; }
static int d_connect(SOCKCB *s, NSFRQE *r) { (void)s; (void)r; g_d.connect++; return 0; }
static int d_listen(SOCKCB *s, int bl)     { (void)s; g_d.listen++;  g_d.last_backlog = bl; return 0; }
static int d_send(SOCKCB *s, NSFRQE *r)    { (void)s; (void)r; g_d.send++; return 0; }
static int d_close(SOCKCB *s, NSFRQE *r)   { (void)s; (void)r; g_d.close++; return 0; }
static int d_detach(SOCKCB *s)             { (void)s; g_d.detach++;  return 0; }

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
    return soc_park(s, r, SOC_PEND_RECV);           /* empty + blocking: park     */
}

static PROTOPS dummy_ops = {
    d_attach, d_bind, d_connect, d_listen, d_send, d_recv, d_close, d_detach
};

/* A protocol missing bind() -- proves the null-callback path maps to EOPNOTSUPP. */
static PROTOPS partial_ops = {
    d_attach, NULL, d_connect, d_listen, d_send, d_recv, d_close, d_detach
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

/* ---- table: capacity, EMFILE, free returns slots --------------------------- */
static void test_table(void)
{
    SOCKCB *arr[NSFSOC_MAX_DEFAULT];
    SOCKCB *s;
    UINT    i, base, nofd0;

    soc_init();
    base = sock_inuse();

    for (i = 0u; i < NSFSOC_MAX_DEFAULT; i++) {
        arr[i] = sock_alloc();
        if (arr[i] == NULL) {
            break;
        }
    }
    CHECK_EQ((long)i, (long)NSFSOC_MAX_DEFAULT, "sock_alloc fills the whole table");
    CHECK_EQ((long)soc_count(), (long)NSFSOC_MAX_DEFAULT, "soc_count == capacity");

    nofd0 = sts_value("NSFSOC", "nofd");
    s = sock_alloc();
    CHECK(s == NULL, "sock_alloc past capacity returns NULL (EMFILE)");
    CHECK_EQ((long)sts_value("NSFSOC", "nofd"), (long)nofd0 + 1, "EMFILE counted (NSFSOC nofd)");

    for (i = 0u; i < NSFSOC_MAX_DEFAULT; i++) {
        soc_destroy(arr[i]);
    }
    CHECK_EQ((long)soc_count(), 0L, "every slot freed after destroy");
    CHECK_EQ((long)sock_inuse(), (long)base, "SOCKET pool back to baseline");

    s = sock_alloc();
    CHECK(s != NULL, "sock_alloc succeeds again after freeing the table");
    soc_destroy(s);
}

/* ---- descriptor: gen<<16|id, stale -> EBADF, reuse differs ------------------ */
static void test_descriptor(void)
{
    SOCKCB *s1, *s2;
    UINT    d1, d2, slot;
    USHORT  g1;

    soc_init();
    s1 = sock_alloc();
    CHECK(s1 != NULL, "alloc s1");
    d1   = soc_desc(s1);
    g1   = s1->gen;
    slot = s1->id;
    CHECK(sock_lookup(d1) == s1, "live descriptor resolves to its socket");
    CHECK(sock_lookup(0u) == NULL, "descriptor 0 is never valid");

    soc_destroy(s1);
    CHECK(sock_lookup(d1) == NULL, "stale descriptor after close -> NULL (EBADF)");

    s2 = sock_alloc();
    CHECK(s2 != NULL, "alloc s2");
    CHECK_EQ((long)s2->id, (long)slot, "s2 reuses the same table slot");
    d2 = soc_desc(s2);
    CHECK(d2 != d1, "reused slot yields a DIFFERENT descriptor");
    CHECK_EQ((long)s2->gen, (long)(USHORT)(g1 + 1u), "generation incremented by one");
    CHECK(sock_lookup(d2) == s2, "the new descriptor resolves");
    CHECK(sock_lookup(d1) == NULL, "the old descriptor stays invalid after reuse");

    soc_destroy(s2);
}

/* ---- dispatch: each callback fires for its request ------------------------- */
static void test_dispatch(void)
{
    SOCKCB *s, *sp;
    NSFRQE  r;
    UINT    d;

    soc_init();
    memset(&g_d, 0, sizeof(g_d));

    s = soc_create(NSF_AF_INET, NSF_SOCK_DGRAM, 17, &dummy_ops);
    CHECK(s != NULL, "soc_create");
    CHECK_EQ((long)g_d.attach, 1L, "attach fired at create");
    d = soc_desc(s);

    rqe_init(&r, RQ_BIND, d, 0u);
    CHECK_EQ((long)soc_dispatch(s, &r), 0L, "dispatch bind -> rc 0");
    CHECK_EQ((long)g_d.bind, 1L, "bind callback fired");

    rqe_init(&r, RQ_CONNECT, d, 0u);
    (void)soc_dispatch(s, &r);
    CHECK_EQ((long)g_d.connect, 1L, "connect callback fired");

    rqe_init(&r, RQ_LISTEN, d, 0u);
    r.p1 = 5u;
    (void)soc_dispatch(s, &r);
    CHECK_EQ((long)g_d.listen, 1L, "listen callback fired");
    CHECK_EQ((long)g_d.last_backlog, 5L, "listen backlog passed through");

    rqe_init(&r, RQ_SEND, d, 0u);
    (void)soc_dispatch(s, &r);
    CHECK_EQ((long)g_d.send, 1L, "send callback fired");

    rqe_init(&r, RQ_RECV, d, RQ_F_NONBLOCK);         /* non-blocking: no park */
    (void)soc_dispatch(s, &r);
    CHECK_EQ((long)g_d.recv, 1L, "recv callback fired");

    rqe_init(&r, RQ_CLOSE, d, 0u);
    (void)soc_dispatch(s, &r);
    CHECK_EQ((long)g_d.close, 1L, "close callback fired");

    rqe_init(&r, 4242u, d, 0u);
    CHECK_EQ((long)soc_dispatch(s, &r), (long)NSF_EINVAL, "unknown fn -> EINVAL");

    soc_destroy(s);
    CHECK_EQ((long)g_d.detach, 1L, "detach fired at destroy");

    /* A protocol missing an op -> EOPNOTSUPP (no callback fired). */
    soc_init();
    memset(&g_d, 0, sizeof(g_d));
    sp = soc_create(NSF_AF_INET, NSF_SOCK_DGRAM, 17, &partial_ops);
    CHECK(sp != NULL, "soc_create (partial ops)");
    rqe_init(&r, RQ_BIND, soc_desc(sp), 0u);
    CHECK_EQ((long)soc_dispatch(sp, &r), (long)NSF_EOPNOTSUPP, "missing bind op -> EOPNOTSUPP");
    CHECK_EQ((long)g_d.bind, 0L, "no bind callback fired for the missing op");
    soc_destroy(sp);
}

/* ---- park / complete ------------------------------------------------------- */
static void test_park_complete(void)
{
    SOCKCB *s;
    NSFRQE  r, r2, r3, r4;
    UINT    d;

    soc_init();
    memset(&g_d, 0, sizeof(g_d));
    s = soc_create(NSF_AF_INET, NSF_SOCK_DGRAM, 17, &dummy_ops);
    d = soc_desc(s);

    /* blocking recv on empty rxq -> parked, app ecb NOT posted */
    rqe_init(&r, RQ_RECV, d, 0u);
    CHECK_EQ((long)soc_dispatch(s, &r), 0L, "blocking recv on empty rxq dispatched");
    CHECK(!posted(&r), "parked recv: app ecb NOT posted");
    CHECK(s->pend_recv == &r, "recv parked in pend_recv");

    /* completion posts it, sets retcode/errno, clears the slot */
    soc_complete(&r, 42, 0);
    CHECK(posted(&r), "after complete: app ecb posted");
    CHECK_EQ((long)r.retcode, 42L, "retcode set by completion");
    CHECK_EQ((long)r.errno_, 0L, "errno 0 on success");
    CHECK(s->pend_recv == NULL, "pend_recv slot cleared by completion");

    /* non-blocking recv on empty rxq -> immediate EWOULDBLOCK, not parked */
    rqe_init(&r2, RQ_RECV, d, RQ_F_NONBLOCK);
    (void)soc_dispatch(s, &r2);
    CHECK(posted(&r2), "non-blocking recv completed immediately");
    CHECK_EQ((long)r2.errno_, (long)NSF_EWOULDBLOCK, "non-blocking empty -> EWOULDBLOCK");
    CHECK_EQ((long)r2.retcode, (long)NSF_RETERR, "non-blocking empty -> retcode -1");
    CHECK(s->pend_recv == NULL, "non-blocking recv did not park");

    /* a second park of the same kind is rejected (one outstanding op per kind) */
    rqe_init(&r3, RQ_RECV, d, 0u);
    rqe_init(&r4, RQ_RECV, d, 0u);
    CHECK_EQ((long)soc_park(s, &r3, SOC_PEND_RECV), 0L, "first park ok");
    CHECK_EQ((long)soc_park(s, &r4, SOC_PEND_RECV), (long)NSF_EINVAL, "double-park rejected");
    soc_complete(&r3, NSF_RETERR, NSF_ECONNABORTED);
    CHECK(s->pend_recv == NULL, "parked r3 completed, slot free again");

    soc_destroy(s);
}

/* ---- soc_destroy: full checklist + leak gate ------------------------------- */
static void test_destroy_leak(void)
{
    SOCKCB *s;
    NSFRQE  r;
    PBUF   *b1, *b2;
    UINT    d, sbase, bbase;

    sbase = sock_inuse();
    bbase = bufsmall_inuse();

    soc_init();
    memset(&g_d, 0, sizeof(g_d));
    s = soc_create(NSF_AF_INET, NSF_SOCK_DGRAM, 17, &dummy_ops);
    d = soc_desc(s);

    /* a parked RECV ... */
    rqe_init(&r, RQ_RECV, d, 0u);
    CHECK_EQ((long)soc_park(s, &r, SOC_PEND_RECV), 0L, "park a recv");

    /* ... plus queued rxq PBUFs (data that arrived, never delivered) */
    b1 = buf_alloc(16u);
    b2 = buf_alloc(16u);
    CHECK(b1 != NULL && b2 != NULL, "rxq PBUFs allocated");
    CHECK_EQ((long)q_enq(&s->rxq, &b1->q), 0L, "enqueue rxq b1");
    CHECK_EQ((long)q_enq(&s->rxq, &b2->q), 0L, "enqueue rxq b2");
#if NSF_DEBUG
    /* Positive pool-inuse deltas are observable only with the debug accounting
     * compiled in (bufsmall_inuse() is a constant 0 otherwise -- like tstip's
     * pool_inuse). The q_enq rc-0 asserts above already prove the PBUFs were
     * queued; here we additionally prove the pool actually grew, so the
     * post-destroy "== bbase" baseline below is a real free, not 0 == 0. */
    CHECK_EQ((long)bufsmall_inuse(), (long)bbase + 2, "two rxq PBUFs in use before destroy");
#endif

    /* destroy runs every checklist step */
    soc_destroy(s);
    CHECK_EQ((long)g_d.detach, 1L, "destroy step 1: detach called (timers cancelled)");
    CHECK(posted(&r), "destroy step 4: parked recv completed (ecb posted)");
    CHECK_EQ((long)r.errno_, (long)NSF_ECONNABORTED, "destroy: parked recv errno ECONNABORTED");
    CHECK_EQ((long)r.retcode, (long)NSF_RETERR, "destroy: parked recv retcode -1");
    CHECK(sock_lookup(d) == NULL, "destroy step 6: descriptor invalid (gen bumped)");
    CHECK_EQ((long)soc_count(), 0L, "destroy: slot vacated");
    CHECK_EQ((long)bufsmall_inuse(), (long)bbase, "destroy steps 2/3: rxq PBUFs freed (BUF baseline)");
    CHECK_EQ((long)sock_inuse(), (long)sbase, "destroy step 7: SOCKET pool baseline (LEAK GATE)");
}

/* No teardown path may skip a pend slot: destroy with each slot set in turn. */
static void check_destroy_completes_slot(UINT which, const char *m1, const char *m2)
{
    SOCKCB *s;
    NSFRQE  r;

    soc_init();
    memset(&g_d, 0, sizeof(g_d));
    s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_ops);
    rqe_init(&r, RQ_NONE, soc_desc(s), 0u);
    CHECK_EQ((long)soc_park(s, &r, which), 0L, "park in the target slot");
    soc_destroy(s);
    CHECK(posted(&r), m1);
    CHECK_EQ((long)r.errno_, (long)NSF_ECONNABORTED, m2);
}

static void test_destroy_each_pend(void)
{
    check_destroy_completes_slot(SOC_PEND_RECV,
        "destroy completes a parked RECV",    "  ... with ECONNABORTED");
    check_destroy_completes_slot(SOC_PEND_ACCEPT,
        "destroy completes a parked ACCEPT",  "  ... with ECONNABORTED");
    check_destroy_completes_slot(SOC_PEND_CONNECT,
        "destroy completes a parked CONNECT", "  ... with ECONNABORTED");
}

int main(void)
{
    printf("=== nsf370 NSFSOC tests ===\n");

    sts_init();
    mm_init(NULL);
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    mm_init_complete();
    soc_init();

    test_table();
    test_descriptor();
    test_dispatch();
    test_park_complete();
    test_destroy_leak();
    test_destroy_each_pend();

    /* Final leak gate: nothing outstanding across the whole suite. */
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open");
    CHECK_EQ((long)sock_inuse(), 0L, "SOCKET pool fully returned");
    CHECK_EQ((long)bufsmall_inuse(), 0L, "BUFSMALL pool fully returned");

    mm_shutdown();
    return mbt_test_summary("TSTSOC");
}
