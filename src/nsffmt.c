/*
 * nsffmt.c -- the safe formatting seam (see nsffmt.h, issue #25.2).
 *
 * A thin wrapper over the platform vsnprintf: forces the NUL-termination
 * libc370 skips on truncation (TSTVSNP proved `size` is a hard write bound,
 * so writing buf[size-1] ourselves is always safe), and clamps the return
 * value to "characters actually in the buffer" rather than C99's raw
 * "would-be" length. No platform seam needed -- this normalizes the platform
 * call itself, so the same source runs unchanged on host and MVS.
 */
#include "nsffmt.h"
#include <stdio.h>              /* vsnprintf */

int nsf_vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap)
{
    int n;

    if (size == 0u) {
        return 0;
    }
    n = vsnprintf(buf, (size_t)size, fmt, ap);
    buf[size - 1u] = '\0';      /* always in-bounds: size is a hard write bound */

    if (n < 0) {
        return 0;
    }
    if ((unsigned)n >= size) {
        return (int)(size - 1u);
    }
    return n;
}

int nsf_snprintf(char *buf, unsigned size, const char *fmt, ...)
{
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = nsf_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
