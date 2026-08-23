/* ==========================================================================
 * nsfreqc.c -- M5-2a: the Phase-2 client-side request transport (ADR-0041).
 *
 * Carries the caller's NSFRQE to the NSFS STC over the private SVC and brings
 * the result back.  See include/nsfreqc.h for the contract.
 *
 * The field rules are NOT reimplemented here: nsfreqx_slot_in builds the image
 * and nsfreqx_result_in applies the result, so the policy stays in the one
 * host-tested place (TSTREQX) rather than being duplicated per transport.
 * ========================================================================== */

#include "nsfreqc.h"
#include "nsfreqx.h"          /* slot_in / result_in -- the host-tested rules */
#include "nsfvsvc.h"          /* NSFV_REQ / NSFV_REQ_RQE / NSFV_SVCNUM        */

#include <string.h>

/* The staging image: the caller's NSFRQE as it goes across, and as it comes
 * back.  One per program -- single client, single slot by construction
 * (ADR-0041 3); the pool is M5-2b. */
static NSFRQE g_image;

/* --------------------------------------------------------------------------
 * Issue the private SVC with R1 = A(req), via the EX-SVC-0 trick: the SVC
 * number rides in R6 and EX ORs its low byte into a stored "SVC 0", so no
 * storage is modified and the sequence stays RENT-safe.  This is the
 * relink-only pattern proven by TSTSVC/TSTUBUF (ADR-0038 6).
 *
 * noinline so the named asm labels appear exactly once.
 * -------------------------------------------------------------------------- */
static void __attribute__((noinline))
nsfreqc_svc(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"          /* R1 = A(req)                        */
        "         LR    6,%1\n"          /* R6 = SVC number                    */
        "         EX    6,NSFRCS0\n"     /* execute SVC <R6-low>               */
        "         B     NSFRCSX\n"
        "NSFRCS0  SVC   0\n"             /* EX target; storage unmodified      */
        "NSFRCSX  DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

/* -------------------------------------------------------------------------- */
void
nsfreqc_call(NSFRQE *r)
{
    NSFV_REQ req;

    if (r == NULL) {
        return;
    }

    /* Hop 1: the caller's block into the image the SVC routine will stage. */
    nsfreqx_slot_in(&g_image, r);

    memset(&req, 0, sizeof(req));
    memcpy(req.eye, NSFV_REQ_EYE, 4);
    req.func   = NSFV_REQ_RQE;
    req.rqeimg = (void *)&g_image;
    /* The user buffer travels as its own move (the ADR-0039 bounce). ubuf is
     * the CALLER-AS address here -- the SVC routine reads it in the caller's
     * address space and the STC never sees it; the STC's private copy gets the
     * staging address instead (ADR-0041 2). */
    req.ubuf   = r->ubuf;
    req.ulen   = r->ulen;
    req.rc     = -1;

    nsfreqc_svc(&req);

    if (req.rc != NSFV_RC_OK) {
        /* The transport itself failed -- the request never reached the stack
         * (anchor gone / STC quiescing / slot busy). Report an API failure
         * rather than leaving the caller's block holding whatever it had.
         * ESHUTDOWN, not a new errno: the frozen NSFRQE layout is unaffected
         * either way, but adding to the errno set is a scope question, and
         * "stack is shutting down" is the honest reading of every rc the
         * routine can return here (CORRUPT / NOREQ / INVALID). */
        r->retcode = NSF_RETERR;
        r->errno_  = NSF_ESHUTDOWN;
        return;
    }

    /* Hop 3b: apply ONLY the result fields (ADR-0041 4). The caller's ubuf,
     * ecb, reqid and every input field survive untouched. */
    nsfreqx_result_in(r, &g_image);
}

/* -------------------------------------------------------------------------- */
int
nsfreqc_init(void)
{
    NSFV_REQ req;

    /* Probe the transport before claiming it: QUERY takes no slot and changes
     * no in-flight state, so it is safe to issue even if another client is
     * mid-request. A non-zero rc means no STC has published an anchor (or the
     * SVC slot is not ours), and we must NOT register -- the caller then stays
     * on the Phase-1 path instead of failing every request. */
    memset(&req, 0, sizeof(req));
    memcpy(req.eye, NSFV_REQ_EYE, 4);
    req.func = NSFV_REQ_QUERY;
    req.rc   = -1;

    nsfreqc_svc(&req);
    if (req.rc != NSFV_RC_OK) {
        return -1;
    }

    nsfreq_set_transport(nsfreqc_call);
    return 0;
}
