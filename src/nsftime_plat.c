/*
 * nsftime_plat.c -- MVS side of the elapsed-interval seam (see nsftime.h).
 *
 * Swapped for src/nsftime_plat_host.c on the host by [host].replace; never
 * compiled by the native cc. It is a separate file from asm/nsftime.asm on
 * purpose: nsf_now/nsf_taskid are single machine instructions and belong in
 * HLASM, while this is ordinary arithmetic over the value nsf_now already
 * produced. Writing it in HLASM would buy nothing and cost the column-72 rule,
 * the as370 listing gate and a live re-validation round (CLAUDE.md 3).
 *
 * THE UNIT. nsf_now stores STCK: a 64-bit count of 2**-12 microseconds. The
 * LOW bit of the HI word is therefore 2**32 * 2**-12 us = 2**20 us =
 * 1.048576 seconds exactly. So the hi word alone is a ~1 s tick, which is all
 * an interval of seconds needs -- no 64-bit arithmetic, and cc370 never has to
 * synthesise a long long.
 */
#include "nsftime.h"

/* secs -> whole hi units. 1 hi unit = 1.048576 s, so units = secs / 1.048576
 * = secs * 1000000 / 1048576 = secs * 15625 / 16384 (the fraction reduced by
 * 64, which is what keeps the multiply inside 32 bits for any sane interval).
 * ROUNDED UP -- see the header: need * 16384 >= secs * 15625 is exactly
 * need * 1.048576 >= secs, so the interval is never reported short. */
#define ELAPSED_NUM   15625u
#define ELAPSED_DEN   16384u

/* secs * 15625 overflows a UINT above this, and a silently wrapped threshold
 * would fire the limiter EARLY -- the one direction the header rules out. An
 * interval this long (~3.2 days) is not what this seam is for, so the honest
 * answer is "not elapsed" rather than a wrapped one. */
#define ELAPSED_MAX_SECS  ((UINT)(0xFFFFFFFFu - ELAPSED_DEN) / ELAPSED_NUM)

int nsf_elapsed_ge(const NSFTIME *since, const NSFTIME *now, UINT secs)
{
    UINT need;
    UINT delta;

    if (since == NULL || now == NULL) {
        return 0;                       /* nothing to measure between         */
    }
    if (secs > ELAPSED_MAX_SECS) {
        return 0;                       /* see ELAPSED_MAX_SECS               */
    }
    need = (secs * ELAPSED_NUM + (ELAPSED_DEN - 1u)) / ELAPSED_DEN;

    /* A `since` in the future is not a negative interval, it is a broken one:
     * answer 0 (the late direction) rather than let the unsigned subtract
     * wrap into an enormous delta and fire immediately. */
    if (now->hi < since->hi) {
        return 0;
    }
    delta = now->hi - since->hi;        /* whole hi units; lo is sub-second   */

    return (delta >= need) ? 1 : 0;
}
