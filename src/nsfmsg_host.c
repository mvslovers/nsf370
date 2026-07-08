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

#define CAP_MAX  64             /* lines retained for inspection (older dropped) */

static char g_cap[CAP_MAX][NSFMSG_LINE];
static UINT g_capn;             /* total emitted since reset (may exceed CAP_MAX) */

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
#endif
