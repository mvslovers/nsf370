/*
 * nsfmain.c -- the NSF started task: init -> loop -> MODIFY/STOP -> shutdown,
 * under ESTAE, config-driven (spec 5, 14, 17; the M0-8 deliverable). MVS-ONLY
 * (project.toml host = false): it uses the MVS console/ESTAE seams, so it is
 * never compiled on the host -- the sequencing it drives (nsf_init_from_cfg),
 * the dispatcher (nsfopr_dispatch) and the loop (evt_mainloop) are all
 * host-tested on their own.
 *
 * Startup order (spec 14.4):
 *   trace/stats init -> cfg_load PROFILE -> mm_init/nsftmr_init/nsfevt_init
 *   -> nsf_init_from_cfg (pools sized from NSFPOOL, trace flags, refs)
 *   -> mm_init_complete (seal) -> operator init -> ESTAE -> evt_mainloop.
 * A config error or a failed pool build REFUSES to start (no partial config).
 */
#include "nsf.h"
#include "nsfcfg.h"
#include "nsfstc.h"
#include "nsfopr.h"
#include "nsfevt.h"
#include "nsfmsg.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsftrc.h"
#include "nsfsts.h"
#include <clibstae.h>          /* __estae, ESTAE_CREATE/DELETE */
#include <clibsdwa.h>          /* SDWA, SDWARCDE, SDWACWT */

/* The PROFILE.TCPIP member is read from the PROFILE DD in the PROC (spec 14.4;
 * libc370 fopen understands "DD:ddname"). */
#define NSF_PROFILE_DD  "DD:PROFILE"

/* The loaded config (1160 B) -- file scope, not a stack frame on the executive
 * task (spec 14.4 keeps large init buffers off the stack). Read-only after a
 * successful parse; the operator DISPLAY reads it via nsf_active_cfg. */
static NSFCFG g_cfg;

/*
 * ESTAE recovery (spec 17.1). MVS enters here on any unhandled abend on the
 * executive task. First-failure data is already captured for the dump: the
 * trace ring ("NSFTRACE"), the statistics registry ("NSFSTATS") and every pool
 * header carry eyecatchers (spec 17.2). We WTO a marker, attempt the SAME
 * orderly teardown the clean path uses (nsf_shutdown: disarm the timer exit,
 * free the pool regions), then percolate (SDWARCDE = SDWACWT == 0) so RTM
 * produces the dump and terminates the address space -- no Hercules restart is
 * ever needed to clean up (goal, ADR-0006). Minimal + defensive: it may run in
 * a damaged environment, so it does the least that is useful and never loops.
 */
static void nsf_recover(SDWA *sdwa)
{
    nsfmsg("NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING");
    nsf_shutdown();
    nsfmsg("NSF901I NSF EMERGENCY TEARDOWN COMPLETE");
    if (sdwa != NULL) {
        sdwa->SDWARCDE = SDWACWT;       /* continue with termination = percolate */
    }
}

int main(int argc, char **argv)
{
    char err[NSFCFG_MSGLEN];
    INT  rc;

    (void)argc;
    (void)argv;

    /* Foundation FFDC surfaces first (static storage, no pool), so even a very
     * early failure leaves a trace ring + eyecatchers in the dump. */
    nsftrc_init();
    sts_init();

    nsfmsg("NSF000I NSF %s STARTING", NSF_VERSION);

    /* 1. Load + fully validate the PROFILE member (all-or-nothing, spec 14.1). */
    rc = cfg_load(NSF_PROFILE_DD, &g_cfg);
    if (rc != 0) {
        nsfmsg("%s", g_cfg.err.msg);            /* the rendered NSF7xxE + line */
        nsfmsg("NSF009E NSF INITIALIZATION FAILED, RC=%d", (int)rc);
        return 8;
    }
    if (g_cfg.nwarn > 0u) {
        nsfmsg("NSF719W %u CONFIG STATEMENT(S) IGNORED", (unsigned)g_cfg.nwarn);
    }

    /* 2. Open the init window; build the foundation pools. */
    if (mm_init(NULL) != 0) {
        nsfmsg("NSF008E NSF MEMORY INITIALIZATION FAILED");
        return 8;
    }
    nsftmr_init();
    if (nsfevt_init() != 0) {
        nsfmsg("NSF008E NSF EVENT POOL CREATE FAILED");
        mm_shutdown();
        return 8;
    }

    /* 3. Wire the config into the component inits (buffer pools sized from
     *    NSFPOOL, NSFTRACE flags, referential integrity). Refuse to start on any
     *    error -- no partial config survives (spec 14.1). */
    rc = nsf_init_from_cfg(&g_cfg, err, sizeof(err));
    if (rc != 0) {
        nsfmsg("%s", err);
        nsfmsg("NSF009E NSF INITIALIZATION FAILED, RC=%d", (int)rc);
        mm_shutdown();
        return 8;
    }
    mm_init_complete();                          /* seal: mm_pool_create is closed */

    /* 4. Operator interface (CIB/QEDIT); add its console ECB to the ECBLIST. */
    if (nsfopr_init() != 0) {
        nsfmsg("NSF807E NSF CONSOLE INTERFACE UNAVAILABLE");
        nsf_shutdown();
        return 8;
    }
    evt_set_operator(nsfopr_ecb(), nsfopr_drain);

    /* 5. ESTAE from init onward (ADR-0006): recovery uses the same teardown. */
    __estae(ESTAE_CREATE, (void *)nsf_recover, NULL);

    nsfmsg("NSF001I NSF INITIALIZATION COMPLETE");

    /* 6. Run the executive until STOP / P NSF (evt_mainloop runs the §5.4
     *    shutdown sequence internally before it returns). */
    evt_mainloop();

    /* 7. Final teardown. Delete the ESTAE FIRST so a fault while releasing pools
     *    cannot re-enter recovery (the ecosystem stumbling block). */
    __estae(ESTAE_DELETE, NULL, NULL);
    nsf_shutdown();
    nsfmsg("NSF011I NSF SHUTDOWN COMPLETE");
    return 0;
}
