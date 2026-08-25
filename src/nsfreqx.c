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

/* ==========================================================================
 * M5-2b3 (ADR-0042): the slot pool's pure arithmetic.
 *
 * Extracted so TSTREQX pins every row, rather than a live run having to hit
 * each one.  See include/nsfreqx.h for the contracts and the reasoning.
 * ========================================================================== */

/* -------------------------------------------------------------------------- */
int nsfreqx_classify(UINT req_ascb, UINT req_asid, UINT asvt_maxu,
                     const UINT *asvt_enty)
{
    UINT entry;

    /* No identity recorded at all.  NOTE this is NOT the ordinary state of a
     * CLAIMED slot -- identity is written at the claim, before staging -- so
     * do not read this branch as the reason CLAIMED is never reaped.  That
     * reason is nsfreqx_reap_ok excluding it explicitly. */
    if (req_ascb == 0u) {
        return NSFREQX_CL_UNKNOWN;
    }
    /* The STC could not reach an ASVT at all. */
    if (asvt_enty == NULL || asvt_maxu == 0u) {
        return NSFREQX_CL_UNKNOWN;
    }
    /* ASIDs are 1-based; entry[asid - 1] is the one that names this address
     * space.  Range-check BEFORE the subtraction, so a zero ASID can never
     * index entry[-1]. */
    if (req_asid == 0u || req_asid > asvt_maxu) {
        return NSFREQX_CL_UNKNOWN;
    }

    entry = asvt_enty[req_asid - 1u];

    if (entry & NSFREQX_ASVT_AVAIL) {
        return NSFREQX_CL_DEAD;         /* the ASID is free: the AS ended     */
    }
    if (entry != req_ascb) {
        return NSFREQX_CL_DEAD;         /* ASID REUSED -- a different AS now  */
    }
    return NSFREQX_CL_LIVE;
}

/* -------------------------------------------------------------------------- */
int nsfreqx_slot_action(UINT state, int verdict, int guard_ok, int ptr_ok)
{
    /* Only a published request is this drain's work item.  A DONE slot belongs
     * to its owner (which releases it), a HELD one was already taken off the
     * work list, and CLAIMED/FREE are mid-claim or idle. */
    if (state != NSFREQX_ST_PENDING) {
        return NSFREQX_ACT_NONE;
    }

    /* Liveness BEFORE storage trust (see the header): a dead client's slot is
     * reclaimed whether or not its guard survived, and the alternative --
     * posting into an address space that ended -- is the worse failure. */
    if (verdict == NSFREQX_CL_DEAD) {
        return NSFREQX_ACT_REAP;
    }
    if (verdict == NSFREQX_CL_UNKNOWN) {
        return NSFREQX_ACT_HOLD;
    }

    /* The client is live, so the only question left is whether the storage
     * describing its request can be trusted.  Neither of these POSTs. */
    if (!guard_ok || !ptr_ok) {
        return NSFREQX_ACT_REAP_BAD;
    }
    return NSFREQX_ACT_DISPATCH;
}

/* -------------------------------------------------------------------------- */
int nsfreqx_slot_legal(UINT from, UINT to)
{
    switch (from) {
    case NSFREQX_ST_FREE:
        return (to == NSFREQX_ST_CLAIMED);
    case NSFREQX_ST_CLAIMED:
        /* PENDING is the publish; FREE is the bail-before-publish path. */
        return (to == NSFREQX_ST_PENDING || to == NSFREQX_ST_FREE);
    case NSFREQX_ST_PENDING:
        /* CLAIMED is the REAPER taking ownership before it clears the slot;
         * FREE is the STC servicing path releasing without a clear. */
        return (to == NSFREQX_ST_DONE || to == NSFREQX_ST_HELD ||
                to == NSFREQX_ST_FREE || to == NSFREQX_ST_CLAIMED);
    case NSFREQX_ST_HELD:
        return (to == NSFREQX_ST_FREE || to == NSFREQX_ST_CLAIMED);
    case NSFREQX_ST_DONE:
        return (to == NSFREQX_ST_FREE || to == NSFREQX_ST_CLAIMED);
    default:
        return 0;
    }
}

/* -------------------------------------------------------------------------- */
int nsfreqx_reap_ok(UINT observed_state, int verdict, int storage_ok)
{
    /* UNKNOWN NEVER RECLAIMS, whatever the storage says.  Untrusted storage
     * means "never POST through this slot", and HOLD already achieves that
     * without freeing anything -- so reclaiming here would buy nothing and
     * risk the one outcome the safe-side asymmetry exists to prevent: a LIVE
     * client having its storage freed underneath it.  This row is the whole
     * reason UNKNOWN is a third state instead of a pessimistic DEAD. */
    if (verdict == NSFREQX_CL_UNKNOWN) {
        return 0;
    }
    /* Two reasons, not one: the client is gone, or the storage describing its
     * request cannot be trusted and must never be POSTed through. */
    if (verdict != NSFREQX_CL_DEAD && storage_ok) {
        return 0;
    }
    /* Published states only.  FREE is not a request at all, and CLAIMED is
     * excluded HERE and nowhere else -- see the header: it is classifiable,
     * so nothing else excludes it. */
    return (observed_state == NSFREQX_ST_PENDING ||
            observed_state == NSFREQX_ST_HELD    ||
            observed_state == NSFREQX_ST_DONE) ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
int nsfreqx_rc_errno(int rc)
{
    switch (rc) {
    case NSFREQX_RC_OK:
        return 0;
    case NSFREQX_RC_NOBUF:
        /* A full pool is a HEALTHY stack with no slot right now.  Reporting
         * ESHUTDOWN here would tell an application to give up when the correct
         * answer is to retry. */
        return NSF_ENOBUFS;
    case NSFREQX_RC_INVALID:
    case NSFREQX_RC_CORRUPT:
    case NSFREQX_RC_NOREQ:
    default:
        /* Every other rc means no STC has published an anchor, or the one that
         * did is quiescing -- "the stack is shutting down" is the honest
         * reading, and it is what the client mapped them to before b3. */
        return NSF_ESHUTDOWN;
    }
}
