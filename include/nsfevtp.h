#ifndef NSFEVTP_H
#define NSFEVTP_H
/*
 * nsfevtp.h -- the WAIT / POST platform seam for the NSFEVT main loop.
 *
 * The loop blocks on an ECB list; the source of a wake is the async STIMER exit
 * (timerECB) or a stop request (stopECB). The WAIT is platform-specific:
 *   MVS : src/nsfevt_plat.c   -- libc370 ecb_waitlist (WAIT ECBLIST, SVC 1)
 *   host: src/nsfevt_plat_host.c -- a pthread mutex + condition variable
 * swapped by the project.toml [host].replace map (the NSFXQ / NSFTIME pattern),
 * so the loop logic in src/nsfevt.c stays portable.
 */
#include "nsf.h"

/* An ECB word. POST sets bit 1 (0x40000000); a cleared ECB is 0. */
typedef UINT NSFECB;
#define NSFECB_POSTED  0x40000000u

/* Terminator bit for the LAST pointer in an MVS ECB list (WAIT ECBLIST
 * convention: the final ECB address carries the 0x80000000 VL bit). The loop
 * passes a plain array + count; the MVS seam adds this bit to a local copy
 * before ecb_waitlist. It is NOT a host concept (host pointers are 64-bit). */
#define NSFECB_LAST    0x80000000u

/* asm() aliases (CLAUDE.md §3). The seam entry names are the aliases:
 *   nsfevt_plat_wait NSFEVWT   nsfevt_plat_post NSFEVPO
 *   nsfevt_plat_wake NSFEVPW */

/* Block until any of the `count` ECBs in the list is posted. The list is a
 * plain array of ECB pointers (no terminator bit -- the MVS seam supplies it).
 * Returns once one is posted. */
void nsfevt_plat_wait(NSFECB **ecblist, int count) asm("NSFEVWT");

/* BIT-ONLY post: set NSFECB_POSTED and (on the host) wake a WAITing loop. On MVS
 * this ONLY sets the bit -- it does NOT wake a task already committed to the
 * WAIT (SVC 1). That is correct for a post ALWAYS accompanied by a real wake (the
 * device / request ECB is SVC-2-posted by its subtask; the handoff/wake bits are
 * drained on the pass that real wake triggers). Do NOT use it for a STANDALONE
 * cross-task signal -- see nsfevt_plat_wake. */
void nsfevt_plat_post(NSFECB *ecb) asm("NSFEVPO");

/* REAL post: a plain SVC-2 POST on MVS (ecb_post) that WAKES a task committed to
 * the WAIT, and the cond-var broadcast on the host. Use for a STANDALONE cross-task
 * signal with no accompanying real wake -- the stop request (nsfevt_stop): a
 * subtask sets g_stop and must wake the executive even when the STIMER heartbeat
 * has been disarmed (an idle timer queue after a TCP connection's timers drained,
 * ADR-0034). ecb_post needs no cthread identity, so any caller is safe. */
void nsfevt_plat_wake(NSFECB *ecb) asm("NSFEVPW");

#endif /* NSFEVTP_H */
