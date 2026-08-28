/* ==========================================================================
 * nsfreqx.h -- M5-2a: the Phase-2 NSFRQE crossing (ADR-0041).
 *
 * The PURE half of the crossing: the field-level rules that decide what an
 * NSFRQE looks like at each of the three hops, with no SVC, no CSA, no keys
 * and no address space in sight.  The MVS transport glue (the SVC issue on the
 * client side, the CSA windows and the reply POST on the STC side) lives with
 * its platform; everything HERE compiles and runs on the host, which is the
 * point -- these functions carry the correctness argument of ADR-0041 and are
 * therefore the part that must be pinned by tests rather than by a live run.
 *
 * The three hops (ADR-0041 1):
 *
 *   app NSFRQE            CSA request slot           STC-private NSFRQE
 *   (caller stack,   -->  (anchor, key 0,      -->   (STC storage, key 8)
 *    caller key)           the transport)                    |
 *                                                            | nsfreq_dispatch
 *                                                            v
 *                                                     soc_complete POSTs
 *                                                     the PRIVATE ecb
 *        <-- reply POST --  result fields  <-- copy-out -----+
 *
 * The executive dispatches the PRIVATE copy, never the CSA block, so
 * soc_complete's SVC 2 POST never targets key-0 storage and no socket or
 * protocol file learns that a boundary exists (ADR-0041 1).
 *
 * asm() external-symbol aliases (CLAUDE.md 3): scheme NSFRX*, unique across the
 * load module and clear of NSFRQ* (nsfreq.h):
 *   nsfreqx_stage_len   NSFRXSLN    nsfreqx_slot_in    NSFRXSIN
 *   nsfreqx_dispatch_in NSFRXDIN    nsfreqx_result_out NSFRXROU
 *   nsfreqx_result_in   NSFRXRIN    nsfreqx_guard_ok   NSFRXGRD
 *   nsfreqx_classify    NSFRXCLS    nsfreqx_slot_action NSFRXACT
 *   nsfreqx_slot_legal  NSFRXLEG    nsfreqx_reap_ok    NSFRXRPO
 *   nsfreqx_rc_errno    NSFRXRCE    nsfreqx_actionable NSFRXABL
 *   nsfreqx_land_copy   NSFRXLCP
 * ========================================================================== */

#ifndef NSFREQX_H
#define NSFREQX_H

#include "nsf.h"
#include "nsfreq.h"     /* NSFRQE -- the frozen 64-byte contract (spec 10.4)  */

/* The staged-chunk bound.  ADR-0039 fixed the CSA staging buffer at 2048 bytes
 * and the move clamps to it.  Defined here (rather than pulled from nsfvsvc.h)
 * so the pure logic builds on the host, where there is no CSA; the two are tied
 * together by NSFREQX_CHUNK_ASSERT below on the target. */
#define NSFREQX_CHUNK   2048U

/* The anchor's RQE-slot guard word (NSFV_ANCHOR.rqe_guard).  Four characters,
 * not a numeric constant: written and compared as a string literal so no
 * hardcoded byte value appears (spec 15.3 charset transparency), and readable
 * as text in a dump.  NON-ZERO on purpose -- a memset-zeroed anchor must be
 * distinguishable from a guard clobbered to zero. */
#define NSFREQX_GUARD   "RQEG"
#define NSFREQX_GUARDLEN 4U

/* --------------------------------------------------------------------------
 * nsfreqx_stage_len -- how many bytes of a `ulen`-byte user buffer actually
 * make the trip in one chunk.
 *
 * THE moved-length contract (ADR-0039's open obligation, discharged in
 * ADR-0041 2).  The clamp is not new -- Stage-0b already moved
 * min(ulen, 2048) -- but it was SILENT: both sides re-derived the count from a
 * shared constant, so a client that ever disagreed about the chunk size would
 * see truncation with no signal.  Naming the clamp here, and feeding its result
 * into the dispatched copy's `ulen`, makes the protocol op itself report the
 * true count through `retcode` -- which is what BSD send/recv do and what the
 * frozen layout already provides.
 * -------------------------------------------------------------------------- */
UINT nsfreqx_stage_len(UINT ulen) asm("NSFRXSLN");

/* --------------------------------------------------------------------------
 * nsfreqx_land_copy -- move one staged chunk between the CSA slot's staging
 * buffer and the STC-private LANDING AREA, bounded by the same clamp.
 *
 * WHY A LANDING AREA EXISTS AT ALL (80-FIX, ADR-0041 2 as corrected).  Hop 2
 * used to point the dispatched `ubuf` straight at slot->stage, which is CSA:
 * subpool 241, KEY 0.  The executive dispatches in its own key 8, so any verb
 * that WRITES its result through ubuf performed a key-8 store into key-0
 * storage and took S0C4.  Reads were fine -- CSA is not fetch-protected --
 * which is exactly why every path exercised before 80-CHK was a read and
 * nothing noticed for a whole milestone.  The rule the fix establishes:
 *
 *   CSA NEVER APPEARS AS A WRITABLE TARGET IN THE PROTOCOL LAYER.
 *
 * It may be READ there; anything written into is private storage, and the
 * crossing into CSA happens in ONE place, under a key window, in the
 * executive.  The protocol layer keeps knowing nothing about address spaces,
 * CSA or keys (ADR-0003), which is what makes that true by construction
 * instead of by care.
 *
 * DIRECTION-NEUTRAL ON PURPOSE.  The same function serves both the copy IN
 * (before dispatch) and the copy OUT (after completion): one encoding of the
 * bound, and the pure half never learns which way the data is going.  A
 * direction predicate is a thing that can be WRONG, and being wrong for a verb
 * added later is precisely how the defect it repairs came to exist.
 *
 * THE BOUND IS nsfreqx_stage_len, NOT THE CALLER'S WORD.  `xlen` reaches us
 * from the CSA slot, where an unauthorised client's request put it; after the
 * fix it bounds a memcpy into the STC's own private storage, so the executive
 * owns that bound and must not take the value on trust.  Reusing the clamp
 * keeps it one host-pinned expression rather than a second `if` that can drift
 * (the nsfreqx_reap_ok / nsfreqx_actionable precedent).
 *
 * Returns the count actually moved, so a caller cannot re-derive it wrongly.
 * -------------------------------------------------------------------------- */
UINT nsfreqx_land_copy(void *dst, const void *src, UINT xlen) asm("NSFRXLCP");

/* --------------------------------------------------------------------------
 * nsfreqx_slot_in -- hop 1: the caller's NSFRQE into the CSA request slot.
 *
 * A straight 64-byte image: the slot is the transport's copy of what the caller
 * asked for, including the caller-AS `ubuf` pointer (which is meaningless in the
 * STC's address space and is why hop 2 rewrites it).  No field is interpreted
 * here -- interpretation is the dispatcher's job, and validation is (d).
 * -------------------------------------------------------------------------- */
void nsfreqx_slot_in(NSFRQE *slot, const NSFRQE *caller) asm("NSFRXSIN");

/* --------------------------------------------------------------------------
 * nsfreqx_dispatch_in -- hop 2: the CSA slot into the STC-private NSFRQE that
 * the executive actually dispatches.  THE correctness crux of M5-2a.
 *
 * Copies the slot, then REWRITES exactly two fields (ADR-0041 2):
 *
 *   ubuf  <- `stage`  the CSA staging buffer.  The slot's ubuf is a CALLER-AS
 *                     address; dereferenced from the STC it reads the wrong
 *                     address space entirely.
 *   ulen  <- the count ACTUALLY staged, not the count requested.  Hand the op
 *            the requested length and a 5000-byte send reads 2952 bytes past a
 *            2048-byte buffer AND reports 5000 moved -- an overread and a
 *            silent over-report, from one wrong field.
 *
 * `q` is zeroed: the private copy is queued on the STC's own request queue, and
 * the caller's linkage means nothing on this side of the boundary.  `ecb` is
 * zeroed too, and that one is load-bearing: completion is detected by testing
 * the POSTED bit of this very word (ADR-0041 5), and the caller's ecb is
 * vestigial in Phase 2 -- inheriting a set POSTED bit would make the STC reply
 * with an untouched retcode on the first check.
 *
 * `staged` is what nsfreqx_stage_len returned for the slot's ulen (passed in
 * rather than recomputed, so the value that governed the MOVE is provably the
 * value the dispatcher sees -- one source of truth per request).
 * -------------------------------------------------------------------------- */
void nsfreqx_dispatch_in(NSFRQE *priv, const NSFRQE *slot,
                         void *stage, UINT staged) asm("NSFRXDIN");

/* --------------------------------------------------------------------------
 * nsfreqx_result_out -- hop 3a: the completed private NSFRQE back into the CSA
 * slot.  Writes ONLY the result fields (ADR-0041 4):
 *
 *   retcode  the EZASOKET RETCODE -- and the moved count
 *   errno_   the EZASOKET ERRNO
 *   apptok   RQ_INITAPI output
 *   p1/p2/p3 fn-specific outputs (accept's descriptor, getsockname's addr/port)
 *
 * Everything else is caller-owned and deliberately NOT written back.  `ubuf` in
 * particular: in the private copy it holds the STC's STAGING address, and
 * writing that into the client's block would hand the caller a pointer into
 * another address space.
 * -------------------------------------------------------------------------- */
void nsfreqx_result_out(NSFRQE *slot, const NSFRQE *priv) asm("NSFRXROU");

/* --------------------------------------------------------------------------
 * nsfreqx_result_in -- hop 3b: the CSA slot's result fields into the caller's
 * own NSFRQE, after the reply POST wakes it.  The same field set as
 * nsfreqx_result_out, applied one hop further out; the caller's ubuf/ecb/reqid
 * and its inputs survive untouched.
 * -------------------------------------------------------------------------- */
void nsfreqx_result_in(NSFRQE *caller, const NSFRQE *slot) asm("NSFRXRIN");

/* --------------------------------------------------------------------------
 * nsfreqx_guard_ok -- is the anchor's RQE-slot guard word intact?
 *
 * The RQE slot and the published wake-ECB address are neighbours in the
 * anchor, so an overrun on the 64-byte RQE move lands on the pointer the STC
 * POSTs through.  That does NOT fail cleanly: a corrupted pointer is still
 * non-zero, so the SVC routine takes the key-8 branch and posts key-0 to a
 * wrong address in the STC's private storage.  The guard turns that silent
 * wrong-address POST into a named, detectable failure.
 *
 * Pure and host-pinned deliberately (TSTREQX): the truth table is arithmetic,
 * so it belongs in a test rather than resting on a live run -- the same shape
 * obligation #5's ASVT classifier will use.
 *
 * Returns 1 when the guard matches NSFREQX_GUARD, 0 otherwise.  A NULL pointer
 * answers 0 (not verifiable == not ok) and never faults.
 * -------------------------------------------------------------------------- */
int nsfreqx_guard_ok(const char *guard) asm("NSFRXGRD");

/* ==========================================================================
 * M5-2b3 (ADR-0042): the slot pool's pure arithmetic.
 *
 * Everything below is the truth table of a decision the transport makes about
 * a CSA slot, extracted so it is pinned by TSTREQX instead of by a live run
 * that would have to hit every row.  This discharges an obligation M5-2
 * inherited from Stage-0c, which asked for exactly this extraction (the way
 * ufsd pulled its ASVT arithmetic into ufsd#asv.c), and it is what turns the
 * per-slot rewrite of nsfsx.c from a hand-copied truth table into a call.
 * ========================================================================== */

/* Slot lifecycle (ADR-0042 5).  FREE/PENDING/DONE/HELD keep the values
 * nsfvsvc.h froze in Stage-0a'/0c; CLAIMED is new and sits between FREE and
 * PENDING -- the state a slot is in once its claim CS succeeded but before the
 * client has finished staging into it.  Defined HERE as well as in nsfvsvc.h
 * because the pure half must build on the host, where there is no CSA. */
#define NSFREQX_ST_FREE     0U
#define NSFREQX_ST_PENDING  1U
#define NSFREQX_ST_DONE     2U
#define NSFREQX_ST_HELD     3U
#define NSFREQX_ST_CLAIMED  4U

/* Router return codes, mirrored here for the same reason as the states: the
 * pure half builds on the host, where nsfvsvc.h's MVS-only contract is not
 * available.  NOBUF is new in b3 -- the pool is full (ADR-0042 7). */
#define NSFREQX_RC_OK       0
#define NSFREQX_RC_INVALID  4
#define NSFREQX_RC_CORRUPT  8
#define NSFREQX_RC_NOREQ    12
#define NSFREQX_RC_NOBUF    16

/* Client-liveness verdicts (ADR-0040, used as proven -- not redesigned). */
#define NSFREQX_CL_LIVE     0
#define NSFREQX_CL_DEAD     1
#define NSFREQX_CL_UNKNOWN  2

/* ASVTAVAI: the ASVT entry is free, i.e. no address space occupies that ASID. */
#define NSFREQX_ASVT_AVAIL  0x80000000U

/* What the STC's drain should do with one slot (nsfreqx_slot_action). */
#define NSFREQX_ACT_NONE      0   /* not our work item this pass              */
#define NSFREQX_ACT_DISPATCH  1   /* hand it to the executive                 */
#define NSFREQX_ACT_HOLD      2   /* -> HELD: neither post nor reap           */
#define NSFREQX_ACT_REAP      3   /* client proved DEAD                       */
#define NSFREQX_ACT_REAP_BAD  4   /* storage we cannot trust; never POST      */

/* --------------------------------------------------------------------------
 * nsfreqx_classify -- the ADR-0040 client-death guard as arithmetic.
 *
 * Given the identity recorded in a slot and the ASVT the STC found, decide
 * whether that client is LIVE, DEAD or UNKNOWN.  All four verdict rows, the
 * range check and -- the row a live run is least likely to exercise -- the
 * `asid - 1` index are here, so they are pinned by a test rather than by
 * having happened to work.
 *
 * `asvt_enty` is the ASVT's entry array (NULL when the STC could not reach
 * one).  `asvt_maxu` is ASVTMAXU.  A NULL array, a zero maximum, a zero ASCB
 * or an out-of-range ASID all answer UNKNOWN.
 *
 * THE SAFE-SIDE ASYMMETRY, restated because it is the reason UNKNOWN exists:
 * a dead client called live leaks a slot; a LIVE client called dead has its
 * storage freed underneath it.  UNKNOWN is therefore neither posted into nor
 * reaped -- it is held.
 *
 * COMPARE THE ASCB ADDRESS ONLY.  A reused SQA block holds a stranger's
 * fields, so anything read THROUGH the recorded ASCB is untrustworthy; the
 * address itself is the only safe comparand.
 * -------------------------------------------------------------------------- */
int nsfreqx_classify(UINT req_ascb, UINT req_asid, UINT asvt_maxu,
                     const UINT *asvt_enty) asm("NSFRXCLS");

/* --------------------------------------------------------------------------
 * nsfreqx_slot_action -- the STC drain's per-slot decision, in one place.
 *
 * Inputs are the slot's observed state, the liveness verdict for its recorded
 * client, and two storage-trust booleans: `guard_ok` (the slot's RQE guard
 * word is intact) and `ptr_ok` (the anchor's published wake-ECB address is
 * still the STC's own).  Both untrusted cases answer REAP_BAD, and the
 * distinction that matters is that REAP_BAD, like REAP, must never POST.
 *
 * ORDER IS THE CONTRACT, not an implementation detail: liveness is decided
 * BEFORE storage trust, because a dead client's slot must be reclaimed whether
 * or not its guard survived, and posting into a dead address space is the
 * worse of the two failures.
 * -------------------------------------------------------------------------- */
int nsfreqx_slot_action(UINT state, int verdict,
                        int guard_ok, int ptr_ok) asm("NSFRXACT");

/* --------------------------------------------------------------------------
 * nsfreqx_actionable -- can the drain CONSUME this action on a pass where a
 * request is (or is not) already in service?  M5-2b4.
 *
 * A WAIT-gate probe must report work the drain will actually consume THIS
 * PASS.  `nsfdev_work_pending` says so in as many words -- "mirroring
 * service's consume conditions" -- and the reason is not tidiness: the
 * executive skips its WAIT whenever a probe answers non-zero, so a probe that
 * reports work the drain then declines to do is a HOT SPIN on the executive
 * task, not a latency improvement.  That is the trap this predicate exists to
 * keep out of two places at once.
 *
 * The split is not arbitrary.  REAP / HOLD / REAP_BAD all finish INSIDE the
 * CSA slot -- reclaim it, mark it HELD, never POST -- and need no private
 * NSFRQE and no executive dispatch, so the drain can do them whether or not
 * something is already in service.  DISPATCH is the one outcome that needs
 * the single private NSFRQE, so under serialised service (ADR-0042 10) it is
 * consumable only when nothing holds it.
 *
 * What that buys, concretely: a second client that publishes a request and
 * then DIES no longer waits for an unrelated client's blocking operation to
 * finish before its slot is reclaimed.  What it deliberately does NOT buy:
 * a dispatchable second request served sooner.  That needs concurrent
 * service, which is a change to the service model and not this step -- and
 * until it happens, a dispatchable PENDING slot MUST stay invisible to the
 * WAIT gate, because reporting it is exactly the spin above.
 *
 * `busy` is the STC's in-service flag (non-zero = the private NSFRQE is
 * held).  Returns 1 when this pass can consume the action, 0 otherwise.
 * -------------------------------------------------------------------------- */
int nsfreqx_actionable(int act, int busy) asm("NSFRXABL");

/* --------------------------------------------------------------------------
 * nsfreqx_slot_legal -- is `from` -> `to` a transition this design allows?
 *
 * The pool has three writers and they do not overlap:
 *   FREE    -> CLAIMED   SVC routine, by CS (the only contended transition)
 *   CLAIMED -> PENDING   SVC routine, after staging: publish LAST
 *   CLAIMED -> FREE      SVC routine, bail before publishing (POST failed)
 *   PENDING -> DONE      STC, serviced
 *   PENDING -> HELD      STC, client liveness UNKNOWN
 *   HELD    -> FREE      UNSTAGE, or a later reap
 *   DONE    -> FREE      SVC routine, releasing its own slot
 *   P/H/D   -> CLAIMED   THE REAPER taking ownership (see below)
 *   CLAIMED -> FREE      also the reaper's release, after it has cleared
 *
 * CLAIMED MEANS "OWNED, NOT AVAILABLE" -- not "owned by a client".  The
 * reaper is an owner too, and it has to be: reclaiming a slot means CLEARING
 * a dead client's data out of CSA, and clearing storage it does not yet own
 * is precisely the race the CS is there to prevent.  So the reap is two
 * moves, not one: CS from the observed state to CLAIMED (which no other
 * party can claim), then clear, then a plain store to FREE.  Reaping
 * straight to FREE would open a window in which a new client claims the slot
 * and starts staging into storage the reaper is still wiping.
 *
 * Everything else is illegal, and the illegal ones are the interesting ones:
 * FREE -> PENDING would publish a slot nobody claimed, and CLAIMED -> DONE
 * would have the STC service a slot whose staging is still in progress.
 * -------------------------------------------------------------------------- */
int nsfreqx_slot_legal(UINT from, UINT to) asm("NSFRXLEG");

/* --------------------------------------------------------------------------
 * nsfreqx_reap_ok -- may the STC reclaim this slot?
 *
 * THE WHOLE RULE, in one place.  There are two reasons to reclaim and they are
 * not the same reason:
 *
 *   the client is DEAD          -- nobody is coming back for this slot;
 *   the storage is NOT TRUSTED  -- the slot's guard word or the published
 *                                 wake-ECB pointer is clobbered, so we must
 *                                 never POST through it, whatever the client
 *                                 is doing.
 *
 * Either reclaims, and only from a state the client has already PUBLISHED
 * (PENDING / HELD / DONE).  `storage_ok` is 0 when either trust check failed.
 *
 * This function GOVERNS -- every production reap is gated on it, and
 * nsfreqx_slot_action's REAP / REAP_BAD rows are asserted to agree with it row
 * for row (TSTREQX).  Two encodings of one rule is how they diverge, and this
 * is the rule that decides whether a live client's storage gets freed.
 *
 * UNKNOWN NEVER RECLAIMS, and the reason has to survive contact with the row
 * next to it.  "HELD already prevents the POST" does NOT distinguish UNKNOWN
 * from LIVE: HELD would prevent a LIVE client's POST just as well, and the
 * adjacent row DELIBERATELY frees a live client's storage
 * (reap_ok(PENDING, LIVE, 0) == 1, pinned).  What actually separates them is
 * the standing of the evidence.  With LIVE the identity was CORROBORATED
 * against the ASVT, so "this slot's storage is corrupt" is a judgement about
 * a slot whose owner is known, and acting on it is a decision one can defend.
 * With UNKNOWN the identity could not be corroborated at all -- possibly not
 * even to the point of knowing whether the ASCB read means anything -- so the
 * storage-trust judgement rests on the same unreadable ground and is not
 * independent evidence of anything.  HELD then costs nothing and guarantees
 * the only thing that must hold ("never POST through this slot"), so there is
 * no case for spending an irreversible reclaim on a verdict that could not be
 * established.  That is the whole reason UNKNOWN is a third state instead of
 * a pessimistic DEAD.
 *
 * A CLAIMED slot is never reaped -- but NOT because it cannot be classified.
 * The identity (req_ascb / req_asid) is recorded at the CLAIM, immediately
 * after the CS and before any staging (asm/nsfvsvc.asm, CLAIMOK), so a CLAIMED
 * slot has a real ASCB and nsfreqx_classify will answer LIVE or DEAD for it.
 * Safety here rests on THIS function excluding CLAIMED explicitly, and on
 * nothing else.  (An earlier version of this comment claimed the two rules
 * agreed by construction. They do not, and that mattered: a rule believed to
 * be redundant is a rule someone removes.)
 *
 * CONSEQUENCE, stated rather than fixed: a client whose address space ends
 * between the claim and the publish leaks that slot until the STC stops.  It
 * is CLASSIFIABLE, so closing that leak is possible -- but it is not simply a
 * matter of widening this predicate.  The two-move reap establishes
 * exclusivity through CS(observed -> CLAIMED); when the observed state already
 * IS CLAIMED that compare succeeds trivially and proves nothing, so it cannot
 * distinguish "I took it" from "the live owner still has it".  Closing the
 * leak needs a distinct fourth state -- CS(CLAIMED -> REAPING) -- for the
 * reaper to prove exclusivity with.  That belongs with fault recovery, which
 * ADR-0039 and ADR-0041 both still name as open.
 * -------------------------------------------------------------------------- */
int nsfreqx_reap_ok(UINT observed_state, int verdict,
                    int storage_ok) asm("NSFRXRPO");

/* --------------------------------------------------------------------------
 * nsfreqx_rc_errno -- the router return code a client got, as an errno.
 *
 * Before the pool every non-OK rc meant "no STC / stack shutting down", and
 * the client mapped all of them to NSF_ESHUTDOWN.  Exhaustion breaks that:
 * a full pool is a healthy, running stack that has no slot right now, and
 * NSF_ENOBUFS is the project's standing answer for exactly that -- the M0
 * invariant that pool exhaustion is normal, handled gracefully by every
 * caller, and never an ABEND (CLAUDE.md 3).  Reporting it as ESHUTDOWN would
 * tell an application to give up when it should retry.
 * -------------------------------------------------------------------------- */
int nsfreqx_rc_errno(int rc) asm("NSFRXRCE");

#endif /* NSFREQX_H */
