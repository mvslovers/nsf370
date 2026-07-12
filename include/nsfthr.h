#ifndef NSFTHR_H
#define NSFTHR_H
/*
 * nsfthr.h -- the I/O-subtask threading seam (spec 9.3; ADR-0022).
 *
 * M1-4b runs each CTCI subchannel's blocking EXCP + single-ECB wait on its OWN
 * task, so an IOS POST of a raw IOB ECB never lands in the executive's multi-ECB
 * WAIT (issue #18). This seam is the thin portability layer over that task:
 *   MVS : src/nsfthr.c       -- libc370 `cthread` (ATTACH a subtask, SVC-2 POST,
 *                               WAIT, TTIMER-bounded wait, termecb join + DETACH)
 *   host: src/nsfthr_host.c   -- a pthread + a condition variable, so the SAME
 *                               subtask function runs under `make test-host`
 * swapped by the project.toml [host].replace map (the NSFXQ / NSFEVT_PLAT
 * pattern), so the CTCI bottom half (src/nsfctcib.c) stays portable.
 *
 * SAME ADDRESS SPACE (ADR-0022 scope). The subtask lives in the executive's own
 * address space, so waking the executive is a PLAIN POST (cthread_post ->
 * ecb_post -> SVC 2), problem state, key 8 -- NONE of UFSD's cross-AS machinery
 * (__xmpost / CSA / key 0). Proven in Stage 0 (TSTCTHR): a subtask SVC-2 POST
 * into the executive's multi-ECB WAIT wakes it and keeps it healthy.
 *
 * WAKE DIRECTIONS (both are nsfthr_post -- a POST is a POST):
 *   subtask -> executive : nsfthr_post(&dev->ecb)  (the executive WAITs it in its
 *                          multi-ECB list; on host this also wakes that WAIT)
 *   executive -> subtask : nsfthr_post(&returnecb / &txgoecb)  (the subtask
 *                          single-ECB-waits it via nsfthr_wait)
 *   IOS/shim -> subtask  : recb / wecb  (IOS on MVS; the channel shim on host);
 *                          the subtask single-ECB-waits them via nsfthr_(timed_)wait
 */
#include "nsf.h"
#include "nsfevtp.h"            /* NSFECB (a plain ECB word) */

/* Opaque subtask handle (a CTHDTASK* on MVS; a pthread wrapper on host). */
typedef struct nsfthr NSFTHR;

/* asm() external-symbol aliases (CLAUDE.md §3), unique 8-char, scheme NSFTH*:
 *   nsfthr_setup NSFTHSET   nsfthr_create NSFTHCRE   nsfthr_post NSFTHPST
 *   nsfthr_wait NSFTHWT     nsfthr_timed_wait NSFTHTWT  nsfthr_join NSFTHJN */

/* One-time: make the subtask entry point available before any nsfthr_create.
 * MVS: IDENTIFY CTHREAD (clib_identify_cthread -- a bare IDENTIFY, SVC 41,
 * problem state; NSF stays unauthorized, so it must do this itself -- libc370
 * only IDENTIFYs CTHREAD inside the authorized clib_apf_setup()). Host: a no-op.
 * Returns 0 on success. */
int  nsfthr_setup(void) asm("NSFTHSET");

/* ATTACH (MVS) / spawn (host) a subtask running fn(arg). Returns an opaque
 * handle, or NULL if the subtask could not be created. fn runs until it returns
 * (a stop request breaks its loop); its return value is the join code. */
NSFTHR *nsfthr_create(int (*fn)(void *arg), void *arg) asm("NSFTHCRE");

/* POST an ECB word (real SVC-2 MVS POST on MVS; set the bit + wake waiters on
 * host). `code` is the low-order post code (masked to the value bits). Safe from
 * either task -- a POST is a POST. */
void nsfthr_post(NSFECB *ecb, UINT code) asm("NSFTHPST");

/* Block the CALLING subtask until *ecb is posted (single ECB -- the only wait
 * shape ADR-0022 permits on a raw IOB ECB). */
void nsfthr_wait(NSFECB *ecb) asm("NSFTHWT");

/* Block until *ecb is posted OR `ticks` (100 ms units) elapse -- so an idle
 * subtask polls its stop flag (and tolerates MIH on an outstanding READ). */
void nsfthr_timed_wait(NSFECB *ecb, UINT ticks) asm("NSFTHTWT");

/* Join a subtask: wait up to `ticks` (100 ms units) for it to terminate, then
 * DETACH it (MVS) / pthread_join (host). Returns 0 if joined (task ended and was
 * detached), non-zero on timeout (the task is still live -- the caller must NOT
 * free storage the task may still touch; retain it, like UFSD's drain timeout). */
int  nsfthr_join(NSFTHR *t, UINT ticks) asm("NSFTHJN");

#endif /* NSFTHR_H */
