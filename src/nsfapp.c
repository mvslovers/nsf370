/* ==========================================================================
 * nsfapp.c -- the app-registry operator report (M5-2c1).  See nsfapp.h.
 * ========================================================================== */

#include "nsfapp.h"

#include "nsfreq.h"     /* nsfreq_app_info / _app_max / _app_classify        */
#include "nsfreqx.h"    /* NSFREQX_CL_* -- the verdicts the classifier gives  */
#include "nsfmsg.h"     /* nsfmsg -- WTO on MVS, a capture ring on the host   */

const char *nsfapp_verdict_name(int verdict)
{
    switch (verdict) {
    case NSFREQX_CL_LIVE:     return "LIVE";
    case NSFREQX_CL_DEAD:     return "DEAD";
    case NSFREQX_CL_UNKNOWN:  return "UNKNOWN";
    case NSFREQ_APPCL_NONE:   return "NO-ID";
    case NSFREQ_APPCL_FREE:   return "FREE";
    default:                  return "?";
    }
}

void nsfapp_report(void)
{
    UINT n = nsfreq_app_max();
    UINT i;
    UINT inuse = 0u;
    UINT dead  = 0u;

    nsfmsg("NSF814I APP REGISTRY:");

    for (i = 0u; i < n; i++) {
        UINT token = 0u;
        UINT ascb  = 0u;
        UINT asid  = 0u;
        int  cl;

        if (!nsfreq_app_info(i, &token, &ascb, &asid)) {
            continue;                   /* in-use slots only -- see the header */
        }
        inuse++;

        /* Classified through nsfreq_app_classify, which is where the
         * zero-identity rule lives; this file never decides that itself. */
        cl = nsfreq_app_classify(i);
        if (cl == NSFREQX_CL_DEAD) {
            dead++;
        }
        nsfmsg("NSF815I   SLOT %2u TOKEN=%08X ASCB=%08X ASID=%04X %s",
               (unsigned)i, (unsigned)token, (unsigned)ascb,
               (unsigned)asid, nsfapp_verdict_name(cl));
    }

    nsfmsg("NSF816I APP REGISTRY: %u OF %u SLOTS IN USE, %u DEAD",
           (unsigned)inuse, (unsigned)n, (unsigned)dead);
}
