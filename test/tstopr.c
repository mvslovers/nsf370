/*
 * tstopr.c -- NSFOPR operator dispatcher + loop-integration tests (spec 5.4/17).
 *
 * Dual (host + MVS). The always-available assertions -- the tokenizer, the
 * charset-transparent upcase, nsf_trace_flag, TRACE toggling nsftrc_flags, and
 * the STOP return value -- run in BOTH builds, so the dispatcher's EBCDIC
 * behaviour is exercised on 3.8j too. The assertions that read WTO output back
 * (nsfmsg capture ring) or inject commands (host CIB seam) are NSF_DEBUG-only
 * scaffolding, so they are compiled only on the host (make test-host,
 * -DNSF_DEBUG=1); on MVS those replies go to the real console and the loop path
 * is the PROC-driven operator validation (jcl/NSFPROC.jcl).
 */
#include "nsfopr.h"
#include "nsfmsg.h"
#include "nsftrc.h"
#include "nsfsts.h"
#include "nsfevt.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

/* NSF_DEBUG gates the WTO-capture / host-inject scaffolding (host shims only);
 * default it off so this file also compiles on the MVS test build. */
#ifndef NSF_DEBUG
#define NSF_DEBUG 0
#endif

#if NSF_DEBUG
/* True if any captured nsfmsg line contains `needle` (host capture ring). */
/* SWAP verb hook stub (issue #64, step 64-3-1).  Counts calls and records the
 * operand, so the test can prove the verb reaches the handler AND that it is
 * inert when unregistered -- the Phase-1 red line, since only Phase 2 may
 * issue SYSEVENT. */
static int  g_swapcalls;
static char g_swaparg[32];

/* APPS verb hook stub (M5-2c1).  Same shape and the same red line: only a
 * build with a client-liveness classifier may answer this verb, so an
 * unregistered APPS must be indistinguishable from a typo. */
static int g_appscalls;

static void apps_stub(void)
{
    g_appscalls++;
}

static void swap_stub(const char *arg)
{
    size_t n;
    g_swapcalls++;
    g_swaparg[0] = '\0';
    if (arg != NULL) {
        n = strlen(arg);
        if (n >= sizeof(g_swaparg)) {
            n = sizeof(g_swaparg) - 1u;
        }
        memcpy(g_swaparg, arg, n);
        g_swaparg[n] = '\0';
    }
}

/* Count matching lines, not just "is one present" -- issue #92 is precisely a
 * case where SOME lines are there and the rest are silently gone, so presence
 * cannot distinguish a complete reply from a truncated one. */
static UINT cap_count(const char *needle)
{
    UINT n = nsfmsg_cap_count();
    UINT i, hits = 0u;

    for (i = 0u; i < n; i++) {
        const char *line = nsfmsg_cap_line(i);
        if (line != NULL && strstr(line, needle) != NULL) {
            hits++;
        }
    }
    return hits;
}

static int cap_has(const char *needle)
{
    UINT n = nsfmsg_cap_count();
    UINT i;

    for (i = 0u; i < n; i++) {
        const char *line = nsfmsg_cap_line(i);
        if (line != NULL && strstr(line, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}
#endif

int main(void)
{
    printf("=== nsf370 NSFOPR tests ===\n");

    nsftrc_init();
    sts_init();

    /* ---- always-available: tokenizer + TRACE + STOP (host AND MVS) ------ */

    /* TRACE comp ON/OFF toggles nsftrc_flags (nsftrc_flags is always public). */
    CHECK((nsftrc_flags & TRCF_IP) == 0u, "IP trace initially off");
    nsfopr_dispatch("TRACE IP ON");
    CHECK((nsftrc_flags & TRCF_IP) != 0u, "TRACE IP ON sets the IP flag");
    nsfopr_dispatch("TRACE IP OFF");
    CHECK((nsftrc_flags & TRCF_IP) == 0u, "TRACE IP OFF clears the IP flag");

    /* lower-case + ALL: proves the upcase and the ALL mapping on both code pages. */
    nsfopr_dispatch("trace all on");
    CHECK((nsftrc_flags & TRCF_ALL) == TRCF_ALL, "TRACE ALL ON sets every flag");
    nsfopr_dispatch("trace all off");
    CHECK((nsftrc_flags & TRCF_ALL) == 0u, "TRACE ALL OFF clears every flag");

    /* STOP returns 1 (stop requested); other verbs return 0. */
    CHECK_EQ(nsfopr_dispatch("DISPLAY"), 0, "DISPLAY returns 0 (no stop)");
    CHECK_EQ(nsfopr_dispatch("STATS"),   0, "STATS returns 0");
    CHECK_EQ(nsfopr_dispatch("STOP"),    1, "STOP returns 1 (stop requested)");

    /* ---- host-only: reply capture + operator path through the loop ------ */
#if NSF_DEBUG
    {
        STSCTR *c;

        nsfmsg_cap_reset();
        nsfopr_dispatch("DISPLAY");
        CHECK(cap_has("NSF800I"), "DISPLAY replies NSF800I ready line");
        CHECK(cap_has("NSF801I"), "DISPLAY replies NSF801I config summary");
        CHECK(cap_has("NSF802I"), "DISPLAY replies NSF802I trace-flags line");

        nsfmsg_cap_reset();
        nsfopr_dispatch("TRACE IP ON");
        CHECK(cap_has("NSF820I"), "TRACE ON replies NSF820I");
        nsfopr_dispatch("TRACE IP OFF");

        nsfmsg_cap_reset();
        nsfopr_dispatch("TRACE");
        CHECK(cap_has("NSF828E"), "TRACE with no operands -> NSF828E syntax");
        nsfmsg_cap_reset();
        nsfopr_dispatch("TRACE BOGUS ON");
        CHECK(cap_has("NSF829E"), "TRACE unknown component -> NSF829E");

        c = sts_register("TSTOPR", "hits");
        CHECK(c != NULL, "registered a stats counter");
        STS_ADD(c, 7);
        nsfmsg_cap_reset();
        nsfopr_dispatch("STATS");
        CHECK(cap_has("NSF810I"), "STATS replies NSF810I header");
        CHECK(cap_has("hits"),    "STATS renders the registered counter");

        nsfmsg_cap_reset();
        nsfopr_dispatch("HELP");
        CHECK(cap_has("NSF880I"), "HELP lists NSF880I command help");
        nsfmsg_cap_reset();
        nsfopr_dispatch("FROBNICATE");
        CHECK(cap_has("NSF808E"), "unknown command -> NSF808E");

        /* -- SWAP verb: unregistered is INERT (the Phase-1 guarantee) ---- */
        g_swapcalls = 0;
        nsfmsg_cap_reset();
        nsfopr_dispatch("SWAP");
        CHECK(g_swapcalls == 0, "SWAP unregistered: handler not called");
        CHECK(cap_has("NSF808E"),
              "SWAP unregistered: falls through to NSF808E like any unknown verb");
        nsfmsg_cap_reset();
        nsfopr_dispatch("HELP");
        CHECK(!cap_has("SRM swappability"),
              "SWAP unregistered: help text does not grow");

        /* -- registered: the verb reaches the handler, with its operand ---- */
        nsfopr_set_swap(swap_stub);
        g_swapcalls = 0;
        nsfmsg_cap_reset();
        nsfopr_dispatch("SWAP");
        CHECK(g_swapcalls == 1, "SWAP registered: handler called exactly once");
        CHECK(!cap_has("NSF808E"), "SWAP registered: not an unknown command");
        nsfmsg_cap_reset();
        nsfopr_dispatch("swap probe");
        CHECK(g_swapcalls == 2, "SWAP is case-folded like every other verb");
        CHECK(strcmp(g_swaparg, "PROBE") == 0,
              "SWAP passes the rest of the line as the operand");
        nsfmsg_cap_reset();
        nsfopr_dispatch("HELP");
        CHECK(cap_has("SRM swappability"),
              "SWAP registered: help text gains the verb");

        /* -- APPS verb: unregistered is INERT (the Phase-1 guarantee) ---- */
        g_appscalls = 0;
        nsfmsg_cap_reset();
        nsfopr_dispatch("APPS");
        CHECK(g_appscalls == 0, "APPS unregistered: handler not called");
        CHECK(cap_has("NSF808E"),
              "APPS unregistered: falls through to NSF808E like any unknown verb");
        nsfmsg_cap_reset();
        nsfopr_dispatch("HELP");
        CHECK(!cap_has("app registry"),
              "APPS unregistered: help text does not grow");

        nsfopr_set_apps(apps_stub);
        g_appscalls = 0;
        nsfmsg_cap_reset();
        nsfopr_dispatch("apps");
        CHECK(g_appscalls == 1, "APPS registered: handler called exactly once");
        CHECK(!cap_has("NSF808E"), "APPS registered: not an unknown command");
        nsfmsg_cap_reset();
        nsfopr_dispatch("HELP");
        CHECK(cap_has("app registry"), "APPS registered: help text gains the verb");
        nsfopr_set_apps(NULL);
        g_appscalls = 0;
        nsfmsg_cap_reset();
        nsfopr_dispatch("APPS");
        CHECK(g_appscalls == 0, "APPS deregistered: inert again");

        /* Deregister, so the rest of the suite sees the Phase-1 shape. */
        nsfopr_set_swap(NULL);
        g_swapcalls = 0;
        nsfmsg_cap_reset();
        nsfopr_dispatch("SWAP");
        CHECK(g_swapcalls == 0, "SWAP deregistered: inert again");

        nsfmsg_cap_reset();
        nsfopr_dispatch("STOP");
        CHECK(cap_has("NSF830I"), "STOP replies NSF830I accepted");
    }

    /* operator path THROUGH the real loop: inject DISPLAY + STOP, run the loop. */
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init creates the EVT pool");
    mm_init_complete();

    CHECK_EQ(nsfopr_init(), 0, "nsfopr_init (host CIB seam) succeeds");
    evt_set_operator(nsfopr_ecb(), nsfopr_drain);

    nsfmsg_cap_reset();
    nsfopr_host_cmd("DISPLAY");          /* queue a MODIFY + POST the console ECB */
    nsfopr_host_stop();                  /* queue a STOP (P NSF)                  */
    evt_mainloop();                      /* wake -> drain -> dispatch -> shutdown */

    CHECK(cap_has("NSF800I"), "loop drained + dispatched the injected DISPLAY");
    CHECK(cap_has("NSF830I"), "loop drained + dispatched the injected STOP");
    CHECK_EQ((long)nsfevt_inuse(), 0,
             "EVT pool at baseline after operator-driven shutdown (leak gate)");


    /* ================================================================
     * issue #92 -- STATS must render EVERY counter, AT WIDE VALUES, and
     * must SAY SO if it ever cannot.
     *
     * DISCRIMINATING BY CONSTRUCTION.  The old renderer put one
     * sts_render into one 512-byte buffer and stopped at the last whole
     * line that fitted -- about 32 of 52 counters.  Registering more than
     * that boundary and asserting the rendered COUNT equals sts_count()
     * fails on the unfixed code and passes on the fixed code.
     *
     * AND AT WIDE VALUES, because the boundary MOVED: every line carries
     * the counter's value ("%u"), so ten-digit values make each line
     * longer and push MORE counters off the end.  A fix that works at
     * small values and fails at large ones is the same defect at a new
     * threshold, so the values here are driven to UINT_MAX first.
     * ================================================================ */
    {
        enum { N92 = 52 };              /* the real NSFS registry size      */
        UINT    i;
        STSCTR *c92;
        UINT    rendered;

        sts_init();
        for (i = 0u; i < (UINT)N92; i++) {
            char nm[13];
            nm[0] = 'c'; nm[1] = (char)('0' + (int)(i / 10u));
            nm[2] = (char)('0' + (int)(i % 10u)); nm[3] = '\0';
            c92 = sts_register("NSFWIDE", nm);
            CHECK(c92 != NULL, "issue #92: counter registered");
            if (c92 != NULL) {
                c92->value = 4294967295u;   /* WIDEST possible line */
            }
        }
        CHECK_EQ((long)sts_count(), (long)N92,
                 "issue #92: all 52 counters registered");

        nsfmsg_cap_reset();
        nsfopr_dispatch("STATS");
        rendered = cap_count("NSF811I");

        /* THE assertion.  On the unfixed renderer this reads ~24 (fewer than
         * the ~32 of narrow values, because the wide values lengthen every
         * line) and the test goes red. */
        CHECK_EQ((long)rendered, (long)N92,
                 "issue #92: EVERY counter rendered, at ten-digit values");
        CHECK(cap_has("NSF810I STATS 52"),
              "issue #92: the header still reports the true total");
        CHECK(!cap_has("NSF818W"),
              "issue #92: no truncation warning when the reply is complete");
        CHECK_EQ((long)nsfmsg_cap_dropped(), 0L,
                 "issue #92: the host capture retained every emitted line");
    }

    /* The visibility half, proven to FIRE.  A renderer that can drop output
     * must be able to report that it did -- otherwise the next counter added
     * past some future boundary is lost in the same silence that produced #92.
     * sts_render_from with a buffer too small for even one line makes no
     * progress, which is the one case the loop cannot resolve, so the reply is
     * incomplete and must say so. */
    {
        char buf[4];
        UINT next = 7u;
        UINT n    = sts_render_from(buf, sizeof(buf), 0u, &next);

        CHECK_EQ((long)n, 0L, "issue #92: no-progress render writes nothing");
        CHECK_EQ((long)next, 0L,
                 "issue #92: no-progress render reports next == first, so a"
                 " caller breaks instead of spinning");
    }

    /* ================================================================
     * issue #92, second problem: the HOST CAPTURE RING dropped the tail.
     *
     * It kept the FIRST CAP_MAX lines and dropped the rest -- the NEWER
     * ones -- so a reply longer than the ring lost its END.  `F NSFS,APPS`
     * at a full 64-slot registry emits 1 heading + 64 slots + 1 summary =
     * 66 lines, so the last slot AND the NSF816I summary went missing, and
     * nsfmsg_cap_line() returned NULL for them, which reads as "no such
     * line" rather than "dropped".  M5-2c1 stage b worked around it by
     * reading the registry through nsfreq_app_info instead of the report.
     *
     * Reproduced at the shape that broke it: 66 lines, the last one a
     * summary, asserting the SUMMARY is retrievable and nothing was
     * dropped.  At CAP_MAX 64 the summary is line 66 and this fails.
     * ================================================================ */
    {
        UINT i;

        nsfmsg_cap_reset();
        nsfmsg("NSF814I APP REGISTRY:");
        for (i = 0u; i < 64u; i++) {
            nsfmsg("NSF815I   SLOT %u", (unsigned)i);
        }
        nsfmsg("NSF816I APP REGISTRY: 64 OF 64 SLOTS IN USE, 0 DEAD");

        CHECK_EQ((long)nsfmsg_cap_count(), 66L,
                 "issue #92: 66 lines emitted (heading + 64 slots + summary)");
        CHECK_EQ((long)nsfmsg_cap_dropped(), 0L,
                 "issue #92: the ring dropped NOTHING -- assertable positively,"
                 " which is what it could not be before");
        CHECK(cap_has("NSF816I"),
              "issue #92: the SUMMARY line survived (it was line 66, evicted"
              " at CAP_MAX 64)");
        CHECK(cap_has("SLOT 63"),
              "issue #92: the last slot line survived too");
    }

    /* THE VISIBILITY HALF, PROVEN TO FIRE.  NSF818W cannot occur in production
     * -- the loop always makes progress -- so it is forced here by shrinking
     * the chunk below one line's width.  A warning that has never been seen to
     * fire is not designed in, it is asserted; this is the difference. */
    {
        nsfopr_set_stats_chunk(4u);     /* smaller than any rendered line */
        nsfmsg_cap_reset();
        nsfopr_dispatch("STATS");
        CHECK(cap_has("NSF818W"),
              "issue #92: an incomplete reply SAYS SO (NSF818W fires)");
        CHECK(cap_has("RENDERED 0 OF 52"),
              "issue #92: the warning names what was rendered and what exists");
        CHECK_EQ((long)cap_count("NSF811I"), 0L,
                 "issue #92: nothing rendered at a 4-byte chunk (no progress)");
        nsfopr_set_stats_chunk(0u);     /* restore the default */

        nsfmsg_cap_reset();
        nsfopr_dispatch("STATS");
        CHECK(!cap_has("NSF818W"),
              "issue #92: warning gone once the chunk is restored");
        CHECK_EQ((long)cap_count("NSF811I"), 52L,
                 "issue #92: and all 52 render again");
    }

    /* Resumption itself: two chunks must cover the registry exactly once. */
    {
        char buf[512];
        UINT first = 0u, next = 0u, seen = 0u;
        UINT guard = 0u;

        while (first < sts_count() && guard++ < 100u) {
            (void)sts_render_from(buf, sizeof(buf), first, &next);
            CHECK(next > first, "issue #92: each chunk makes progress");
            if (next <= first) break;
            seen += (next - first);
            first = next;
        }
        CHECK_EQ((long)seen, (long)sts_count(),
                 "issue #92: resumption covers every counter exactly once");
    }

    mm_shutdown();
#endif /* NSF_DEBUG */
    return mbt_test_summary("TSTOPR");
}
