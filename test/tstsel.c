/*
 * tstsel.c -- NSFSEL SELECT engine unit tests (spec 10.3, ADR-0035).
 *
 * DIRECT-CALL, no threads, no event loop: the SELECT engine is executive-side,
 * single-task machinery, so every path is driven deterministically -- build an
 * RQ_SELECT NSFRQE, dispatch it (nsfreq_dispatch routes it to the registered
 * engine), and drive readiness / timeout / teardown by hand:
 *   - readiness: a controllable dummy PROTOPS poll (g_ready[id]) + soc_notify_ready;
 *   - timeout:   nsftmr_run(ticks) fires the embedded timeout TMR;
 *   - teardown:  soc_destroy pokes SEL_DEAD.
 * "Parked" is detected by a sentinel RETCODE the engine only overwrites on
 * completion (soc_park sets no retcode). The FACADE mask translation (right-to-left
 * bit numbering) is tested separately, through nsf_select, in test/host/tsteza.c;
 * the REAL readiness logic (tcp_poll) is tested in test/tsttcp.c. This file proves
 * the engine mechanics: item scan, immediate/poll-form/park/poke/timeout, the
 * bounded pool (EMFILE), teardown -> ECONNABORTED, the generic poll fallback, and
 * the leak gate.
 *
 * Dual host + MVS (the engine is byte-order-agnostic): the MVS run proves it
 * cross-compiles + runs on 3.8j.
 */
#include "nsfsel.h"
#include "nsfsoc.h"
#include "nsfreq.h"
#include "nsftmr.h"
#include "nsfbuf.h"
#include "nsfsts.h"
#include "nsftrc.h"
#include "nsfmm.h"
#include "nsfque.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

/* Not a real completion -- the engine overwrites retcode only when it completes r;
 * a value left here means the request is still parked. */
#define PARKED   0x5A5A5A5A

/* ---- controllable dummy protocols ------------------------------------------ *
 * dummy_poll has a poll op driven by g_ready[SOCKCB.id]; dummy_nopoll has NO poll
 * op (poll == NULL), exercising the engine's generic fallback (read = rxq/acceptq
 * non-empty, write = always). */
static int g_ready[NSFSOC_MAX_DEFAULT];         /* per-socket readiness (by id)  */

static int d_attach(SOCKCB *s) { (void)s; return 0; }
static int d_detach(SOCKCB *s) { (void)s; return 0; }
static int d_poll(SOCKCB *s, int want) { return g_ready[s->id] & want; }

/* positional: attach,bind,connect,listen,send,recv,close,detach,accept,poll,shutdown */
static PROTOPS dummy_poll = {
    d_attach, NULL, NULL, NULL, NULL, NULL, NULL, d_detach, NULL, d_poll, NULL
};
static PROTOPS dummy_nopoll = {
    d_attach, NULL, NULL, NULL, NULL, NULL, NULL, d_detach, NULL, NULL, NULL
};

/* ---- helpers --------------------------------------------------------------- */

static void setup(void)
{
    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    (void)buf_init();
    (void)soc_reserve(0);
    mm_init_complete();
    soc_init();
    nsfreq_init();
    nsfsel_init();                          /* registers the engine + notify seam */
    memset(g_ready, 0, sizeof(g_ready));
}

static void teardown(void)
{
    mm_shutdown();
}

/* Build an RQ_SELECT request over `n` items with the given timeout encoding, then
 * dispatch it. Returns the request's retcode (PARKED if it parked). */
static INT do_select(NSFRQE *r, NSFSELITEM *items, UINT n,
                     UINT p3, UINT sec, UINT usec)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn      = (USHORT)RQ_SELECT;
    r->ubuf    = (n > 0u) ? items : NULL;
    r->ulen    = n * (UINT)sizeof(NSFSELITEM);   /* BYTES (ADR-0047)  */
    r->p1      = sec;
    r->p2      = usec;
    r->p3      = p3;
    r->retcode = PARKED;                    /* sentinel: overwritten on complete  */
    nsfreq_dispatch(r);
    return r->retcode;
}

static void item(NSFSELITEM *it, UINT desc, UCHAR want)
{
    it->desc    = desc;
    it->want    = want;
    it->ready   = 0u;
    it->rsvd[0] = 0u;
    it->rsvd[1] = 0u;
}

static UINT sel_inuse(void)
{
#if NSF_DEBUG
    return nsfsel_debug_inuse();
#else
    return 0u;
#endif
}

/* ---- scenarios ------------------------------------------------------------- */

/* S1: a socket already ready completes immediately (no park). */
static void test_immediate(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    CHECK(s != NULL, "S1 create socket");
    g_ready[s->id] = (int)SEL_READ;
    item(&it[0], soc_desc(s), (UCHAR)SEL_READ);

    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 5u, 0u);
    CHECK_EQ((long)rc, 1L, "S1 immediate ready -> RETCODE 1");
    CHECK_EQ((long)it[0].ready, (long)SEL_READ, "S1 item marked read-ready");
    CHECK_EQ((long)sel_inuse(), 0L, "S1 nothing parked");

    soc_destroy(s);
}

/* S2: the generic poll fallback (NULL poll) -- rxq non-empty is read-ready, write
 * is always ready. */
static void test_fallback(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_DGRAM, 17, &dummy_nopoll);
    PBUF      *b;
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    CHECK(s != NULL, "S2 create socket");

    /* write is always ready under the fallback */
    item(&it[0], soc_desc(s), (UCHAR)SEL_WRITE);
    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 0u, 0u);
    CHECK_EQ((long)rc, 1L, "S2 fallback write always ready");

    /* read is NOT ready on an empty rxq (poll form -> immediate 0) */
    item(&it[0], soc_desc(s), (UCHAR)SEL_READ);
    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 0u, 0u);
    CHECK_EQ((long)rc, 0L, "S2 fallback read not ready on empty rxq");

    /* queue a PBUF -> read-ready */
    b = buf_alloc(0u);
    CHECK(b != NULL, "S2 buf_alloc");
    CHECK_EQ((long)q_enq(&s->rxq, &b->q), 0L, "S2 enqueue rxq");
    item(&it[0], soc_desc(s), (UCHAR)SEL_READ);
    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 0u, 0u);
    CHECK_EQ((long)rc, 1L, "S2 fallback read ready when rxq non-empty");

    soc_destroy(s);                         /* flushes the rxq PBUF               */
}

/* S3: poll form (finite 0/0 timeout) with nothing ready -> immediate RETCODE 0. */
static void test_pollform(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    item(&it[0], soc_desc(s), (UCHAR)(SEL_READ | SEL_WRITE));
    g_ready[s->id] = 0;
    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 0u, 0u);
    CHECK_EQ((long)rc, 0L, "S3 poll form, nothing ready -> RETCODE 0");
    CHECK_EQ((long)sel_inuse(), 0L, "S3 not parked");

    soc_destroy(s);
}

/* S4: park (finite timeout, nothing ready), then a readiness poke completes it. */
static void test_park_poke(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    g_ready[s->id] = 0;
    item(&it[0], soc_desc(s), (UCHAR)SEL_WRITE);
    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 5u, 0u);
    CHECK_EQ((long)rc, (long)PARKED, "S4 parked (nothing ready, timed)");
    CHECK_EQ((long)sel_inuse(), 1L, "S4 one SELECT parked");

    /* a poke that finds it STILL not ready leaves it parked (the concurrent-recv
     * rule: readiness reflects current state, not the poke's optimism). */
    soc_notify_ready(s, (UCHAR)SEL_WRITE);
    CHECK_EQ((long)r.retcode, (long)PARKED, "S4 poke with not-ready -> still parked");
    CHECK_EQ((long)sel_inuse(), 1L, "S4 still parked");

    /* now make it ready and poke -> completes */
    g_ready[s->id] = (int)SEL_WRITE;
    soc_notify_ready(s, (UCHAR)SEL_WRITE);
    CHECK_EQ((long)r.retcode, 1L, "S4 poke with ready -> RETCODE 1");
    CHECK_EQ((long)it[0].ready, (long)SEL_WRITE, "S4 item write-ready");
    CHECK_EQ((long)sel_inuse(), 0L, "S4 slot freed on completion");

    soc_destroy(s);
}

/* S5: timeout fires -> RETCODE 0, all masks zeroed, slot freed. */
static void test_timeout(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    g_ready[s->id] = 0;
    item(&it[0], soc_desc(s), (UCHAR)SEL_READ);
    rc = do_select(&r, it, 1u, (UINT)SEL_F_TIMED, 1u, 0u);   /* 1 s = 10 ticks     */
    CHECK_EQ((long)rc, (long)PARKED, "S5 parked");
    CHECK(nsftmr_count() >= 1u, "S5 timeout timer armed");

    nsftmr_run(10u);                        /* advance 1 s -> fire the timeout     */
    CHECK_EQ((long)r.retcode, 0L, "S5 timeout -> RETCODE 0");
    CHECK_EQ((long)it[0].ready, 0L, "S5 masks zeroed on timeout");
    CHECK_EQ((long)sel_inuse(), 0L, "S5 slot freed on timeout");

    soc_destroy(s);
}

/* S6: pure timer (no items) -- parks, fires on timeout. */
static void test_pure_timer(void)
{
    NSFRQE r;
    INT    rc;

    rc = do_select(&r, NULL, 0u, (UINT)SEL_F_TIMED, 0u, 200000u);  /* 0.2 s -> 2 ticks */
    CHECK_EQ((long)rc, (long)PARKED, "S6 pure timer parks");
    nsftmr_run(2u);
    CHECK_EQ((long)r.retcode, 0L, "S6 pure timer -> RETCODE 0");
    CHECK_EQ((long)sel_inuse(), 0L, "S6 slot freed");
}

/* S7: block-forever (no timeout flag) parks with NO timer, completes on a poke. */
static void test_forever(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    g_ready[s->id] = 0;
    item(&it[0], soc_desc(s), (UCHAR)SEL_READ);
    rc = do_select(&r, it, 1u, 0u, 0u, 0u);                 /* no SEL_F_TIMED      */
    CHECK_EQ((long)rc, (long)PARKED, "S7 forever parks");
    CHECK_EQ((long)nsftmr_count(), 0L, "S7 no timeout timer armed");

    g_ready[s->id] = (int)SEL_READ;
    soc_notify_ready(s, (UCHAR)SEL_READ);
    CHECK_EQ((long)r.retcode, 1L, "S7 poke completes a forever select");
    CHECK_EQ((long)sel_inuse(), 0L, "S7 slot freed");

    soc_destroy(s);
}

/* S8: the bounded pool -- a NSFSEL_MAX+1'th concurrently parked SELECT -> EMFILE. */
static void test_emfile(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r[NSFSEL_MAX + 1];
    NSFSELITEM it[NSFSEL_MAX + 1];
    int        i;

    g_ready[s->id] = 0;
    for (i = 0; i < NSFSEL_MAX; i++) {
        item(&it[i], soc_desc(s), (UCHAR)SEL_READ);
        (void)do_select(&r[i], &it[i], 1u, 0u, 0u, 0u);     /* forever -> parks    */
        CHECK_EQ((long)r[i].retcode, (long)PARKED, "S8 parked");
    }
    CHECK_EQ((long)sel_inuse(), (long)NSFSEL_MAX, "S8 pool full");

    item(&it[NSFSEL_MAX], soc_desc(s), (UCHAR)SEL_READ);
    (void)do_select(&r[NSFSEL_MAX], &it[NSFSEL_MAX], 1u, 0u, 0u, 0u);
    CHECK_EQ((long)r[NSFSEL_MAX].retcode, (long)NSF_RETERR, "S8 overflow -> RETCODE -1");
    CHECK_EQ((long)r[NSFSEL_MAX].errno_, (long)NSF_EMFILE, "S8 overflow -> EMFILE");

    /* drain the parked ones (destroy the socket -> SEL_DEAD aborts them all) */
    soc_destroy(s);
    CHECK_EQ((long)sel_inuse(), 0L, "S8 all parked selects freed by teardown");
}

/* S9: teardown while parked -> the SELECT completes NSF_ECONNABORTED (no hang).
 * This is also the non-blocking-CONNECT-failure shape: a refused connect destroys
 * the socket (tcp_close_done -> soc_destroy), and the SEL_DEAD poke aborts a
 * SELECT-for-write parked on it. */
static void test_teardown(void)
{
    SOCKCB    *s = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    NSFRQE     r;
    NSFSELITEM it[1];
    INT        rc;

    g_ready[s->id] = 0;
    item(&it[0], soc_desc(s), (UCHAR)SEL_WRITE);
    rc = do_select(&r, it, 1u, 0u, 0u, 0u);                 /* forever park        */
    CHECK_EQ((long)rc, (long)PARKED, "S9 parked");

    soc_destroy(s);                         /* SEL_DEAD poke */
    CHECK_EQ((long)r.retcode, (long)NSF_RETERR, "S9 teardown -> RETCODE -1");
    CHECK_EQ((long)r.errno_, (long)NSF_ECONNABORTED, "S9 teardown -> ECONNABORTED");
    CHECK_EQ((long)sel_inuse(), 0L, "S9 slot freed by teardown");
}

/* S10: multi-socket -- only the ready one counts; a stale descriptor in the set
 * contributes nothing. */
static void test_multi(void)
{
    SOCKCB    *s0 = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    SOCKCB    *s1 = soc_create(NSF_AF_INET, NSF_SOCK_STREAM, 6, &dummy_poll);
    UINT       stale;
    NSFRQE     r;
    NSFSELITEM it[3];
    INT        rc;

    stale = soc_desc(s1);
    g_ready[s0->id] = (int)SEL_WRITE;
    g_ready[s1->id] = (int)SEL_READ;

    item(&it[0], soc_desc(s0), (UCHAR)SEL_READ);            /* s0 wants read: not ready*/
    item(&it[1], soc_desc(s0), (UCHAR)SEL_WRITE);           /* s0 write: ready         */
    item(&it[2], stale,        (UCHAR)SEL_READ);            /* placeholder             */

    /* close s1, so its descriptor in it[2] becomes stale (skipped). */
    soc_destroy(s1);
    item(&it[2], stale, (UCHAR)SEL_READ);

    rc = do_select(&r, it, 3u, (UINT)SEL_F_TIMED, 0u, 0u);
    CHECK_EQ((long)rc, 1L, "S10 exactly one ready (s0 write)");
    CHECK_EQ((long)it[0].ready, 0L, "S10 s0 read not ready");
    CHECK_EQ((long)it[1].ready, (long)SEL_WRITE, "S10 s0 write ready");
    CHECK_EQ((long)it[2].ready, 0L, "S10 stale desc contributes nothing");

    soc_destroy(s0);
}

/* S11 (M5-2d1): A FOREIGN DESCRIPTOR IN THE MASK.
 *
 * This is the widest door the ownership check had to cover: one SELECT resolves
 * ONE DESCRIPTOR PER MASK ITEM, so a client that can put arbitrary numbers in a
 * mask probes many descriptors per request. A check on the per-verb path alone
 * would have left it open.
 *
 * The contract asserted here is deliberately NOT "SELECT returns an error":
 *   - the foreign entry is silently NOT READY,
 *   - the REST OF THE MASK IS SERVED NORMALLY,
 *   - and that is byte-for-byte what a closed or never-existing descriptor
 *     already did, so the two cannot be told apart.
 * Anything louder would turn SELECT into an existence oracle for descriptor
 * numbers -- which is worth more to an attacker than the SELECT itself. */
#define S11_ASCB_A  0x00F10000u
#define S11_ASID_A  0x0011u
#define S11_ASCB_B  0x00F20000u
#define S11_ASID_B  0x0012u

static UINT s11_open(UINT ascb, UINT asid, UINT *tok, const char *who)
{
    NSFRQE r;

    memset(&r, 0, sizeof(r));
    memcpy(r.eye, NSFRQE_EYE, 4);
    r.fn = (USHORT)RQ_INITAPI;
    nsfreq_dispatch_id(&r, ascb, asid);
    CHECK_EQ((long)r.retcode, (long)NSF_RETOK, who);
    *tok = r.apptok;

    memset(&r, 0, sizeof(r));
    memcpy(r.eye, NSFRQE_EYE, 4);
    r.fn = (USHORT)RQ_SOCKET;
    r.apptok = *tok;
    r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_STREAM; r.p3 = 6u;
    nsfreq_dispatch_id(&r, ascb, asid);
    CHECK(r.retcode >= 0, "the app opened a socket");
    return (UINT)r.retcode;
}

static void test_ownership_mask(void)
{
    NSFRQE     r;
    NSFSELITEM it[3];
    UINT       tokA, tokB, dA, dB;
    SOCKCB    *sa;
    INT        rc_foreign, rc_unknown;

    CHECK_EQ((long)nsfreq_register_proto(6u, &dummy_poll), 0L,
             "register the pollable protocol");

    dA = s11_open(S11_ASCB_A, S11_ASID_A, &tokA, "A: INITAPI");
    dB = s11_open(S11_ASCB_B, S11_ASID_B, &tokB, "B: INITAPI");
    CHECK(dA != dB, "A and B hold different descriptors");

    /* Make BOTH sockets genuinely ready, so "not ready" can only come from the
     * ownership check and never from the socket simply having nothing to say. */
    sa = sock_lookup(dA);
    CHECK(sa != NULL, "A's socket resolves");
    if (sa != NULL) g_ready[sa->id] = (int)SEL_READ;
    sa = sock_lookup(dB);
    CHECK(sa != NULL, "B's socket resolves");
    if (sa != NULL) g_ready[sa->id] = (int)SEL_READ;

    /* B selects on: its own socket, A's socket, and a number nobody owns. */
    item(&it[0], dB, (UCHAR)SEL_READ);
    item(&it[1], dA, (UCHAR)SEL_READ);
    item(&it[2], 0x00BADBADu, (UCHAR)SEL_READ);

    memset(&r, 0, sizeof(r));
    memcpy(r.eye, NSFRQE_EYE, 4);
    r.fn      = (USHORT)RQ_SELECT;
    r.ubuf    = it;
    r.ulen    = 3u * (UINT)sizeof(NSFSELITEM);  /* BYTES (ADR-0047)  */
    r.p3      = SEL_F_TIMED;                /* poll form: 0/0 -> never parks     */
    r.retcode = PARKED;
    nsfreq_dispatch_id(&r, S11_ASCB_B, S11_ASID_B);

    CHECK_EQ((long)r.retcode, 1L, "SELECT counted exactly ONE ready socket");
    CHECK_EQ((long)r.errno_, 0L, "SELECT returned no error for the foreign entry");
    CHECK_EQ((long)it[0].ready, (long)SEL_READ, "B's OWN socket was served");
    rc_foreign = (INT)it[1].ready;
    rc_unknown = (INT)it[2].ready;
    CHECK_EQ((long)rc_foreign, 0L, "A's socket is silently NOT READY for B");
    CHECK_EQ((long)rc_foreign, (long)rc_unknown,
             "foreign and never-existing look identical in the mask");

    /* The control: A selecting on the SAME descriptor gets it, so the entry was
     * withheld from B specifically -- not broken for everyone. */
    item(&it[0], dA, (UCHAR)SEL_READ);
    memset(&r, 0, sizeof(r));
    memcpy(r.eye, NSFRQE_EYE, 4);
    r.fn      = (USHORT)RQ_SELECT;
    r.ubuf    = it;
    r.ulen    = 1u * (UINT)sizeof(NSFSELITEM);  /* BYTES (ADR-0047)  */
    r.p3      = SEL_F_TIMED;
    r.retcode = PARKED;
    nsfreq_dispatch_id(&r, S11_ASCB_A, S11_ASID_A);
    CHECK_EQ((long)r.retcode, 1L, "A selecting on A's own socket: ready");
    CHECK_EQ((long)it[0].ready, (long)SEL_READ, "...and the bit is set");

    /* THE PARKED RE-SCAN PATH (M5-2d1). Everything above uses the POLL form,
     * which never parks -- so it exercises only the scan at dispatch, where the
     * identity comes from nsfreq_caller_id. The re-scan in nsfsel_on_notify runs
     * with NO DISPATCH OPEN and reads the identity captured in the SELCB; that
     * is a different code path with a different source for the same decision,
     * and without this it would be covered by nothing.
     *
     * B parks a SELECT on A's descriptor. Making A's socket ready must NOT
     * complete B's SELECT -- if the re-scan lost the owner, it would. */
    {
        NSFRQE rp;
        NSFSELITEM ip[1];

        sa = sock_lookup(dA);
        if (sa != NULL) g_ready[sa->id] = 0;        /* start not-ready          */

        item(&ip[0], dA, (UCHAR)SEL_READ);
        memset(&rp, 0, sizeof(rp));
        memcpy(rp.eye, NSFRQE_EYE, 4);
        rp.fn      = (USHORT)RQ_SELECT;
        rp.ubuf    = ip;
        rp.ulen    = 1u * (UINT)sizeof(NSFSELITEM);  /* BYTES         */
        rp.p1      = 5u;                            /* 5 s: parks              */
        rp.p3      = SEL_F_TIMED;
        rp.retcode = PARKED;
        nsfreq_dispatch_id(&rp, S11_ASCB_B, S11_ASID_B);
        CHECK_EQ((long)rp.retcode, (long)PARKED,
                 "B's SELECT on A's socket parked (foreign == not ready)");
        CHECK_EQ((long)sel_inuse(), 1L, "one SELECT parked");

        /* A's socket becomes ready and pokes. B must stay parked: the re-scan
         * runs as B, and A's socket is not B's. */
        if (sa != NULL) g_ready[sa->id] = (int)SEL_READ;
        soc_notify_ready(sa, (UCHAR)SEL_READ);
        CHECK_EQ((long)rp.retcode, (long)PARKED,
                 "A's readiness does NOT complete B's parked SELECT");
        CHECK_EQ((long)sel_inuse(), 1L, "B is still parked");

        /* THE CONTROL, AND IT IS THE SAME POKE, NOT A SIMILAR ONE. A parks its
         * own SELECT on the same descriptor while B stays parked, then ONE
         * poke goes to both: A's completes, B's does not. Without this,
         * "B stayed parked" could equally mean the poke was broken. */
        {
            NSFRQE rA;
            NSFSELITEM iA[1];

            if (sa != NULL) g_ready[sa->id] = 0;
            item(&iA[0], dA, (UCHAR)SEL_READ);
            memset(&rA, 0, sizeof(rA));
            memcpy(rA.eye, NSFRQE_EYE, 4);
            rA.fn      = (USHORT)RQ_SELECT;
            rA.ubuf    = iA;
            rA.ulen    = 1u * (UINT)sizeof(NSFSELITEM);  /* BYTES     */
            rA.p1      = 5u;
            rA.p3      = SEL_F_TIMED;
            rA.retcode = PARKED;
            nsfreq_dispatch_id(&rA, S11_ASCB_A, S11_ASID_A);
            CHECK_EQ((long)rA.retcode, (long)PARKED, "A's own SELECT parked too");
            CHECK_EQ((long)sel_inuse(), 2L, "both A and B are parked");

            if (sa != NULL) g_ready[sa->id] = (int)SEL_READ;
            soc_notify_ready(sa, (UCHAR)SEL_READ);

            CHECK_EQ((long)rA.retcode, 1L,
                     "ONE poke completes A's parked SELECT (the re-scan works)");
            CHECK_EQ((long)rp.retcode, (long)PARKED,
                     "...and the SAME poke leaves B's parked (owner is honoured)");
            CHECK_EQ((long)sel_inuse(), 1L, "only A's slot was freed");
        }

        /* Tear B's parked SELECT down through the documented path: destroying a
         * socket in a parked set pokes SEL_DEAD -> ECONNABORTED (ADR-0035). */
        soc_destroy(sock_lookup(dA));
        CHECK_EQ((long)sel_inuse(), 0L, "B's parked SELECT was torn down");
    }

    /* Phase 1 (no identity) is unaffected: the same mask, dispatched with no
     * caller, sees every socket. */
    item(&it[0], dB, (UCHAR)SEL_READ);
    memset(&r, 0, sizeof(r));
    memcpy(r.eye, NSFRQE_EYE, 4);
    r.fn      = (USHORT)RQ_SELECT;
    r.ubuf    = it;
    r.ulen    = 1u * (UINT)sizeof(NSFSELITEM);  /* BYTES (ADR-0047)  */
    r.p3      = SEL_F_TIMED;
    r.retcode = PARKED;
    nsfreq_dispatch(&r);                    /* == dispatch_id(r, 0, 0) */
    CHECK_EQ((long)r.retcode, 1L,
             "Phase 1 sees B's socket without naming any caller (check inert)");
}

int main(void)
{
    printf("=== nsf370 NSFSEL (SELECT engine) tests ===\n");

    setup();  test_immediate();   teardown();
    setup();  test_fallback();    teardown();
    setup();  test_pollform();    teardown();
    setup();  test_park_poke();   teardown();
    setup();  test_timeout();     teardown();
    setup();  test_pure_timer();  teardown();
    setup();  test_forever();     teardown();
    setup();  test_emfile();      teardown();
    setup();  test_teardown();    teardown();
    setup();  test_multi();       teardown();
    setup();  test_ownership_mask(); teardown();

    return mbt_test_summary("TSTSEL");
}
