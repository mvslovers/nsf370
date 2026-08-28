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
