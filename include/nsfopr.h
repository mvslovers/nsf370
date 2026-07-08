#ifndef NSFOPR_H
#define NSFOPR_H
/*
 * nsfopr.h -- the operator interface: MODIFY/STOP command dispatch (spec 1.4d,
 * 5.4, 17).
 *
 * Split into a PORTABLE dispatcher and a thin MVS seam so the routing logic is
 * host-tested by feeding command strings:
 *
 *   nsfopr_dispatch(text)  parse "DISPLAY | STATS | TRACE c ON|OFF | STOP | HELP"
 *                          and act (set nsftrc_flags, render sts, request stop),
 *                          WTOing NSF8xx replies via nsfmsg. Pure C.
 *
 * The CIB/QEDIT plumbing that DELIVERS the string is the platform seam:
 *   MVS : src/nsfopr_plat.c       EXTRACT COMM (COMECB + CIB chain) + QEDIT
 *   host: src/nsfopr_plat_host.c  an injectable queue so the loop test can drive
 *                                 the whole operator path
 * swapped by the project.toml [host].replace map. The main loop (NSFEVT) adds
 * the console ECB to its ECBLIST via evt_set_operator(nsfopr_ecb, nsfopr_drain).
 */

#include "nsf.h"
#include "nsfevtp.h"            /* NSFECB (the console ECB the loop WAITs on) */

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module function pins a unique 8-char linker name. Scheme NSFOP*:
 *   nsfopr_dispatch NSFOPDSP   nsfopr_init NSFOPINI   nsfopr_ecb NSFOPECB
 *   nsfopr_drain NSFOPDRN
 *   (host-only inject: nsfopr_host_cmd NSFOPHCM   nsfopr_host_stop NSFOPHST) */

/* Route one MODIFY command string to its handler, WTOing NSF8xx replies. Pure C
 * (no MVS services); the CIB seam hands it the operand text after the verb sep.
 * Returns 1 if the command requested a stop (STOP), else 0. */
int nsfopr_dispatch(const char *text) asm("NSFOPDSP");

/* Obtain the console interface (COMECB + CIB chain) and set the CIB queue depth.
 * Init-time; returns 0 on success, non-zero if the console is unavailable. */
int nsfopr_init(void) asm("NSFOPINI");

/* The console ECB the main loop adds to its ECBLIST (NULL before nsfopr_init). */
NSFECB *nsfopr_ecb(void) asm("NSFOPECB");

/* Drain and dispatch every queued operator command, QEDITing each processed CIB.
 * Called UNCONDITIONALLY each loop pass -- deliberately NOT gated on the console
 * ECB bit: MVS may queue the startup CIB without POSTing the ECB, and gating
 * would hold the single CIB slot and reject later MODIFYs (IEE342I TASK BUSY).
 * A CIBSTOP verb requests shutdown (nsfevt_stop); CIBMODFY text goes to
 * nsfopr_dispatch. */
void nsfopr_drain(void) asm("NSFOPDRN");

#if NSF_DEBUG
/* Host-only injection so a test can drive the operator path through the real
 * loop (src/nsfopr_plat_host.c): queue a MODIFY command / a STOP, then POST the
 * console ECB (nsfopr_ecb) so the WAITing loop wakes and nsfopr_drain runs. */
void nsfopr_host_cmd(const char *text) asm("NSFOPHCM");
void nsfopr_host_stop(void) asm("NSFOPHST");
#endif

#endif /* NSFOPR_H */
