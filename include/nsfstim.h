#ifndef NSFSTIM_H
#define NSFSTIM_H
/*
 * nsfstim.h -- the platform seam that arms the real interval timer for NSFTMR
 * (spec ch. 06). Kept separate from nsftmr.h the same way nsftime.h is separate
 * from its callers: MVS-only code lives in asm/nsfstim.asm, the host build swaps
 * in src/nsfstim_host.c via the project.toml [host].replace map (the NSFXQ /
 * NSFTIME pattern).
 *
 * On MVS the arming is a single STIMER (SVC 47, TYPE=REAL) re-armed to the head
 * delta by nsftmr_run; its exit does nothing but POST the timer ECB, so all
 * timer processing happens on the executive task. Only ONE STIMER is active at a
 * time -- exactly the single-interval model 3.8j provides (there is no STIMERM;
 * see nsftmr.h and ADR-0011). TTIMER CANCEL disarms it.
 *
 * The timer ECB is owned here (a module word in nsfstim.asm) and exposed via
 * nsftmr_plat_ecb(): the WAIT belongs to the CONSUMER, not this seam. At M0-6
 * the executive main loop includes this ECB in its WAIT ECBLIST; until then the
 * timer-accuracy job (test/mvs) is the consumer and WAITs on it through its own
 * small asm helper. So this seam never blocks.
 *
 * STATUS: like asm/nsfxq.asm and asm/nsftime.asm, asm/nsfstim.asm ASSEMBLES
 * (as370: the STIMER/TTIMER/POST macros resolve from the sysroot macro library)
 * and cross-LINKS in the normal build, but its RUNTIME on 3.8j is validated by
 * the on-MVS timer-accuracy job (the ADR-0011 gate), not yet by CI.
 *
 * One tick is 100 ms (spec 6.3); STIMER BINTVL is in 0.01 s units, so the MVS
 * side arms BINTVL = ticks * 10.
 */
#include "nsf.h"

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"). The
 * three arming primitives are the C<->asm seam: each alias here MUST be
 * character-identical to the CSECT label in asm/nsfstim.asm (host builds use
 * src/nsfstim_host.c, which inherits the aliases transparently). Scheme NSFTM +
 * verb, sharing the NSFTMR component prefix:
 *   nsftmr_plat_arm NSFTMARM   nsftmr_plat_disarm NSFTMDIS   nsftmr_plat_ecb NSFTMECB
 */

/* Arm the platform timer to POST the timer ECB after `ticks` logical ticks
 * (100 ms each). Clears the ECB first, then arms; replaces any pending arm (one
 * STIMER at a time). Never WAITs. */
void  nsftmr_plat_arm(UINT ticks) asm("NSFTMARM");

/* Cancel any pending platform timer (TTIMER CANCEL). Idempotent. */
void  nsftmr_plat_disarm(void) asm("NSFTMDIS");

/* Address of the timer ECB the arm exit POSTs -- the WAIT target for the
 * executive loop (M0-6) and the accuracy job. Stable for the life of the run. */
UINT *nsftmr_plat_ecb(void) asm("NSFTMECB");

#if NSF_DEBUG
/* Host-only introspection of the seam (implemented in src/nsfstim_host.c, NOT
 * by the MVS asm; NSF_DEBUG is unset in the cross build). The host shim records
 * each arm/disarm so tsttmr can assert that nsftmr_run re-arms for the head
 * delta and disarms when the queue drains. Reset the counters between scenarios
 * with nsftmr_plat_probe_reset. */
UINT nsftmr_plat_last_arm(void)      asm("NSFTMLAR");   /* ticks of last arm    */
UINT nsftmr_plat_arm_count(void)     asm("NSFTMACN");
UINT nsftmr_plat_disarm_count(void)  asm("NSFTMDCN");
void nsftmr_plat_probe_reset(void)   asm("NSFTMPRR");
#endif

#endif /* NSFSTIM_H */
