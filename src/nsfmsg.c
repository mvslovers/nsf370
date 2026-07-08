/*
 * nsfmsg.c -- the portable core of the operator/log message seam (see nsfmsg.h).
 *
 * Format one line with the platform vsnprintf, then hand the finished line to
 * the platform emit seam (wto on MVS, capture+stdout on the host). No state, no
 * allocation, run-to-completion -- callable from mainline and (defensively, one
 * line, no SVC of its own) from the recovery path.
 */
#include "nsfmsg.h"
#include <stdio.h>              /* vsnprintf */

void nsfmsg(const char *fmt, ...)
{
    char    line[NSFMSG_LINE];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';      /* vsnprintf always NUL-terminates, but
                                           be explicit -- emit trusts the NUL */
    nsfmsg_emit(line);
}
