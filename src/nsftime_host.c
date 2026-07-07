/*
 * nsftime_host.c -- host implementation of the platform-primitive seam
 * (see nsftime.h). Swapped in for asm/nsftime.asm on the native test build via
 * the project.toml [host].replace map; never compiled by cc370.
 *
 * nsf_now() reads the wall clock with gettimeofday and splits it into the two
 * 32-bit halves of NSFTIME (hi = seconds, lo = microseconds). This is the
 * portable, dependency-free reading the task sanctions for the host; the MVS
 * side (STCK) is a true monotonic TOD. It is adequate for trace-entry ordering
 * -- the only host consumer at M0-4.
 *
 * nsf_taskid() returns 0: the host test build runs a single mainline task.
 * Per-exit task ids matter only once asynchronous MVS exits write trace, which
 * is an M1 concern (nsftrc.c single-writer note).
 */
#include "nsftime.h"

#include <sys/time.h>           /* gettimeofday */

void nsf_now(NSFTIME *out)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    out->hi = (UINT)tv.tv_sec;
    out->lo = (UINT)tv.tv_usec;
}

UINT nsf_taskid(void)
{
    return 0u;
}
