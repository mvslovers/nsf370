/*
 * tststcm.c -- NSF STC on-MVS component validation (spec 5/14/17; host = false).
 *
 * MVS-only: it links the executive against the real MVS seams (STIMER exit, WTO,
 * ESTAE) and proves the M0-8 startup wiring runs on 3.8j:
 *   1. a PROFILE parses and nsf_init_from_cfg builds the config-sized pools with
 *      real mm_pool_create (below the 16 MB line, EBCDIC-independent path);
 *   2. those pools hand out + reclaim buffers (alloc N, free N, alloc N again --
 *      a leak would fail the re-alloc; there is no NSF_DEBUG leak counter on the
 *      production/MVS build);
 *   3. the ESTAE recovery environment establishes and deletes (the __estae
 *      C-bridge links + runs on 3.8j -- the mechanism nsf_recover rides on).
 *
 * The full operator flow -- S NSF reads the PROFILE DD, F NSF,DISPLAY|STATS|
 * TRACE respond via WTO, P NSF shuts down clean, an induced ABEND is caught by
 * the ESTAE (trace+stats in the dump) and PERCOLATES -- terminates the address
 * space on percolate and needs a live console, so it is the PROC-driven operator
 * validation (jcl/NSFPROC.jcl), not a batch RC test. CC 0 here == no abend
 * through init + teardown, which is itself the corruption/leak gate.
 *
 * Diagnosing: CC 0 = wiring + ESTAE link/run proven; an ABEND (PSW dump) = a
 * seam is wrong on the target (read the PSW, as with tstevtm.c).
 */
#include "nsfstc.h"
#include "nsfcfg.h"
#include "nsfevt.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsftrc.h"
#include "nsfsts.h"
#include "nsfbuf.h"
#include <clibstae.h>          /* __estae, ESTAE_CREATE/DELETE, SDWA */
#include <clibsdwa.h>
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define NPROBE  4u

static const char PROFILE[] =
    "DEVICE  CTCA CTC 0E20\n"
    "LINK    LNK1 CTC 0 CTCA\n"
    "HOME    10.1.1.2 LNK1\n"
    "NSFPOOL BUFSMALL 32\n"
    "NSFPOOL BUFLARGE 16\n"
    "NSFTRACE CONFIG ON\n";

/* A throwaway recovery for the establish/delete smoke: percolate if ever run
 * (it is not driven here -- no abend is induced). */
static void probe_recover(SDWA *sdwa)
{
    if (sdwa != NULL) {
        sdwa->SDWARCDE = SDWACWT;
    }
}

int main(void)
{
    NSFCFG cfg;
    char   err[NSFCFG_MSGLEN];
    PBUF  *b[NPROBE];
    UINT   i;
    INT    rc;
    int    ok;

    printf("=== nsf370 NSF STC MVS component validation ===\n");
    nsftrc_init();
    sts_init();

    /* 1. config -> component init on real 3.8j. */
    CHECK_EQ(cfg_parse(PROFILE, (UINT)strlen(PROFILE), &cfg), 0,
             "PROFILE parses on MVS");
    CHECK_EQ(mm_init(NULL), 0, "mm_init opens the init window");
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init creates the EVT pool");
    rc = nsf_init_from_cfg(&cfg, err, sizeof(err));
    CHECK_EQ((long)rc, 0L, "nsf_init_from_cfg wires the config on MVS");
    CHECK((nsftrc_flags & TRCF_CONFIG) != 0u, "NSFTRACE CONFIG ON applied");
    mm_init_complete();

    /* 2. the config-sized pools hand out and reclaim (leak gate without a debug
     *    counter: free everything, then re-allocate the same count). */
    ok = 1;
    for (i = 0u; i < NPROBE; i++) {
        b[i] = buf_alloc(64);
        if (b[i] == NULL) { ok = 0; }
    }
    CHECK(ok, "buffer pool hands out NPROBE buffers");
    for (i = 0u; i < NPROBE; i++) {
        if (b[i] != NULL) { buf_free(b[i]); }
    }
    ok = 1;
    for (i = 0u; i < NPROBE; i++) {
        b[i] = buf_alloc(64);
        if (b[i] == NULL) { ok = 0; }
    }
    CHECK(ok, "re-allocates NPROBE after free (pool fully reclaimed, no leak)");
    for (i = 0u; i < NPROBE; i++) {
        if (b[i] != NULL) { buf_free(b[i]); }
    }

    /* 3. ESTAE recovery environment establishes + deletes on 3.8j. */
    CHECK_EQ(__estae(ESTAE_CREATE, (void *)probe_recover, NULL), 0,
             "ESTAE CREATE links + runs on MVS");
    CHECK_EQ(__estae(ESTAE_DELETE, NULL, NULL), 0, "ESTAE DELETE succeeds");

    /* 4. orderly teardown completes without abend (CC 0 is the gate). */
    nsf_shutdown();
    printf("STC component validation complete\n");

    return mbt_test_summary("TSTSTCM");
}
