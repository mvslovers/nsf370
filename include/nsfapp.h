#ifndef NSFAPP_H
#define NSFAPP_H
/*
 * nsfapp.h -- the app-registry operator report (M5-2c1, obligation #3).
 *
 * WHY THIS EXISTS. An application address space that ends without calling
 * TERMAPI leaves its app slot and every socket scoped to it allocated. Whether
 * anything can be reclaimed depends on one question -- what does the ADR-0040
 * client-death guard say about that address space now -- and until this file
 * there was no way to ask it from outside. `F NSFS,APPS` asks.
 *
 * READ-ONLY. It classifies and prints; it reaps nothing, frees nothing and
 * changes no state. That is deliberate for the measurement it was written for
 * (does a normally-ended job classify DEAD, and after how long?), and it stays
 * true afterwards: it is how an operator sees why a reclamation did or did not
 * happen, which is a question that outlives the measurement.
 *
 * PORTABLE ON PURPOSE. It reads the registry through nsfreq_app_info and asks
 * for a verdict through nsfreq_app_classify, so nothing here knows what an
 * ASVT is. The MVS-only half is the classifier the STC registers. That is what
 * lets the message text and -- more to the point -- the zero-identity rule be
 * pinned by a host test instead of by reading a console log.
 *
 * PHASE 2 ONLY IN PRACTICE, BUT NOT BY CONSTRUCTION. Phase 1 registers no
 * verb, so `F NSF,APPS` stays NSF808E; if it ever did register one, every slot
 * would report NO-ID, which is the truth about a Phase-1 registry rather than
 * a special case (see NSFREQ_APPCL_NONE).
 *
 * asm() external-symbol aliases (CLAUDE.md 3): scheme NSFAP*, unique:
 *   nsfapp_report NSFAPRPT   nsfapp_verdict_name NSFAPVNM
 */

#include "nsf.h"

/* Emit the app registry to the operator: one NSF815I line per slot in use --
 * index, app token, caller ASCB/ASID and the liveness verdict as a WORD, not a
 * code -- bracketed by an NSF814I heading and an NSF816I summary counting the
 * slots in use and, of those, how many classify DEAD.
 *
 * IN-USE SLOTS ONLY. The registry is 16 slots and this is meant to be issued
 * repeatedly while watching a client die; printing 16 lines a poll would bury
 * the one or two that matter in the console log. The summary carries the
 * bound, so an empty registry still says so in one line. */
void nsfapp_report(void) asm("NSFAPRPT");

/* The verdict of nsfreq_app_classify as a fixed-width word for the report:
 * "LIVE" / "DEAD" / "UNKNOWN" / "NO-ID" / "FREE", never a numeric code -- the
 * whole point is that somebody reads these lines and can quote what they said.
 * Any unrecognised value renders as "?" rather than being trusted. */
const char *nsfapp_verdict_name(int verdict) asm("NSFAPVNM");

#endif /* NSFAPP_H */
