/*
 * tstevtm.c -- NSFEVT on-MVS validation (spec ch. 05, the M0-6 MVS step).
 *
 * MVS-only (project.toml host = false): it drives the REAL async STIMER exit,
 * which cannot run on the host. This is where the exit (asm/nsfstim.asm
 * NSFTMEXP) is proven on 3.8j -- the S0C6 class must be gone.
 *
 * Flow: arm the STIMER heartbeat (nsftmr_plat_arm), then run the main loop. The
 * loop WAITs on {timerECB, ...}; every ~100 ms the STIMER exit POSTs timerECB
 * and RE-ARMS itself, so the loop wakes, runs nsftmr_run, and dispatches
 * EV_TIMER_EXPIRED. After N ticks the handler requests a stop and the loop shuts
 * down cleanly. The delta queue is intentionally EMPTY, so nsftmr_run is a no-op
 * and the self-re-arming exit is the sole heartbeat driver (no double re-arm).
 *
 * Diagnosing the outcome (the three shapes are deliberately distinct):
 *   - CC 0 + "observed N ticks"  -> the exit POSTs AND re-arms; fix proven.
 *   - ABEND (PSW dump)           -> the exit's linkage is wrong; read the PSW.
 *   - hang / S322, 0 or 1 ticks  -> STIMER never armed (0) or the exit fired
 *                                    once but failed to re-arm (1); the dump's
 *                                    PSW parked in the WAIT SVC is the signature.
 */
#include "nsfevt.h"
#include "nsftmr.h"
#include "nsfstim.h"
#include "nsftime.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <stdio.h>

#define NTICKS  10u

static UINT    g_count;
static NSFTIME g_t[NTICKS + 1u];

static void on_tick(EVT *ev)
{
    (void)ev;
    if (g_count <= NTICKS) {
        nsf_now(&g_t[g_count]);      /* timestamp this tick for the interval */
    }
    g_count++;
    if (g_count >= NTICKS) {
        nsfevt_stop();               /* simulated stop after N ticks */
    }
}

/* Microseconds between two STCK readings (TOD bit 51 == 1 us), as the accuracy
 * job does. */
static UINT tod_diff_us(const NSFTIME *a, const NSFTIME *b)
{
    UINT dlo    = b->lo - a->lo;
    UINT borrow = (b->lo < a->lo) ? 1u : 0u;
    UINT dhi    = b->hi - a->hi - borrow;

    return (dhi << 20) | (dlo >> 12);
}

int main(void)
{
    UINT i;
    UINT sum_us = 0u;
    UINT n      = 0u;
    UINT mean_t;

    printf("=== nsf370 NSFEVT MVS validation (async STIMER exit) ===\n");

    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) {
        printf("nsfevt_init failed\n");
        return 1;
    }
    evt_register(EV_TIMER_EXPIRED, on_tick);
    mm_init_complete();

    g_count = 0u;
    nsftmr_plat_arm(1u);             /* arm the ~100 ms heartbeat (1 tick) */
    evt_mainloop();                  /* runs until N ticks -> stop -> shutdown */

    printf("observed %u timer ticks (target %u)\n",
           (unsigned)g_count, (unsigned)NTICKS);
    for (i = 1u; i < g_count && i <= NTICKS; i++) {
        sum_us += tod_diff_us(&g_t[i - 1u], &g_t[i]);
        n++;
    }
    if (n > 0u) {
        mean_t = ((sum_us / n) + 50u) / 100u;    /* tenths of a ms */
        printf("mean tick interval %u.%u ms over %u intervals\n",
               (unsigned)(mean_t / 10u), (unsigned)(mean_t % 10u), (unsigned)n);
    }

    CHECK(g_count >= NTICKS, "loop observed >= N async-STIMER timer ticks");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after shutdown");

    mm_shutdown();
    return mbt_test_summary("TSTEVTM");
}
