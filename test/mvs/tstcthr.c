/*
 * tstcthr.c -- Stage 0 of M1-4b (issue #18): de-risk the libc370 `cthread`
 * seam IN ISOLATION, before any CTCI I/O is wired to it. MVS-only (project.toml
 * host = false): cthread / IDENTIFY / WAIT ECBLIST are MVS services with no host
 * analog. `cthread` is the FIRST use anywhere in the ecosystem code we can see
 * (ADR-0022), so nothing below is assumed -- it is proven here or M1-4b stops.
 *
 * THE ASSUMPTION THIS PROVES (the exact hinge #18 turns on).  The M1-4b fix wakes
 * the executive from a CTCI I/O subtask with cthread_post(&dev->ecb) -- a real
 * SVC-2 MVS POST (cthread_post -> ecb_post -> @@ECBPST, confirmed in libc370).
 * #18 hung because *IOS* posted a raw IOB ECB into the executive's multi-ECB
 * `WAIT ECBLIST`, out of phase, and a later operator/stop POST no longer woke it.
 * The fix bets that a *normal-task* SVC-2 POST into that same multi-ECB WAIT
 * behaves normally -- it wakes the parent AND leaves the WAIT healthy for the
 * loop's own ECBs.  So the primary test (TEST 1) drives a cthread subtask
 * SVC-2-posting one ECB while the async STIMER exit (the loop's own timer
 * heartbeat -- the operator/stop analog) posts the other, BOTH in one
 * `WAIT ECBLIST`, over many rounds, and proves neither source is ever lost.  If
 * this carried #18's pathology it would hang HERE, in isolation -- not after two
 * subtasks and the queues are built at Stage 2.
 *
 * PREREQUISITE surfaced here (a real M1-4b finding).  ATTACH EP=CTHREAD needs
 * CTHREAD IDENTIFYed.  libc370 does that inside clib_apf_setup() -- but NSF is
 * deliberately UNAUTHORIZED (ADR-0022) and never calls it.  So NSF must call
 * clib_identify_cthread() itself: a bare IDENTIFY (SVC 41, problem state) that
 * also force-links the CTHREAD module via its =V(CTHREAD).  The subtask then runs
 * unauthorized (AC(0), problem state, key 8), exactly as ADR-0022 requires.
 *
 * TEST 2 joins the subtask through CTHDTASK.termecb (posted by MVS at task end)
 * + cthread_detach -- the clean-shutdown join M1-4b needs at dev_shutdown.
 * TEST 3 forces a fault inside cthread_pop(CTHDPOP_ESTAE) and proves the PARENT
 * survives and still detaches -- the subtask-recovery path M1-4b needs under
 * nsf_recover.  It runs LAST: the forced S0C4 may cut a dump, which must not
 * obscure TEST 1/2 above.
 *
 * Diagnosing the outcome (read build/test-runner.spool, not just the matrix):
 *   - CC 0 + "TEST1 OK"        -> SVC-2-into-multi-ECB WAIT is healthy: proceed.
 *   - hang / no completion     -> the fix's premise is WRONG; #18 lives in
 *                                 cthread too.  STOP and report -- do NOT build
 *                                 Stage 1+ on it.
 *   - ABEND before "TEST1"     -> IDENTIFY / ATTACH linkage is wrong.
 */
#include "nsfstim.h"            /* nsftmr_plat_arm / _disarm / _ecb (STIMER)   */

#include <clibos.h>            /* clib_identify_cthread (IDENTIFY CTHREAD)     */
#include <clibthrd.h>          /* cthread_create / _post / _detach / _push /... */
#include <clibecb.h>          /* ECB, ecb_waitlist, ecb_timed_wait, bits      */

#include <mbtcheck.h>
#include <stdio.h>

#define K_POSTS   20u          /* SVC-2 POSTs the subtask fires into the WAIT  */
#define N_TIMER    8u          /* STIMER heartbeats the WAIT must also observe */
#define MAX_ITERS 600u         /* safety cap (a real WAIT hang shows as a job  */
                               /* hang; this only bounds a logic bug)         */

/* ---- TEST 1 poster subtask: SVC-2 POST the parent ECB K times ------------- *
 * Runs on its OWN TCB.  Naps ~50 ms between posts (cthread_timed_wait on a
 * private ECB nobody posts -- it self-posts on timeout), so its posts interleave
 * with the parent's ~100 ms STIMER heartbeat.  Each cthread_post is a real SVC-2
 * MVS POST of the parent's `subecb`, which the parent is holding in a multi-ECB
 * WAIT ECBLIST.  Returns 0 -> terminates -> MVS posts CTHDTASK.termecb (TEST 2). */
static int poster_sub(void *a1, void *a2)
{
    ECB     *subecb = (ECB *)a1;
    unsigned k      = (unsigned)a2;
    unsigned i;

    for (i = 0u; i < k; i++) {
        ECB nap = 0u;
        cthread_timed_wait(&nap, 5u, 0u);   /* ~50 ms nap (self-posts on timeout) */
        cthread_post(subecb, 1u);           /* SVC-2 POST into the parent's WAIT  */
    }
    return 0;
}

/* ---- TEST 3 fault subtask: prove the subtask ESTAE catches a fault -------- *
 * estae_risky is pushed (cthread_push) then popped under an ESTAE
 * (cthread_pop(CTHDPOP_ESTAE)); the store through NULL cuts an S0C4 the ESTAE
 * must intercept.  `arg` (NULL) is taken through a volatile pointer so cc370
 * cannot fold the store away.  The point is only that the PARENT survives and can
 * still join+detach -- ATTACH already isolates the fault to this TCB; this proves
 * cthread's recovery wrapper on top of it works. */
static int estae_risky(void *arg)
{
    volatile int *p = (volatile int *)arg;   /* arg == NULL */
    *p = 1;                                   /* S0C4: store via NULL */
    return 99;                                /* not reached */
}

static int estae_sub(void *a1, void *a2)
{
    (void)a1;
    (void)a2;
    cthread_push(estae_risky, NULL);
    return cthread_pop(CTHDPOP_ESTAE);        /* run risky under ESTAE */
}

int main(void)
{
    ECB        subecb   = 0u;
    ECB       *timerecb = (ECB *)nsftmr_plat_ecb();
    CTHDTASK  *sub;
    int        idrc;
    unsigned   iters      = 0u;
    unsigned   sub_wakes  = 0u;
    unsigned   tmr_wakes  = 0u;

    printf("=== nsf370 TSTCTHR: cthread seam de-risk (M1-4b Stage 0) ===\n");

    /* PREREQUISITE: IDENTIFY CTHREAD so ATTACH EP=CTHREAD resolves, WITHOUT APF
     * (NSF stays unauthorized). Force-links the CTHREAD module via =V(CTHREAD). */
    idrc = clib_identify_cthread();
    printf("clib_identify_cthread rc=%d\n", idrc);
    CHECK_EQ((long)idrc, 0, "IDENTIFY CTHREAD succeeded (ATTACH EP=CTHREAD ready)");

    /* ---- TEST 1: SVC-2 POST from a subtask into the executive's multi-ECB
     * WAIT, alongside the async STIMER heartbeat. THE LINCHPIN. ---- */
    sub = cthread_create((void *)poster_sub, &subecb, (void *)K_POSTS);
    printf("cthread_create sub=%08X tcb=%08X\n",
           (unsigned)sub, sub ? sub->tcb : 0u);
    CHECK(sub != NULL, "cthread_create ATTACHed the poster subtask");
    if (sub == NULL) {
        return mbt_test_summary("TSTCTHR");     /* nothing below can run */
    }

    nsftmr_plat_arm(1u);                        /* arm the ~100 ms heartbeat */

    while ((sub_wakes < K_POSTS || tmr_wakes < N_TIMER) && iters < MAX_ITERS) {
        ECB *wl[2];

        wl[0] = &subecb;
        wl[1] = (ECB *)((unsigned)timerecb | 0x80000000u);   /* VL: end of list */
        ecb_waitlist(wl);                       /* WAIT ECBLIST (SVC 1) */
        iters++;

        if (subecb & ECB_POSTED_BIT) {          /* subtask SVC-2 POST woke us */
            subecb = 0u;
            sub_wakes++;
        }
        if (*timerecb & ECB_POSTED_BIT) {       /* STIMER exit woke us */
            *timerecb = 0u;                     /* the exit self-re-arms (ADR-0017) */
            tmr_wakes++;
        }
    }
    nsftmr_plat_disarm();

    printf("TEST1: sub_wakes=%u (want %u)  tmr_wakes=%u (want %u)  iters=%u\n",
           sub_wakes, (unsigned)K_POSTS, tmr_wakes, (unsigned)N_TIMER, iters);
    CHECK(sub_wakes >= K_POSTS,
          "every subtask SVC-2 POST woke the multi-ECB WAIT (no lost wakeup)");
    CHECK(tmr_wakes >= N_TIMER,
          "STIMER heartbeat kept waking the WAIT alongside subtask POSTs (no #18 hang)");
    if (sub_wakes >= K_POSTS && tmr_wakes >= N_TIMER) {
        printf("TEST1 OK -- SVC-2 POST into a multi-ECB WAIT is healthy\n");
    }

    /* ---- TEST 2: join the subtask via termecb, then detach ---- */
    {
        int wrc = ecb_timed_wait(&sub->termecb, 500u, 0u);   /* <= 5 s for term */
        printf("TEST2: termecb=%08X (timed_wait rc=%d)\n",
               (unsigned)sub->termecb, wrc);
        CHECK((sub->termecb & ECB_POSTED_BIT) != 0u,
              "subtask CTHDTASK.termecb posted by MVS at task end");
        idrc = cthread_detach(sub);
        printf("TEST2: cthread_detach rc=%d tcb=%08X\n", idrc, sub->tcb);
        CHECK_EQ((long)sub->tcb, 0, "detach cleared the subtask TCB (joined)");
    }

    /* ---- TEST 3: subtask ESTAE -- a fault must not take the parent down.
     * LAST, because the forced S0C4 may cut a SYSUDUMP. ---- */
    {
        CTHDTASK *est = cthread_create((void *)estae_sub, NULL, NULL);

        printf("TEST3: estae subtask=%08X\n", (unsigned)est);
        CHECK(est != NULL, "cthread_create ATTACHed the ESTAE subtask");
        if (est != NULL) {
            int wrc = ecb_timed_wait(&est->termecb, 500u, 0u);
            printf("TEST3: termecb=%08X (wait rc=%d) subrc=%d\n",
                   (unsigned)est->termecb, wrc, est->rc);
            idrc = cthread_detach(est);
            printf("TEST3: cthread_detach rc=%d\n", idrc);
            CHECK((est->termecb & ECB_POSTED_BIT) != 0u || est->tcb == 0u,
                  "ESTAE subtask ended + joined (parent isolated from the fault)");
        }
    }

    printf("PARENT ALIVE after all cthread tests\n");
    CHECK(1, "parent reached the end (survived subtask lifecycle + fault)");

    return mbt_test_summary("TSTCTHR");
}
