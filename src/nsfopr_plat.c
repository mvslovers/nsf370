/*
 * nsfopr_plat.c -- MVS operator (CIB/QEDIT) seam for the executive (nsfopr.h).
 *
 * EXTRACT COMM gives the COMECB (the loop WAITs on it) and the CIB chain; QEDIT
 * (via libc370 __cibset/__cibget/__cibdel) manages the chain. On each drain we
 * consume every queued CIB, dispatch by verb, and QEDIT it away:
 *   CIBMODFY -> the operand text goes to the portable nsfopr_dispatch
 *   CIBSTOP  -> orderly shutdown (nsfevt_stop -> EV_SHUTDOWN)
 *   CIBSTART -> the initial S NSF (acknowledged, no action)
 *
 * The drain runs unconditionally each loop pass (see nsfopr_drain / the loop in
 * nsfevt.c): a startup CIB can be queued before the COMECB is posted, so gating
 * on the ECB would hold the single CIB slot and reject later MODIFYs (IEE342I
 * TASK BUSY). Swapped for src/nsfopr_plat_host.c on the host by [host].replace.
 */
#include "nsfopr.h"
#include "nsfmsg.h"
#include "nsfevt.h"             /* nsfevt_stop */
#include <clibcib.h>           /* COM, CIB, __gtcom, __cibset/get/del, CIB* */
#include <string.h>

/* CIBSTART + up to 4 queued MODIFYs (matches the ecosystem STCs). */
#define NSFOPR_CIBS  5

/* Longest MODIFY operand text acted on (CIBDATA is small; the parser trims). */
#define NSFOPR_TEXT  128

static COM *g_com;

int nsfopr_init(void)
{
    g_com = __gtcom();
    if (g_com == NULL) {
        return 1;                       /* no console interface available */
    }
    __cibset(NSFOPR_CIBS);              /* QEDIT: bound the CIB queue depth */
    return 0;
}

NSFECB *nsfopr_ecb(void)
{
    return (g_com != NULL) ? (NSFECB *)g_com->comecbpt : NULL;
}

static void dispatch_cib(CIB *cib)
{
    char text[NSFOPR_TEXT];
    UINT len;

    switch (cib->cibverb) {
    case CIBMODFY:
        len = (UINT)cib->cibdatln;
        if (len >= sizeof(text)) {
            len = sizeof(text) - 1u;
        }
        memcpy(text, cib->cibdata, len);
        text[len] = '\0';
        (void)nsfopr_dispatch(text);
        break;
    case CIBSTOP:
        nsfmsg("NSF830I NSF STOP ACCEPTED -- SHUTTING DOWN");
        nsfevt_stop();                  /* EV_SHUTDOWN: the loop breaks + tears down */
        break;
    case CIBSTART:
        nsfmsg("NSF803I NSF OPERATOR INTERFACE ACTIVE");
        break;
    default:
        break;
    }
}

void nsfopr_drain(void)
{
    CIB *cib;

    if (g_com == NULL) {
        return;
    }
    while ((cib = __cibget()) != NULL) {
        dispatch_cib(cib);
        __cibdel(cib);                  /* QEDIT: free the processed CIB */
    }
}
