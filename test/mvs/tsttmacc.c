/*
 * tsttmacc.c -- NSFTMR timer-accuracy job (spec 6.3, the ADR-0011 gate).
 *
 * MVS-only (project.toml host = false): it drives the real STIMER and so
 * cannot run on the host. It MEASURES what ADR-0011 asserts -- that a 100 ms
 * STIMER tick under Hercules on 3.8j is accurate enough that TCP's 200 ms
 * delayed ACK and >= 1 s RTO never fire early.
 *
 * WHAT IS MEASURED (read this before trusting the number): this job times the
 * STIMER TIMEBASE with a task-synchronous STIMER WAIT (test/asm/tststmw.asm) --
 * arm 100 ms, block, read the elapsed interval with nsf_now (STCK). It is a
 * standalone loop: it does NOT go through nsftmr_run and it does NOT use the
 * NSFTMR async STIMER-REAL + POST-exit seam (asm/nsfstim.asm). The timebase is
 * the same one STIMER REAL / STIMERM would use (same Hercules TOD emulation), so
 * this validates the 100 ms tick's ACCURACY. It does NOT validate the async
 * exit-dispatch path or its latency -- that (and the seam's live run) is an M0-6
 * task; see asm/nsfstim.asm and ADR-0011.
 *
 * PASS criterion (ADR-0011): mean within [90,110] ms AND no single interval
 * over 200 ms. On failure it still prints the full distribution (never a silent
 * pass), so the 100 ms tick can be revisited before ADR-0011 is frozen.
 *
 * LIVE-RUN STATUS (M0-5): PASSES on MVS after the issue #8 entry-convention fix.
 * Measured N=100: mean 100.1/100.2 ms, min/max 100 ms, jitter 0 ms -- both
 * criteria pass, so ADR-0011 is FROZEN. See docs/adr/ADR-0011.
 *
 * TOD arithmetic: nsf_now returns the 64-bit STCK value in NSFTIME{hi,lo}. TOD
 * bit 51 ticks at 1 microsecond, so microseconds = (TOD64 >> 12).
 */
#include "nsftime.h"
#include <mbtcheck.h>
#include <stdio.h>

/* STIMER WAIT helper (test/asm/tststmw.asm): block for centisecs * 0.01 s.
 * asm() alias per CLAUDE.md paragraph 3. */
void stimer_wait(UINT centisecs) asm("STIMWAIT");

#define ACC_ITERS   100u        /* >= 100 iterations per ADR-0011 */
#define TICK_MS     100u        /* one NSFTMR tick = 100 ms = 10 centiseconds */
#define TICK_CS     10u

/* Microseconds between two STCK readings: (b - a) >> 12 (TOD bit 51 == 1 us).
 * A 100 ms interval is ~4.1e8 raw TOD units, so the difference lives in the low
 * word; the high term is carried for completeness. */
static UINT tod_diff_us(const NSFTIME *a, const NSFTIME *b)
{
    UINT dlo    = b->lo - a->lo;
    UINT borrow = (b->lo < a->lo) ? 1u : 0u;
    UINT dhi    = b->hi - a->hi - borrow;

    return (dhi << 20) | (dlo >> 12);
}

int main(void)
{
    NSFTIME  t0, t1;
    UINT     i;
    UINT     sum_us = 0u;
    UINT     min_us = 0xFFFFFFFFu;
    UINT     max_us = 0u;
    UINT     mean_t, min_ms, max_ms, jit_ms;   /* mean_t in tenths of a ms */

    printf("=== nsf370 NSFTMR timer-accuracy (ADR-0011 gate, n=%u) ===\n",
           (unsigned)ACC_ITERS);
    printf("STIMER WAIT %u ms x %u (timebase only; async exit = M0-6)\n",
           (unsigned)TICK_MS, (unsigned)ACC_ITERS);

    for (i = 0u; i < ACC_ITERS; i++) {
        UINT us;

        nsf_now(&t0);
        stimer_wait(TICK_CS);        /* block ~100 ms */
        nsf_now(&t1);

        us = tod_diff_us(&t0, &t1);
        sum_us += us;
        if (us < min_us) {
            min_us = us;
        }
        if (us > max_us) {
            max_us = us;
        }
    }

    mean_t = ((sum_us / ACC_ITERS) + 50u) / 100u;   /* tenths of ms, rounded */
    min_ms = min_us / 1000u;
    max_ms = max_us / 1000u;
    jit_ms = (max_us - min_us) / 1000u;

    printf("intervals: mean %u.%u ms  min %u ms  max %u ms  jitter %u ms\n",
           (unsigned)(mean_t / 10u), (unsigned)(mean_t % 10u),
           (unsigned)min_ms, (unsigned)max_ms, (unsigned)jit_ms);

    /* mean in [90,110] ms (900..1100 tenths) and no interval over 200 ms. */
    CHECK(mean_t >= 900u && mean_t <= 1100u,
          "mean interval within [90,110] ms");
    CHECK(max_ms <= 200u,
          "no single interval exceeds 200 ms");

    return mbt_test_summary("TSTTMACC");
}
