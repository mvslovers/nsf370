/*
 * tststc.c -- NSF startup sequencing / NSFCFG->component-init tests (spec 14.4).
 *
 * Host-native (make test-host). Drives the portable wiring the M0-8 STC startup
 * performs, against configs produced by the real cfg_parse:
 *   1. NSFTRACE flags map into nsftrc_flags (ON enables, OFF disables);
 *   2. NSFPOOL counts resolve (present -> value, absent -> default);
 *   3. referential integrity (spec 14.5): LINK->DEVICE and HOME->LINK, with the
 *      NSF720E/NSF721E rejection messages;
 *   4. nsf_init_from_cfg wires a valid config (creates the buffer pools, leak
 *      gate) and REFUSES a broken one with no partial state (no pools created).
 */
#include "nsfstc.h"
#include "nsfcfg.h"
#include "nsftrc.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

/* The live-region leak counter is NSF_DEBUG-only (mm keeps it off in production).
 * Default NSF_DEBUG off so this file also compiles on the MVS test build, where
 * the leak-gate assertion is skipped (the alloc/free path still runs). */
#ifndef NSF_DEBUG
#define NSF_DEBUG 0
#endif

/* A fully-consistent profile exercising DEVICE/LINK/HOME/GATEWAY/NSFPOOL/NSFTRACE
 * (pool names are the real NSFMM pool names so nsf_cfg_pool_count resolves them). */
static const char VALID[] =
    "DEVICE  CTCA CTC 0E20\n"
    "LINK    LNK1 CTC 0 CTCA\n"
    "HOME    10.1.1.2 LNK1\n"
    "GATEWAY DEFAULTNET 10.1.1.1 LNK1 1500 0\n"
    "NSFPOOL BUFSMALL 100\n"
    "NSFPOOL BUFLARGE 40\n"
    "NSFTRACE IP ON\n"
    "NSFTRACE TCP OFF\n";

/* Parses fine (the parser defers refs, spec 14.5) but LINK names no DEVICE. */
static const char BAD_LINKDEV[] =
    "DEVICE CTCA CTC 0E20\n"
    "LINK   LNK1 CTC 0 NOSUCH\n"
    "HOME   10.1.1.2 LNK1\n";

/* Parses fine but HOME names no LINK. */
static const char BAD_HOMELNK[] =
    "DEVICE CTCA CTC 0E20\n"
    "LINK   LNK1 CTC 0 CTCA\n"
    "HOME   10.1.1.2 NOLINK\n";

int main(void)
{
    NSFCFG cfg;
    char   err[NSFCFG_MSGLEN];
    INT    rc;

    printf("=== nsf370 NSFSTC (startup sequencing) tests ===\n");
    nsftrc_init();

    /* ---- 1. trace-flag mapping ----------------------------------------- */
    CHECK_EQ((long)nsf_trace_flag("IP"),  (long)TRCF_IP,  "nsf_trace_flag IP");
    CHECK_EQ((long)nsf_trace_flag("ALL"), (long)TRCF_ALL, "nsf_trace_flag ALL");
    CHECK_EQ((long)nsf_trace_flag("NOPE"), 0L, "nsf_trace_flag unknown -> 0");

    CHECK_EQ(cfg_parse(VALID, (UINT)strlen(VALID), &cfg), 0,
             "VALID profile parses");

    nsftrc_enable(TRCF_TCP);             /* so the config's TCP OFF is observable */
    CHECK_EQ((long)nsf_cfg_apply_trace(&cfg), 2L,
             "apply_trace applies both NSFTRACE statements");
    CHECK((nsftrc_flags & TRCF_IP) != 0u, "NSFTRACE IP ON took effect");
    CHECK((nsftrc_flags & TRCF_TCP) == 0u, "NSFTRACE TCP OFF took effect");

    /* ---- 2. pool-count resolution -------------------------------------- */
    CHECK_EQ((long)nsf_cfg_pool_count(&cfg, "BUFSMALL", 999u), 100L,
             "NSFPOOL BUFSMALL resolves to 100");
    CHECK_EQ((long)nsf_cfg_pool_count(&cfg, "BUFLARGE", 999u), 40L,
             "NSFPOOL BUFLARGE resolves to 40");
    CHECK_EQ((long)nsf_cfg_pool_count(&cfg, "ABSENT", 77u), 77L,
             "an unsized pool falls back to the default");

    /* ---- 3. referential integrity -------------------------------------- */
    CHECK_EQ((long)nsf_cfg_check_refs(&cfg, err, sizeof(err)), (long)NSFSTC_OK,
             "VALID config passes referential integrity");

    CHECK_EQ(cfg_parse(BAD_LINKDEV, (UINT)strlen(BAD_LINKDEV), &cfg), 0,
             "BAD_LINKDEV still parses (refs deferred)");
    rc = nsf_cfg_check_refs(&cfg, err, sizeof(err));
    CHECK_EQ((long)rc, (long)NSFSTC_E_LINKDEV, "LINK->undefined DEVICE rejected");
    CHECK(strstr(err, "NSF720E") != NULL, "renders NSF720E for the link/device");

    CHECK_EQ(cfg_parse(BAD_HOMELNK, (UINT)strlen(BAD_HOMELNK), &cfg), 0,
             "BAD_HOMELNK still parses");
    rc = nsf_cfg_check_refs(&cfg, err, sizeof(err));
    CHECK_EQ((long)rc, (long)NSFSTC_E_HOMELNK, "HOME->undefined LINK rejected");
    CHECK(strstr(err, "NSF721E") != NULL, "renders NSF721E for the home/link");

    /* ---- 4. nsf_init_from_cfg: success wires pools; failure is clean ---- */

    /* Success path: a valid config creates the buffer pools sized from NSFPOOL. */
    CHECK_EQ(cfg_parse(VALID, (UINT)strlen(VALID), &cfg), 0, "re-parse VALID");
    CHECK_EQ(mm_init(NULL), 0, "mm_init opens the init window");
    rc = nsf_init_from_cfg(&cfg, err, sizeof(err));
    CHECK_EQ((long)rc, 0L, "nsf_init_from_cfg succeeds on a valid config");
    CHECK(nsf_active_cfg() == &cfg, "the config is remembered as active");
    mm_init_complete();
    {
        PBUF *b = buf_alloc(64);         /* the pools really exist + hand out */
        CHECK(b != NULL, "buffer pool created by nsf_init_from_cfg allocates");
        if (b != NULL) {
            buf_free(b);
        }
    }
    nsf_shutdown();                      /* real teardown: disarm + mm_shutdown */
#if NSF_DEBUG
    CHECK_EQ((long)mm_debug_live_regions(), 0L, "all pool regions freed (leak gate)");
#endif
    CHECK(nsf_active_cfg() == NULL, "shutdown clears the active config");

    /* Failure path: a broken-refs config refuses to start and creates nothing. */
    CHECK_EQ(cfg_parse(BAD_LINKDEV, (UINT)strlen(BAD_LINKDEV), &cfg), 0,
             "re-parse BAD_LINKDEV");
    CHECK_EQ(mm_init(NULL), 0, "mm_init (second window)");
    rc = nsf_init_from_cfg(&cfg, err, sizeof(err));
    CHECK_EQ((long)rc, (long)NSFSTC_E_LINKDEV,
             "nsf_init_from_cfg refuses a broken config");
    CHECK(nsf_active_cfg() == NULL, "a refused config is not made active");
    nsf_shutdown();
#if NSF_DEBUG
    CHECK_EQ((long)mm_debug_live_regions(), 0L,
             "no pools created on the refused path (no partial state)");
#endif

    return mbt_test_summary("TSTSTC");
}
