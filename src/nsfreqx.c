/* ==========================================================================
 * nsfreqx.c -- M5-2a: the Phase-2 NSFRQE crossing, pure half (ADR-0041).
 *
 * Field-level rules only: what an NSFRQE looks like at each of the three hops
 * between an application address space and the NSFS STC.  No SVC, no CSA, no
 * key switch, no POST -- those belong to the transport (ADR-0038) and stay with
 * their platform.  Keeping this half pure is what lets the correctness argument
 * of ADR-0041 be pinned by host tests instead of by a live run.
 *
 * See include/nsfreqx.h for the hop diagram and the per-function contracts.
 * ========================================================================== */

#include "nsfreqx.h"

#include <string.h>     /* memcpy                                             */

/* -------------------------------------------------------------------------- */
UINT nsfreqx_stage_len(UINT ulen)
{
    return (ulen > NSFREQX_CHUNK) ? NSFREQX_CHUNK : ulen;
}

/* -------------------------------------------------------------------------- */
void nsfreqx_slot_in(NSFRQE *slot, const NSFRQE *caller)
{
    if (slot == NULL || caller == NULL) {
        return;
    }
    /* The full 64-byte image, verbatim. The caller-AS ubuf rides along and is
     * rewritten at the next hop (nsfreqx_dispatch_in); nothing is interpreted
     * or validated here -- validation is M5-2d. */
    memcpy(slot, caller, sizeof(NSFRQE));
}

/* -------------------------------------------------------------------------- */
void nsfreqx_dispatch_in(NSFRQE *priv, const NSFRQE *slot,
                         void *stage, UINT staged)
{
    if (priv == NULL || slot == NULL) {
        return;
    }
    memcpy(priv, slot, sizeof(NSFRQE));

    /* The private copy is queued on the STC's own request queue: the caller's
     * linkage is meaningless on this side of the boundary (ADR-0041 1). */
    priv->q.next = NULL;

    /* The private ECB starts UN-POSTED, and this is load-bearing: completion is
     * detected by testing the POSTED bit of exactly this word at the end of each
     * executive pass (ADR-0041 5). In Phase 2 the caller's own ecb is vestigial
     * -- nsfreq_wait parks on the anchor's reply_ecb -- so a client has no
     * reason to initialise it, and inheriting stack garbage with the POSTED bit
     * set would make the STC declare the request complete on the FIRST check,
     * copy out an untouched retcode and reply. Wrong answer, no abend, nothing
     * to grep for. */
    priv->ecb = 0u;

    /* THE two rewrites (ADR-0041 2). ubuf: a caller-AS address reads the wrong
     * address space from here. ulen: the count actually staged, so the protocol
     * op reports the true moved count through retcode rather than the count the
     * caller asked for -- the overread-and-over-report defect in one field. */
    priv->ubuf = stage;
    priv->ulen = staged;
}

/* -------------------------------------------------------------------------- */
void nsfreqx_result_out(NSFRQE *slot, const NSFRQE *priv)
{
    if (slot == NULL || priv == NULL) {
        return;
    }
    /* Result fields ONLY (ADR-0041 4). Notably NOT ubuf: in the private copy it
     * holds the STC's staging address, and handing that to the client would be
     * a pointer into another address space. */
    slot->retcode = priv->retcode;
    slot->errno_  = priv->errno_;
    slot->apptok  = priv->apptok;
    slot->p1      = priv->p1;
    slot->p2      = priv->p2;
    slot->p3      = priv->p3;
}

/* -------------------------------------------------------------------------- */
void nsfreqx_result_in(NSFRQE *caller, const NSFRQE *slot)
{
    if (caller == NULL || slot == NULL) {
        return;
    }
    /* Same field set as nsfreqx_result_out, one hop further out: the caller's
     * own ubuf / ecb / reqid and every input field survive untouched. */
    caller->retcode = slot->retcode;
    caller->errno_  = slot->errno_;
    caller->apptok  = slot->apptok;
    caller->p1      = slot->p1;
    caller->p2      = slot->p2;
    caller->p3      = slot->p3;
}

/* -------------------------------------------------------------------------- */
int nsfreqx_guard_ok(const char *guard)
{
    if (guard == NULL) {
        return 0;               /* not verifiable is not the same as ok       */
    }
    return (memcmp(guard, NSFREQX_GUARD, NSFREQX_GUARDLEN) == 0) ? 1 : 0;
}
