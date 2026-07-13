/*
 * nsfmsg.c -- the portable core of the operator/log message seam (see nsfmsg.h).
 *
 * Format one line with nsf_vsnprintf (libc370's own vsnprintf does not
 * NUL-terminate on truncation, issue #25.2 -- nsf_vsnprintf does, always),
 * then hand the finished line to the platform emit seam (wto on MVS,
 * capture+stdout on the host). No state, no allocation, run-to-completion --
 * callable from mainline and (defensively, one line, no SVC of its own) from
 * the recovery path.
 */
#include "nsfmsg.h"
#include "nsffmt.h"             /* nsf_vsnprintf */

void nsfmsg(const char *fmt, ...)
{
    char    line[NSFMSG_LINE];
    va_list ap;

    va_start(ap, fmt);
    (void)nsf_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    nsfmsg_emit(line);
}
