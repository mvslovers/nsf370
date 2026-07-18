/*
 * tsttmcad.c -- NSFTMR/NSFEVT timer CADENCE on MVS (ADR-0034, the issue #40
 * regression gate). MVS-only (project.toml host = false): it drives the REAL
 * async STIMER exit through the executive main loop, which cannot run on host.
 *
 * WHAT IS MEASURED. This is the exact measurement that FOUND #40 live: arm a
 * delta-20 timer (20 ticks = 2.0 s at the 100 ms tick) and time when it actually
 * fires, THROUGH the whole loop path -- tmr_start -> nsftmr_plat_arm(20) -> the
 * async STIMER exit (asm/nsfstim.asm NSFTMEXP) POSTs the timer ECB -> the loop's
 * step-4 nsftmr_wake() advances the queue by the ARMED 20 ticks and fires it.
 *
 * The bug: the old loop consumed nsftmr_run(1u) per wake while the STIMER was
 * re-armed to the head delta, so a delta-N timer fired after N(N+1)/2 ticks --
 * a delta-20 fired after 210 ticks = 21.0 s (proven live on MVSCE). The fix
 * (nsftmr_wake advancing the armed amount) fires it after ~2.0 s.
 *
 * The timer RE-ARMS itself delta-20 on each fire (the TCP persist/rexmit shape),
 * so three fires land at ~2.0 / ~4.0 / ~6.0 s -- the backed-off cadence #40
 * distorted. PASS: each fire within +/- 1 tick (100 ms) of k * 2.0 s.
 *
 * TOD arithmetic mirrors tsttmacc.c / tstevtm.c: nsf_now returns the 64-bit STCK
 * value; TOD bit 51 ticks at 1 us, so microseconds = (TOD64 >> 12).
 */
#include "nsfevt.h"
#include "nsftmr.h"
#include "nsftime.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define NFIRES      3u          /* three delta-20 fires (2.0 / 4.0 / 6.0 s)     */
#define DELTA_TICKS 20u         /* the delta-20 that found #40 (2.0 s)          */

static TMR     g_timer;
static UINT    g_nfires;
static NSFTIME g_fire[NFIRES];  /* STCK timestamp at each fire                  */

/* Re-arming callback: record the fire time, then re-arm delta-20 until NFIRES,
 * exactly the TCP persist/rexmit self-re-arm pattern that surfaced #40. */
static void cadence_fn(void *arg)
{
    (void)arg;
    if (g_nfires < NFIRES) {
        nsf_now(&g_fire[g_nfires]);
    }
    g_nfires++;
    if (g_nfires < NFIRES) {
        tmr_start(&g_timer, DELTA_TICKS, cadence_fn, NULL);
    } else {
        nsfevt_stop();
    }
}

/* Milliseconds between two STCK readings (TOD bit 51 == 1 us). */
static UINT tod_diff_ms(const NSFTIME *a, const NSFTIME *b)
{
    UINT dlo    = b->lo - a->lo;
    UINT borrow = (b->lo < a->lo) ? 1u : 0u;
    UINT dhi    = b->hi - a->hi - borrow;

    return ((dhi << 20) | (dlo >> 12)) / 1000u;
}

int main(void)
{
    NSFTIME t_start;
    UINT    i;
    int     all_ok = 1;

    printf("=== nsf370 NSFTMR/NSFEVT cadence (ADR-0034 / #40, delta-20) ===\n");

    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) {
        printf("nsfevt_init failed\n");
        return 1;
    }
    mm_init_complete();

    g_nfires = 0u;
    memset(&g_timer, 0, sizeof(g_timer));

    /* Arm the delta-20 timer (tmr_start arms the STIMER itself now -- the
     * bootstrap fix; no heartbeat needed), then run the loop. */
    nsf_now(&t_start);
    tmr_start(&g_timer, DELTA_TICKS, cadence_fn, NULL);
    evt_mainloop();

    printf("observed %u of %u fires\n", (unsigned)g_nfires, (unsigned)NFIRES);
    for (i = 0u; i < g_nfires && i < NFIRES; i++) {
        UINT ms       = tod_diff_ms(&t_start, &g_fire[i]);
        UINT expect   = (i + 1u) * 2000u;              /* 2000 / 4000 / 6000 ms */
        UINT lo       = expect - 100u;                 /* -1 tick               */
        UINT hi       = expect + 100u;                 /* +1 tick               */
        int  ok       = (ms >= lo && ms <= hi);

        printf("  fire %u at %u.%03u s (expect %u.0 s +/- 0.1) %s\n",
               (unsigned)(i + 1u), (unsigned)(ms / 1000u), (unsigned)(ms % 1000u),
               (unsigned)(expect / 1000u), ok ? "OK" : "OFF");
        if (!ok) {
            all_ok = 0;
        }
    }

    CHECK_EQ((long)g_nfires, (long)NFIRES, "all three delta-20 fires observed");
    CHECK(all_ok, "each fire within +/- 1 tick of k * 2.0 s (NOT 21 s: #40 fixed)");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after shutdown");

    mm_shutdown();
    return mbt_test_summary("TSTTMCAD");
}
