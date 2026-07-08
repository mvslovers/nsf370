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

    mm_shutdown();
#endif /* NSF_DEBUG */

    return mbt_test_summary("TSTOPR");
}
