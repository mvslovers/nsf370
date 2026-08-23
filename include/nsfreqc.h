/* ==========================================================================
 * nsfreqc.h -- M5-2a: the Phase-2 CLIENT-side request transport (ADR-0041).
 *
 * The application half of the crossing.  An app builds its NSFRQE exactly as
 * it always has and calls nsfreq_call(); this file is what carries that block
 * to the NSFS STC and brings the result back, through the private SVC
 * (ADR-0038) and the keyed CSA bounce (ADR-0039).
 *
 * INERT UNTIL REGISTERED.  nsfreqc_init() installs itself into NSFREQ through
 * nsfreq_set_transport(); a program that never calls it keeps the Phase-1
 * same-address-space path byte-for-byte.  That is the same "registration
 * seam" idiom as evt_set_request / nsfip_register_proto / nsfreq_register_select,
 * and it is why the 20 existing modules that link nsfreq.c need no change.
 *
 * THE SURFACES ABOVE THIS DO NOT CHANGE.  NSFEZA still builds the NSFRQE on
 * the caller's stack and still calls nsfreq_call at every call site; the C,
 * EZASOKET and EZASMI surfaces are byte-identical.  Applications RELINK ONLY.
 *
 * THE CLIENT STAYS UNAUTHORISED.  The SVC is Type 3 and needs no APF library
 * (ADR-0038) -- that is the red line this whole transport exists to honour.
 *
 * MVS-ONLY: there is no SVC or CSA on the host.
 *
 * asm() aliases (CLAUDE.md 3), scheme NSFRC*, distinct from the NSFRQ and
 * NSFRX namespaces:
 *   nsfreqc_init NSFRCINI    nsfreqc_call NSFRCCAL
 * ========================================================================== */

#ifndef NSFREQC_H
#define NSFREQC_H

#include "nsf.h"
#include "nsfreq.h"

/* Route this program's requests across the address-space boundary.  Call once,
 * before the first request (NSFEZA's implicit INITAPI is the natural point).
 * Returns 0 if the transport is reachable -- i.e. the NSFS STC has published
 * its anchor and stolen the SVC slot -- and non-zero if it is not, in which
 * case nothing is registered and the caller stays on the Phase-1 path.        */
int  nsfreqc_init(void) asm("NSFRCINI");

/* The registered transport: stage the NSFRQE (and any ubuf data) into the CSA
 * request slot, issue the SVC -- which POSTs the STC and WAITs for the reply
 * inside the routine -- then apply the result fields to the caller's block.
 *
 * SYNCHRONOUS BY CONSTRUCTION: the SVC routine does the POST *and* the WAIT in
 * one invocation (ADR-0038), so submit and wait cannot be separated on this
 * transport.  nsfreq_call maps straight onto it; nsfreq_submit performs the
 * whole round trip and nsfreq_wait is then a no-op.  Harmless here because
 * NSFEZA uses nsfreq_call at every call site -- there is no async submit/wait
 * split in the API to preserve.                                               */
void nsfreqc_call(NSFRQE *r) asm("NSFRCCAL");

#endif /* NSFREQC_H */
