/*
 * nsfopr_plat_host.c -- host operator seam for the executive (nsfopr.h).
 *
 * There is no MVS console on the host, so this stands in a small injectable
 * command queue plus a fake console ECB: a test enqueues a MODIFY command
 * (nsfopr_host_cmd) or a STOP (nsfopr_host_stop) and POSTs the ECB, and the real
 * main loop wakes and calls nsfopr_drain, which dispatches exactly as the MVS
 * CIB seam would. This lets the whole operator path -- loop wake, drain, dispatch
 * -- run under test-host. Swapped in for src/nsfopr_plat.c by [host].replace.
 */
#include "nsfopr.h"
#include "nsfmsg.h"
#include "nsfevt.h"             /* nsfevt_stop */
#include "nsfevtp.h"            /* NSFECB, nsfevt_plat_post */
#include <string.h>

#define QMAX  8
#define TLEN  128

static NSFECB g_opecb;                  /* the fake console ECB the loop WAITs on */
static char   g_text[QMAX][TLEN];       /* queued command text                    */
static int    g_stopv[QMAX];            /* 1 == a STOP verb (no text)             */
static int    g_head;
static int    g_tail;

int nsfopr_init(void)
{
    g_opecb = 0u;
    g_head  = 0;
    g_tail  = 0;
    return 0;
}

NSFECB *nsfopr_ecb(void)
{
    return &g_opecb;
}

void nsfopr_drain(void)
{
    while (g_head != g_tail) {
        int idx = g_head;
        g_head = (g_head + 1) % QMAX;
        if (g_stopv[idx]) {
            nsfmsg("NSF830I NSF STOP ACCEPTED -- SHUTTING DOWN");
            nsfevt_stop();
        } else {
            (void)nsfopr_dispatch(g_text[idx]);
        }
    }
    /* Emulate QEDIT clearing the console ECB as the CIB chain empties, so the
     * loop's WAIT re-blocks instead of spinning on a stale post (the MVS system
     * does this for comecbpt; here we own the fake ECB). Test injection is
     * single-threaded, so there is no lost-post race with a concurrent enqueue. */
    g_opecb = 0u;
}

#if NSF_DEBUG
static void enqueue(const char *text, int stopv)
{
    int nt = (g_tail + 1) % QMAX;

    if (nt == g_head) {
        return;                         /* queue full: drop (test would notice) */
    }
    if (text != NULL) {
        size_t n = strlen(text);
        if (n >= TLEN) {
            n = TLEN - 1u;
        }
        memcpy(g_text[g_tail], text, n);
        g_text[g_tail][n] = '\0';
    } else {
        g_text[g_tail][0] = '\0';
    }
    g_stopv[g_tail] = stopv;
    g_tail = nt;
    nsfevt_plat_post(&g_opecb);         /* wake the WAITing loop */
}

void nsfopr_host_cmd(const char *text)
{
    enqueue(text, 0);
}

void nsfopr_host_stop(void)
{
    enqueue(NULL, 1);
}
#endif
