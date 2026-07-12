/*
 * tstthrw.c -- Stage 0 of issue #21: prove ON MVS which timed-wait shape
 * actually times out on a cthread subtask (and on the main task), before any
 * driver behavior is built on it. MVS-only (project.toml host = false): the
 * pthread host shim's pthread_cond_timedwait always fires, so the host can
 * never distinguish the two shapes (the exact blind spot #21 hid in).
 *
 * THE LINCHPIN. libc370 ecb_timed_waitlist(waitlist, timeecb, bintvl, code)
 * arms STIMER REAL whose exit POSTs *timeecb -- and then WAITs on `waitlist`
 * AS GIVEN. A POST of an ECB that is NOT in the waitlist does not satisfy the
 * WAIT, so the "timeout" of
 *
 *     wl[0] = ecb|VL;  ecb_timed_waitlist(wl, &tmo, ...)     (tmo NOT in wl)
 *
 * posts a dead stack ECB and never wakes the waiter: it is a pure infinite
 * WAIT. That was nsfthr_timed_wait's shape through M2 -- so the CTCI subtask's
 * 500 ms self-poll (wait_or_stop) was a no-op and a lost txgoecb/wecb wake was
 * permanent (the 90 s burst-tail stall). ecb_timed_wait/cthread_timed_wait
 * "work" only because they pass the SAME ecb as list entry and timeecb -- which
 * is also why TSTCTHR's subtask naps fired (cthread_timed_wait) while ADR-0023
 * §6 saw an executive ecb_timed_waitlist timeout "not fire": one bug, two
 * misread symptoms. cthread_timed_wait is NOT the fix -- it clears the ECB and
 * phantom-posts it on timeout (the exact hazards ADR-0023 §5 rejects). The fix
 * (ADR-0025) keeps the separate timeout ECB but puts it IN the waitlist.
 *
 * WHAT THIS JOB PROVES (read build/test-runner.spool, not just the matrix):
 *   T1  the OLD shape (verbatim local copy), on a subtask, ECB never posted:
 *       does NOT return within ~2 s of heartbeats; returns only when the
 *       parent finally posts the ECB for real. If T1 ever returns in ~500 ms,
 *       the #21 analysis is wrong: STOP and re-open before trusting the rest.
 *   T2  the FIXED nsfthr_timed_wait, on a subtask: returns in ~500 ms with the
 *       target ECB UNTOUCHED (no phantom post -- the wait_or_stop contract).
 *   T3  the FIXED nsfthr_timed_wait on the MAIN task (heartbeat disarmed --
 *       ecb_timed_waitlist TTIMER-CANCELs the caller's STIMER, ADR-0023 §6):
 *       also ~500 ms. This is the shutdown-window nsfthr_join bound.
 *   T4  nsfthr_join of a parked (live) subtask times out -> rc 1 (RETAIN, not
 *       an unbounded hang); after a real release it joins clean -> rc 0.
 */
#include "nsfthr.h"
#include "nsfstim.h"            /* nsftmr_plat_arm / _disarm / _ecb            */
#include "nsftime.h"            /* nsf_now (STCK) for elapsed measurement      */

#include <clibecb.h>           /* ECB, ecb_waitlist, ecb_timed_waitlist        */
#include <mbtcheck.h>
#include <stdio.h>

#define POLL_TICKS   5u         /* the CTCI_POLL_TICKS value: 5 = 500 ms       */
#define HANG_TICKS   20u        /* ~2 s of 100 ms heartbeats = "did not fire"  */
#define BOUND_ITERS  600u       /* safety cap on any parent wake loop          */

/* Microseconds between two STCK readings (TOD bit 51 == 1 us), as in
 * test/mvs/tsttmacc.c. Intervals here are < 5 s, so the low-word term rules. */
static UINT tod_diff_us(const NSFTIME *a, const NSFTIME *b)
{
    UINT dlo    = b->lo - a->lo;
    UINT borrow = (b->lo < a->lo) ? 1u : 0u;
    UINT dhi    = b->hi - a->hi - borrow;

    return (dhi << 20) | (dlo >> 12);
}

/* ---- T1: the OLD nsfthr_timed_wait shape, copied verbatim ------------------
 * (src/nsfthr.c as of M2: timeout ECB `tmo` NOT in the waitlist.) Kept here so
 * the defect stays pinned: if libc370 ever changes ecb_timed_waitlist to wait
 * on timeecb implicitly, T1 flips and we re-read ADR-0025. */
static void old_timed_wait(NSFECB *ecb, UINT ticks)
{
    ECB *wl[1];
    ECB  tmo = 0u;

    wl[0] = (ECB *)((unsigned)ecb | 0x80000000u);
    (void)ecb_timed_waitlist(wl, &tmo, ticks * 10u, 0u);
}

/* Cross-task cells for the three subtask probes. Each subtask measures its own
 * elapsed time (STCK on its own TCB), stores it, and posts its done ECB. */
static NSFECB g_neverA, g_doneA;  static UINT g_elapA_ms;
static NSFECB g_neverB, g_doneB;  static UINT g_elapB_ms;
static NSFECB g_park;

static int sub_old_shape(void *arg)
{
    NSFTIME t0, t1;

    (void)arg;
    nsf_now(&t0);
    old_timed_wait(&g_neverA, POLL_TICKS);      /* 500 ms nominal */
    nsf_now(&t1);
    g_elapA_ms = tod_diff_us(&t0, &t1) / 1000u;
    nsfthr_post(&g_doneA, 1u);
    return 0;
}

static int sub_fixed_shape(void *arg)
{
    NSFTIME t0, t1;

    (void)arg;
    nsf_now(&t0);
    nsfthr_timed_wait(&g_neverB, POLL_TICKS);   /* the FIXED seam */
    nsf_now(&t1);
    g_elapB_ms = tod_diff_us(&t0, &t1) / 1000u;
    nsfthr_post(&g_doneB, 1u);
    return 0;
}

/* T4: park until released (a plain, un-timed wait), then end. */
static int sub_parker(void *arg)
{
    (void)arg;
    nsfthr_wait(&g_park);
    return 0;
}

/* Parent-side bounded wake loop: WAIT on {done, timerecb}, counting heartbeat
 * ticks, until `done` is posted or `maxticks` heartbeats passed. Returns the
 * tick count when `done` was seen posted, or maxticks if it never was. */
static UINT ticks_until_posted(NSFECB *done, ECB *timerecb, UINT maxticks)
{
    UINT ticks = 0u;
    UINT iters = 0u;

    while (ticks < maxticks && iters < BOUND_ITERS) {
        ECB *wl[2];

        if (*done & NSFECB_POSTED) {
            return ticks;
        }
        wl[0] = (ECB *)done;
        wl[1] = (ECB *)((unsigned)timerecb | 0x80000000u);
        (void)ecb_waitlist(wl);
        iters++;
        if (*timerecb & ECB_POSTED_BIT) {
            *timerecb = 0u;                 /* the exit self-re-arms (ADR-0017) */
            ticks++;
        }
    }
    return (*done & NSFECB_POSTED) ? ticks : maxticks;
}

int main(void)
{
    ECB     *timerecb = (ECB *)nsftmr_plat_ecb();
    NSFTHR  *t;
    UINT     ticks;

    printf("=== nsf370 TSTTHRW: timed-wait linchpin (issue #21 Stage 0) ===\n");

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");

    nsftmr_plat_arm(1u);                    /* ~100 ms parent heartbeat */

    /* ---- T1: old shape on a subtask must NOT time out ---- */
    g_neverA = 0u; g_doneA = 0u; g_elapA_ms = 0u;
    t = nsfthr_create(sub_old_shape, NULL);
    CHECK(t != NULL, "T1: subtask (old shape) created");
    ticks = ticks_until_posted(&g_doneA, timerecb, HANG_TICKS);
    printf("T1: old shape after %u ticks: done=%08X\n",
           (unsigned)ticks, (unsigned)g_doneA);
    CHECK(!(g_doneA & NSFECB_POSTED),
          "T1: OLD shape did NOT time out in ~2 s (tmo not in waitlist => no wake)");
    /* Release the parked subtask with a REAL post, prove it was the only wake. */
    nsfthr_post(&g_neverA, 1u);
    ticks = ticks_until_posted(&g_doneA, timerecb, HANG_TICKS);
    CHECK((g_doneA & NSFECB_POSTED) != 0u, "T1: real post released the subtask");
    printf("T1: subtask elapsed %u ms (nominal timeout was 500)\n",
           (unsigned)g_elapA_ms);
    CHECK(g_elapA_ms >= 1500u,
          "T1: it waited until the release (>= 1500 ms), not the 500 ms timer");
    CHECK_EQ((long)nsfthr_join(t, 20u), 0, "T1: subtask joined");

    /* ---- T2: FIXED nsfthr_timed_wait on a subtask fires at ~500 ms ---- */
    g_neverB = 0u; g_doneB = 0u; g_elapB_ms = 0u;
    t = nsfthr_create(sub_fixed_shape, NULL);
    CHECK(t != NULL, "T2: subtask (fixed seam) created");
    ticks = ticks_until_posted(&g_doneB, timerecb, HANG_TICKS);
    printf("T2: fixed seam returned after %u ticks, elapsed %u ms, ecb=%08X\n",
           (unsigned)ticks, (unsigned)g_elapB_ms, (unsigned)g_neverB);
    CHECK((g_doneB & NSFECB_POSTED) != 0u,
          "T2: FIXED timed wait returned on its own (subtask self-poll fires)");
    CHECK(g_elapB_ms >= 300u && g_elapB_ms <= 1200u,
          "T2: elapsed ~500 ms (timer-driven, not an immediate or late return)");
    CHECK(!(g_neverB & NSFECB_POSTED),
          "T2: target ECB untouched by the timeout (no phantom completion)");
    CHECK_EQ((long)nsfthr_join(t, 20u), 0, "T2: subtask joined");

    /* ---- T3/T4 run on the MAIN task: disarm the heartbeat first (the timed
     * wait TTIMER-CANCELs the caller's STIMER -- the ADR-0023 §6 constraint;
     * this is exactly the shutdown window where nsfthr_join runs for real). */
    nsftmr_plat_disarm();

    {
        NSFTIME t0, t1;
        NSFECB  neverM = 0u;
        UINT    ms;

        nsf_now(&t0);
        nsfthr_timed_wait(&neverM, POLL_TICKS);
        nsf_now(&t1);
        ms = tod_diff_us(&t0, &t1) / 1000u;
        printf("T3: fixed seam on the MAIN task elapsed %u ms, ecb=%08X\n",
               (unsigned)ms, (unsigned)neverM);
        CHECK(ms >= 300u && ms <= 1200u,
              "T3: timed wait fires on the main task too (join bound is real)");
        CHECK(!(neverM & NSFECB_POSTED), "T3: target ECB untouched");
    }

    /* ---- T4: join bound on a live (parked) subtask ---- */
    g_park = 0u;
    t = nsfthr_create(sub_parker, NULL);
    CHECK(t != NULL, "T4: parker subtask created");
    CHECK_EQ((long)nsfthr_join(t, POLL_TICKS), 1,
             "T4: join of a live subtask timed out -> RETAIN (rc 1), no hang");
    nsfthr_post(&g_park, 1u);               /* release it */
    CHECK_EQ((long)nsfthr_join(t, 20u), 0, "T4: released subtask joined clean");

    return mbt_test_summary("TSTTHRW");
}
