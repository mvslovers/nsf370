/*
 * nsfmsg_host.c -- host emit seam for nsfmsg (see nsfmsg.h).
 *
 * On the host there is no operator console: capture each emitted line into a
 * small ring the operator/startup tests read back (nsfmsg_cap_*), and echo it to
 * stdout so a test run is legible. Swapped in for src/nsfmsg_plat.c by
 * [host].replace; never compiled by cc370 on the host.
 */
#include "nsfmsg.h"
#include <stdio.h>
#include <string.h>

/* Lines retained for inspection.  RAISING THIS IS ONLY HALF THE FIX (issue #92).
 *
 * The ring keeps the FIRST CAP_MAX lines and drops everything after -- it drops
 * the NEWER lines, not the older ones, which the old comment had backwards.  At
 * 64 that silently ate the tail of any long reply: `F NSFS,APPS` at a full
 * 64-slot registry emits 1 heading + 64 slots + 1 summary = 66 lines, so the
 * last slot AND the NSF816I summary -- the line a test would assert on -- were
 * gone, and nsfmsg_cap_line() returned NULL for them, which reads as "no such
 * line" rather than "dropped".  M5-2c1 stage b worked around it by reading the
 * registry through nsfreq_app_info instead of through the report.
 *
 * 256 clears every reply this system can currently produce with headroom.  But
 * a bigger number is the SAME DEFECT AT A NEW THRESHOLD, so the real fix is
 * nsfmsg_cap_dropped(): the overflow is now countable, so a test can assert it
 * is zero instead of trusting a line count it cannot distinguish from a short
 * reply. */
#define CAP_MAX  256

static char g_cap[CAP_MAX][NSFMSG_LINE];
static UINT g_capn;             /* total emitted since reset (may exceed CAP_MAX) */
                                /* -- the difference IS the dropped count */

void nsfmsg_emit(const char *line)
{
    if (g_capn < CAP_MAX) {
        size_t n = strlen(line);
        if (n >= NSFMSG_LINE) {
            n = NSFMSG_LINE - 1;
        }
        memcpy(g_cap[g_capn], line, n);
        g_cap[g_capn][n] = '\0';
    }
    g_capn++;
    fputs(line, stdout);
    fputc('\n', stdout);
}

#if NSF_DEBUG
void nsfmsg_cap_reset(void)
{
    g_capn = 0u;
}

UINT nsfmsg_cap_count(void)
{
    return g_capn;
}

const char *nsfmsg_cap_line(UINT i)
{
    if (i < g_capn && i < CAP_MAX) {
        return g_cap[i];
    }
    return NULL;
}

/* Lines emitted but NOT retained.  Zero means the capture is complete, so a
 * test can assert completeness positively rather than inferring it from a line
 * count -- which is exactly what it could not do before (issue #92). */
UINT nsfmsg_cap_dropped(void)
{
    return (g_capn > CAP_MAX) ? (g_capn - CAP_MAX) : 0u;
}
#endif
