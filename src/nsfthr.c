/*
 * nsfthr.c -- MVS implementation of the I/O-subtask threading seam (nsfthr.h),
 * over libc370 `cthread`. MVS-ONLY: swapped for src/nsfthr_host.c on the host by
 * the project.toml [host].replace map. `cthread` was de-risked in isolation at
 * M1-4b Stage 0 (TSTCTHR, CC 0) before this wrapper existed.
 *
 * The opaque NSFTHR handle IS the libc370 CTHDTASK. Same address space, so every
 * POST is a plain SVC-2 (cthread_post -> ecb_post), problem state, key 8 -- no
 * __xmpost / CSA / key 0 (ADR-0022).
 */
#include "nsfthr.h"

#include <clibos.h>            /* clib_identify_cthread (IDENTIFY CTHREAD)   */
#include <clibthrd.h>          /* cthread_create / _post / _wait / _detach   */
#include <clibecb.h>          /* ECB, ecb_timed_wait, ECB_POSTED_BIT        */

int nsfthr_setup(void)
{
    /* IDENTIFY CTHREAD so ATTACH EP=CTHREAD resolves, WITHOUT APF -- NSF stays
     * unauthorized (ADR-0022). libc370 otherwise only does this inside the
     * authorized clib_apf_setup(). Also force-links CTHREAD via its =V(CTHREAD). */
    return clib_identify_cthread();
}

NSFTHR *nsfthr_create(int (*fn)(void *arg), void *arg)
{
    /* cthread invokes fn(arg1, arg2); our fn reads only arg1 (arg2 = 0). */
    return (NSFTHR *)cthread_create((void *)fn, arg, (void *)0);
}

void nsfthr_post(NSFECB *ecb, UINT code)
{
    (void)cthread_post((ECB *)ecb, code);   /* SVC-2 POST, same address space */
}

/* NB. We do NOT use cthread_wait / cthread_timed_wait here: cthread_wait and
 * cthread_timed_wait both CLEAR the ECB after waking, and ecb_timed_wait POSTs
 * the ECB itself on timeout. Any of those breaks the caller's re-check loop
 * (wait_or_stop): a cleared completion is lost, a timeout-posted ECB is a phantom
 * completion. We want a wait that leaves *ecb EXACTLY as posted (or not) by the
 * real poster, so we use the raw WAIT ECBLIST primitives with a SEPARATE timeout
 * ECB. */

void nsfthr_wait(NSFECB *ecb)
{
    ECB *wl[1];

    wl[0] = (ECB *)((unsigned)ecb | 0x80000000u);   /* single entry, VL bit */
    (void)ecb_waitlist(wl);                         /* WAIT ECBLIST; no clear */
}

void nsfthr_timed_wait(NSFECB *ecb, UINT ticks)
{
    ECB *wl[1];
    ECB  tmo = 0u;

    /* On timeout ecb_timed_waitlist posts `tmo` (a discarded stack ECB), NOT
     * *ecb -- so after this call `*ecb & POSTED` reflects a REAL post only, and
     * *ecb is never cleared. A tick is 100 ms = 10 STIMER units. */
    wl[0] = (ECB *)((unsigned)ecb | 0x80000000u);
    (void)ecb_timed_waitlist(wl, &tmo, ticks * 10u, 0u);
}

int nsfthr_join(NSFTHR *t, UINT ticks)
{
    CTHDTASK *task = (CTHDTASK *)t;

    if (task == NULL) {
        return 0;
    }
    /* Bounded wait for the subtask to end (termecb, posted by MVS at task end). */
    (void)ecb_timed_wait(&task->termecb, ticks * 10u, 0u);
    if ((task->termecb & ECB_POSTED_BIT) == 0u) {
        return 1;                   /* still live -- do NOT detach a running task */
    }
    (void)cthread_detach(task);     /* DETACH STAE=YES: reclaim the subtask TCB   */
    return 0;
}
