/*
 * nsftime_plat_host.c -- host side of the elapsed-interval seam (see
 * nsftime.h). Swapped in for src/nsftime_plat.c on the native test build by
 * the project.toml [host].replace map; never compiled by cc370.
 *
 * THE UNIT. The host nsf_now stores a gettimeofday reading split in two:
 * hi = whole seconds, lo = microseconds within that second. So the seconds
 * are already there and the only arithmetic is the BORROW -- without it a
 * reading 0.999999 s after `since` would report a whole second elapsed, which
 * is the early direction the header rules out.
 */
#include "nsftime.h"

int nsf_elapsed_ge(const NSFTIME *since, const NSFTIME *now, UINT secs)
{
    UINT dsec;

    if (since == NULL || now == NULL) {
        return 0;                       /* nothing to measure between         */
    }
    /* A `since` in the future -- see the MVS side: 0, not a wrapped delta. */
    if (now->hi < since->hi) {
        return 0;
    }
    dsec = now->hi - since->hi;

    if (now->lo < since->lo) {
        /* Part-way through the final second, so one fewer WHOLE second has
         * passed than the seconds fields suggest. */
        if (dsec == 0u) {
            return 0;
        }
        dsec--;
    }
    return (dsec >= secs) ? 1 : 0;
}
