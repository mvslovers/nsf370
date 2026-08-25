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
 *   nsfreqx_rc_errno    NSFRXRCE
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
 * nsfreqx_slot_legal -- is `from` -> `to` a transition this design allows?
 *
 * The pool has three writers and they do not overlap:
 *   FREE    -> CLAIMED   SVC routine, by CS (the only contended transition)
 *   CLAIMED -> PENDING   SVC routine, after staging: publish LAST
 *   CLAIMED -> FREE      SVC routine, bail before publishing (POST failed)
 *   PENDING -> DONE      STC, serviced
 *   PENDING -> HELD      STC, client liveness UNKNOWN
 *   PENDING -> FREE      STC, reaping a DEAD client
 *   HELD    -> FREE      UNSTAGE, or a later reap
 *   DONE    -> FREE      SVC routine, releasing its own slot
 *
 * Everything else is illegal, and the illegal ones are the interesting ones:
 * FREE -> PENDING would publish a slot nobody claimed, and CLAIMED -> DONE
 * would have the STC service a slot whose staging is still in progress.
 * -------------------------------------------------------------------------- */
int nsfreqx_slot_legal(UINT from, UINT to) asm("NSFRXLEG");

/* --------------------------------------------------------------------------
 * nsfreqx_reap_ok -- may the STC reclaim this slot?
 *
 * Only a DEAD verdict reclaims, and only from a state the client has already
 * PUBLISHED (PENDING / HELD / DONE).
 *
 * A CLAIMED slot is never reaped, and the reason is not caution -- it is that
 * there is nothing to classify.  The identity (req_ascb / req_asid) is written
 * during staging, so a slot that is CLAIMED but not yet PENDING carries no
 * identity at all; nsfreqx_classify answers UNKNOWN for a zero ASCB, and
 * UNKNOWN is never reaped.  The two rules agree by construction rather than by
 * both being remembered.
 *
 * CONSEQUENCE, stated rather than fixed: a client whose address space ends
 * between the claim and the publish leaks that slot until the STC stops.  That
 * is the fault-recovery item ADR-0039 and ADR-0041 both still name as open.
 * -------------------------------------------------------------------------- */
int nsfreqx_reap_ok(UINT observed_state, int verdict) asm("NSFRXRPO");

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
