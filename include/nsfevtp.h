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
 *   nsfevt_plat_wait NSFEVWT   nsfevt_plat_post NSFEVPO */

/* Block until any of the `count` ECBs in the list is posted. The list is a
 * plain array of ECB pointers (no terminator bit -- the MVS seam supplies it).
 * Returns once one is posted. */
void nsfevt_plat_wait(NSFECB **ecblist, int count) asm("NSFEVWT");

/* Post an ECB: set NSFECB_POSTED and (on the host) wake a WAITing loop. Safe to
 * call from the async exit (MVS) or another thread (host test). */
void nsfevt_plat_post(NSFECB *ecb) asm("NSFEVPO");

#endif /* NSFEVTP_H */
