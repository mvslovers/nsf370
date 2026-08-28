/* ==========================================================================
 * nsfsx.h -- M5-2a: the NSFS STC's cross-address-space request transport
 *            (ADR-0041 + its addendum).
 *
 * The STC-side half of the crossing: it owns the CSA anchor, installs the
 * private SVC, and turns a request that arrived from another address space
 * into a call on the ordinary executive.
 *
 * IT REGISTERS INTO THE EXISTING evt_set_request SLOT.  In the NSFS STC the
 * same-address-space request transport (nsfreq_submit / _drain / _ecb) has no
 * job -- nothing in this address space submits, and the three-hop design calls
 * nsfreq_dispatch() directly -- so this is not a fifth executive seam but the
 * SAME one, wired to a different transport.  That is spec 2's "the NSFRQE
 * request block is the phase boundary; only the transport changes" made real
 * in one registration call:
 *
 *     evt_set_request(nsfsx_ecb(), nsfsx_drain, nsfsx_pending);
 *
 * MVS-ONLY.  There is no CSA, SVC table or ASCB on the host; the field-level
 * rules that this file drives are the host-tested part (nsfreqx.h / TSTREQX).
 *
 * DUPLICATION, DELIBERATE AND TEMPORARY.  The anchor/SVC/classifier plumbing
 * here is modelled on src/nsfv.c rather than extracted from it.  Refactoring
 * proven MVS transport code in the same step that adds new behaviour is the
 * Runaway-Refactor stop pattern, and the four Stage-0 gates are only a clean
 * regression measure while the probe stays byte-for-byte unchanged.  The probe
 * scaffolding is removed in M5-2c; that is where the two copies collapse into
 * this one.
 *
 * asm() external-symbol aliases (CLAUDE.md 3): scheme NSFSX*, unique:
 *   nsfsx_start NSFSXSTA   nsfsx_stop  NSFSXSTO   nsfsx_ecb   NSFSXECB
 *   nsfsx_drain NSFSXDRN   nsfsx_pending NSFSXPND
 *   nsfsx_stats_extra NSFSXSXT   nsfsx_classify_client NSFSXCLC
 *   nsfsx_recover_quiesce NSFSXRQ
 * ========================================================================== */

#ifndef NSFSX_H
#define NSFSX_H

#include "nsf.h"

/* Bring the transport up: allocate the CSA anchor, load the SVC routine into
 * CSA and patch the anchor address into it, PUBLISH the wake-ECB address, and
 * only then steal the SVC slot.
 *
 * The order is the contract (ADR-0041 addendum): the slot steal is the "we are
 * open for business" signal, so the ECB address must already be published --
 * a client must never find a stolen slot together with an unpublished or stale
 * address.  Returns 0 on success; on failure nothing is left installed.        */
int   nsfsx_start(void) asm("NSFSXSTA");

/* Take it down in the reverse order: restore the SVC slot first (no new client
 * can enter), then invalidate the published ECB address, wake anyone parked,
 * drain the in-flight count, unload the routine and free the CSA.              */
void  nsfsx_stop(void) asm("NSFSXSTO");

/* THE RECOVERY-PATH COUNTERPART OF nsfsx_stop (issue #79).  An ESTAE exit is
 * not a shutdown path and must not pretend to be one, so this is deliberately
 * NOT nsfsx_stop with a flag: it does the two things whose absence costs an
 * IPL and nothing else.
 *
 *   DOES     restore the SVC slot (no new client can enter, and the next
 *            S NSFS can steal it again -- without this the STC cannot restart
 *            at all), then clear ANCHOR_ACTIVE and null the published wake-ECB
 *            address, which names key-8 storage that dies with this address
 *            space.
 *
 *   DOES NOT drain.  nsfsx_stop's drain polls for up to 10 s and nudges parked
 *            clients; a polling loop in an exit in a damaged environment is not
 *            acceptable.  Recovery takes the RETAIN posture unconditionally.
 *
 *   DOES NOT free the CSA anchor or unload the router.  The M5-2b3 rule stands
 *            unchanged: a client may be parked in a WAIT *inside* that code, so
 *            freeing it is strictly worse than leaking it.  Restoring the table
 *            entry is a different act from freeing the module and only the
 *            first is in scope.
 *
 * State-aware: it borrows key 0 and returns the task to the state it was
 * entered in, because __prob unconditionally MODESETs to problem state and the
 * exit may well have been entered in supervisor state (see the NSF902I reading
 * nsf_recover emits).  Returns one of the NSFSX_RQ_* values below; the caller
 * turns that into the operator message, because a silent failure here is the
 * difference between "recovered" and "an IPL is coming" and nobody can see it
 * from the outside.  Safe to call when the transport was never started.        */
/* QUIESCED means "nothing of ours is left reachable" -- the slot is not stolen
 * and the anchor is marked inactive.  It does NOT assert that a stolen slot was
 * RETURNED: the restore is a no-op when nothing was stolen, which is reachable
 * (an abend part-way through nsfsx_start, anchor allocated, slot not yet taken).
 * Read it as a state, not as an event.  NSF903I says "SVC RESTORED", which is
 * the wording the live gate captured and is kept so that evidence stays
 * reproducible; on the no-op path read it as "the slot is not ours".           */
#define NSFSX_RQ_QUIESCED   0   /* slot not stolen, anchor marked inactive     */
#define NSFSX_RQ_IDLE       1   /* nothing was installed -- nothing to undo    */
#define NSFSX_RQ_NOKEY      2   /* could not reach key 0: SLOT STILL STOLEN    */
#define NSFSX_RQ_STUCK      3   /* key obtained, slot still not restored       */
int   nsfsx_recover_quiesce(void) asm("NSFSXRQ");

/* The executive's wake ECB -- in the STC's OWN key-8 private storage, NOT the
 * key-0 CSA one.  evt_mainloop WAITs from problem state, where a key-0 ECB in
 * the ECBLIST is a documented abend (S047 / X'201'), so the SVC routine posts
 * this address instead (ADR-0041 addendum).                                    */
UINT *nsfsx_ecb(void) asm("NSFSXECB");

/* The ADR-0040 client-death guard asked about a bare caller identity: returns
 * NSFREQX_CL_LIVE / _DEAD / _UNKNOWN.  Registered with nsfreq_set_classifier
 * so the portable app registry can ask a question only MVS can answer.
 *
 * This is the SECOND consumer of the guard and it reclaims different things:
 * the drain reaps CSA REQUEST SLOTS at the transport, while the registry
 * reclaims APP SLOTS AND SOCKETS in the executive.  They share this classifier
 * and nothing else; keeping the two reclamation paths distinct is deliberate
 * (M5-2c1). */
int   nsfsx_classify_client(UINT ascb, UINT asid) asm("NSFSXCLC");

/* One call per executive pass: complete a finished request (copy the result
 * out, re-check client liveness, reply) and/or take a newly arrived one and
 * dispatch it into the executive.                                              */
void  nsfsx_drain(void) asm("NSFSXDRN");

/* The WAIT gate's side-effect-free probe.  TRUE for a request waiting to be
 * dispatched AND for one that has completed but not yet been replied to --
 * that second state is reachable, and without it the loop can commit to a WAIT
 * on top of it (ADR-0041 5; the ADR-0025 defect-(2) / #27 class).              */
int   nsfsx_pending(void) asm("NSFSXPND");

/* The Phase-2 supplement to the operator STATS reply (issue #64, step 64-0):
 * the transport's raw wake-ECB word with its POSTED bit decoded, plus the
 * counters that interpret it (evtpasses / wakeposts / served) and the anchor's
 * diagnostic words. Registered with nsfopr_set_stats_extra by nsfsmain, so it
 * runs only in the NSFS module; safe before nsfsx_start (it reports zeros). */
void  nsfsx_stats_extra(void) asm("NSFSXSXT");

#endif /* NSFSX_H */
