/*
 * tsttmr.c -- NSFTMR host unit tests (spec ch. 06).
 *
 * White-box tests of the sorted delta queue, read back through the always-on
 * nsftmr_count / nsftmr_peek inspection seam, plus the re-arm / disarm behaviour
 * observed through the NSF_DEBUG-only platform-seam probe (src/nsfstim_host.c
 * records each nsftmr_plat_arm/disarm because the host has no real STIMER).
 * Portable C: builds and runs natively via `make test-host` with NSF_DEBUG=1.
 *
 * Covers the M0-5 Definition of Done: delta-queue insertion order (and the
 * successor-delta bookkeeping); several timers firing in the correct order as
 * ticks advance; cancel-from-the-middle handing its delta to the successor;
 * restart of a pending timer; idempotent cancel in any state; and nsftmr_run
 * firing all due timers, re-arming only the head delta, and doing nothing on an
 * empty queue.
 */
#include "nsftmr.h"
#include "nsfstim.h"            /* the platform-seam probe (NSF_DEBUG) */
#include <mbtcheck.h>
#include <string.h>

/* Fire recorder: each timer carries its integer id as `arg`; the callback logs
 * the ids in the order they fire so a test can assert fire order. */
static int fire_log[32];
static int fire_n;

static void rec_fn(void *arg)
{
    if (fire_n < (int)(sizeof(fire_log) / sizeof(fire_log[0]))) {
        fire_log[fire_n] = (int)(long)arg;
    }
    fire_n++;
}

/* Arm t for `ticks`, tagging it with id `id` (recovered by rec_fn). */
static void arm(TMR *t, UINT ticks, int id)
{
    tmr_start(t, ticks, rec_fn, (void *)(long)id);
}

/* Self-re-arming callback for the persist/backoff cadence test: on each fire it
 * doubles the interval and re-arms itself, up to `g_rearm_cap` fires. This is the
 * exact shape of the TCP zero-window persist probe -- the pattern that surfaced
 * issue #40 live. `g_rearm_iv` carries the next interval. */
static TMR *g_rearm_t;
static UINT g_rearm_iv;
static int  g_rearm_cap;
static void rearm_fn(void *arg)
{
    (void)arg;
    fire_n++;
    if (fire_n < g_rearm_cap) {
        g_rearm_iv *= 2u;
        tmr_start(g_rearm_t, g_rearm_iv, rearm_fn, NULL);
    }
}

int main(void)
{
    TMR a, b, c, d;
    const TMR *p;

    printf("=== nsf370 NSFTMR tests ===\n");

    /* IDLE must be 0 so a memset-cleared owner CB carries a safe-to-cancel
     * timer (the tests rely on this to arm stack TMRs). */
    CHECK_EQ((long)TMR_IDLE, 0, "TMR_IDLE is 0 (memset-safe)");

    /* ---- 1. delta-queue insertion order + successor bookkeeping ---- *
     * Arm a=3, b=8, c=5, d=3 (out of order, with a tie at 3). Expected fire
     * order by absolute deadline: a(3), d(3, after a), c(5), b(8); stored as
     * deltas 3, 0, 2, 3. */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c)); memset(&d, 0, sizeof(d));
    arm(&a, 3, 1);
    arm(&b, 8, 2);
    arm(&c, 5, 3);
    arm(&d, 3, 4);
    CHECK_EQ((long)nsftmr_count(), 4, "four timers armed");
    p = nsftmr_peek(0); CHECK(p == &a, "head is a (soonest)");
    CHECK_EQ((long)p->delta, 3, "a delta is 3");
    p = nsftmr_peek(1); CHECK(p == &d, "d follows a (tie, FIFO order)");
    CHECK_EQ((long)p->delta, 0, "d delta is 0 (co-scheduled with a)");
    p = nsftmr_peek(2); CHECK(p == &c, "c is third");
    CHECK_EQ((long)p->delta, 2, "c delta is 2 (5 - 3)");
    p = nsftmr_peek(3); CHECK(p == &b, "b is last");
    CHECK_EQ((long)p->delta, 3, "b delta is 3 (8 - 5)");
    CHECK(nsftmr_peek(4) == NULL, "peek past count returns NULL");

    /* ---- 2. several timers fire in the correct order as ticks advance ---- *
     * Same queue (a,d at 3; c at 5; b at 8). Advance in steps and check the
     * fire order is a, d, c, b. */
    fire_n = 0;
    nsftmr_run(3);              /* a and d come due together */
    CHECK_EQ((long)fire_n, 2, "run(3) fires the two timers due at tick 3");
    CHECK(fire_log[0] == 1 && fire_log[1] == 4, "fire order a then d");
    CHECK_EQ((long)nsftmr_count(), 2, "two timers remain");
    nsftmr_run(2);              /* c due at 5 */
    CHECK_EQ((long)fire_n, 3, "run(2) fires c");
    CHECK(fire_log[2] == 3, "c fires third");
    nsftmr_run(3);              /* b due at 8 */
    CHECK_EQ((long)fire_n, 4, "run(3) fires b");
    CHECK(fire_log[3] == 2, "b fires last");
    CHECK_EQ((long)nsftmr_count(), 0, "queue drained after all fire");

    /* ---- 3. cancel-from-the-middle fixes the successor delta ---- *
     * a=2, b=5, c=9 -> deltas 2, 3, 4. Cancel b: c must inherit b's delta so its
     * absolute deadline (9) is unchanged -> c delta 3+4 = 7. */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b)); memset(&c, 0, sizeof(c));
    arm(&a, 2, 1);
    arm(&b, 5, 2);
    arm(&c, 9, 3);
    p = nsftmr_peek(1); CHECK(p == &b && p->delta == 3, "b delta is 3 before cancel");
    tmr_cancel(&b);
    CHECK_EQ((long)nsftmr_count(), 2, "cancel removes b");
    CHECK_EQ((long)b.state, TMR_IDLE, "cancelled b is IDLE");
    p = nsftmr_peek(0); CHECK(p == &a && p->delta == 2, "a unchanged at the head");
    p = nsftmr_peek(1); CHECK(p == &c, "c is now second");
    CHECK_EQ((long)p->delta, 7, "c inherited b's delta (3 + 4 = 7)");
    fire_n = 0;
    nsftmr_run(9);
    CHECK(fire_n == 2 && fire_log[0] == 1 && fire_log[1] == 3,
          "after cancel only a and c fire, in order");

    /* ---- 4. restart of a pending timer ---- *
     * a=5, b=10 -> deltas 5, 5. Restart a to 12: a leaves the head, b's deadline
     * (10) is preserved (b delta 10), a re-inserts at the tail (delta 2). */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    arm(&a, 5, 1);
    arm(&b, 10, 2);
    CHECK(nsftmr_peek(0) == &a, "a is the head before restart");
    arm(&a, 12, 1);            /* restart a while pending */
    CHECK_EQ((long)nsftmr_count(), 2, "restart keeps the count at 2");
    p = nsftmr_peek(0); CHECK(p == &b && p->delta == 10,
          "b is now the head, deadline preserved (delta 10)");
    p = nsftmr_peek(1); CHECK(p == &a && p->delta == 2,
          "restarted a moved to the tail (delta 2, absolute 12)");

    /* ---- 5. idempotent cancel in any state ---- */
    nsftmr_init();
    memset(&a, 0, sizeof(a));
    tmr_cancel(&a);            /* never armed (IDLE) */
    CHECK_EQ((long)nsftmr_count(), 0, "cancel of a never-armed timer is a no-op");
    arm(&a, 5, 1);
    CHECK_EQ((long)nsftmr_count(), 1, "arm then present");
    tmr_cancel(&a);            /* PENDING -> IDLE */
    CHECK_EQ((long)nsftmr_count(), 0, "cancel removes it");
    tmr_cancel(&a);            /* already IDLE */
    CHECK_EQ((long)nsftmr_count(), 0, "second cancel is a no-op");
    CHECK_EQ((long)a.state, TMR_IDLE, "double-cancelled timer stays IDLE");

    /* ---- 6. wake accounting: a wake advances the ARMED amount, not 1 ---- *
     * The #40 regression. nsftmr_wake() is what the executive loop calls on each
     * STIMER post; it advances the queue by the exact tick count the STIMER
     * interval was armed for and RETURNS it. Asserting the sequence of return
     * values captures the cadence precisely: a delta-N timer must fire in ONE
     * wake of N ticks (the old loop consumed 1 per wake and re-armed for N-1, so
     * a delta-N timer fired after N(N+1)/2 ticks). These are portable (no STIMER
     * needed -- g_armed is tracked in nsftmr.c), so they run on host AND MVS. */
    nsftmr_init();
    memset(&a, 0, sizeof(a));
    fire_n = 0;
    arm(&a, 20, 1);                        /* the delta-20 that found #40 live */
    CHECK_EQ((long)nsftmr_wake(), 20, "single delta-20 wake advances 20 (not 1)");
    CHECK_EQ((long)fire_n, 1, "the delta-20 timer fired in that one wake");
    CHECK_EQ((long)nsftmr_count(), 0, "queue drained");
    /* The bug would have fired it after 20*21/2 = 210 ticks across 20 wakes. */

    /* ---- 7. two staggered timers fire at their own absolute deadlines ---- *
     * a=5, b=20. Wake 1 advances 5 (a fires), wake 2 advances 15 (b fires) --
     * cumulative 5 then 20, each timer on its own deadline. */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    fire_n = 0;
    arm(&a, 5, 1);
    arm(&b, 20, 2);
    CHECK_EQ((long)nsftmr_wake(), 5, "wake 1 advances 5 (a's delta)");
    CHECK(fire_n == 1 && fire_log[0] == 1, "a fired at cumulative 5");
    CHECK_EQ((long)nsftmr_wake(), 15, "wake 2 advances 15 (b's remaining, cum 20)");
    CHECK(fire_n == 2 && fire_log[1] == 2, "b fired at cumulative 20");
    CHECK_EQ((long)nsftmr_count(), 0, "both drained");

    /* ---- 8. re-arm-in-callback (the persist/backoff pattern, #40 live shape) --- *
     * A timer that doubles its interval and re-arms itself each fire: intervals
     * 10, 20, 40, 80. Each wake advances exactly the armed interval, so the
     * inter-probe deltas are 10, 20, 40, 80 -- NOT the quadratic blow-up that
     * put persist probe #2 at 210 ticks on MVSCE. */
    nsftmr_init();
    memset(&a, 0, sizeof(a));
    fire_n = 0;
    g_rearm_t = &a; g_rearm_iv = 10u; g_rearm_cap = 4;
    tmr_start(&a, g_rearm_iv, rearm_fn, NULL);
    CHECK_EQ((long)nsftmr_wake(), 10, "probe 1 interval = 10");
    CHECK_EQ((long)fire_n, 1, "probe 1 fired");
    CHECK_EQ((long)nsftmr_wake(), 20, "probe 2 interval = 20 (NOT 210: #40 regress)");
    CHECK_EQ((long)fire_n, 2, "probe 2 fired");
    CHECK_EQ((long)nsftmr_wake(), 40, "probe 3 interval = 40");
    CHECK_EQ((long)nsftmr_wake(), 80, "probe 4 interval = 80");
    CHECK_EQ((long)fire_n, 4, "four probes fired");
    CHECK_EQ((long)nsftmr_count(), 0, "queue drained (cap reached, no re-arm)");

    /* ---- 9. head-shortening: a nearer timer inserted under a long one ---- *
     * a=100 (a 2MSL-like long timer), then b=10 (a rexmit-like near timer). b
     * becomes the head, so tmr_start re-arms for 10: the near timer fires in one
     * 10-tick wake, on time -- it does NOT wait out a's 100. (The real-time
     * "a fires safe-late by the forgotten sub-interval" is an MVS property with
     * no host STIMER to elapse; the live cadence pin validates the timing. Here
     * we prove the re-arm and the near timer's on-time fire.) */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    fire_n = 0;
    arm(&a, 100, 1);
    arm(&b, 10, 2);
    CHECK(nsftmr_peek(0) == &b, "the nearer timer b is the head");
    CHECK_EQ((long)nsftmr_wake(), 10, "head-shortened wake advances 10, not 100");
    CHECK(fire_n == 1 && fire_log[0] == 2, "b fired first, on time (cum 10)");
    CHECK_EQ((long)nsftmr_wake(), 90, "a fires on the next wake (cum 100)");
    CHECK(fire_n == 2 && fire_log[1] == 1, "a fired second");

#if NSF_DEBUG
    /* ---- 10. nsftmr_run re-arms only the head, disarms on drain, no-op empty -- *
     * The platform seam is observable only through the host probe. */
    nsftmr_init();
    nsftmr_plat_probe_reset();
    nsftmr_run(5);             /* empty queue */
    CHECK_EQ((long)nsftmr_plat_arm_count(), 0, "run on empty does not arm");
    CHECK_EQ((long)nsftmr_plat_disarm_count(), 0, "run on empty does not disarm");

    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    arm(&a, 4, 1);
    arm(&b, 7, 2);             /* deltas 4, 3 */
    nsftmr_plat_probe_reset();
    nsftmr_run(2);             /* nothing due; head a reduced to 2 */
    CHECK_EQ((long)nsftmr_plat_last_arm(), 2, "partial advance re-arms the reduced head delta");
    CHECK(nsftmr_peek(0) == &a && nsftmr_peek(0)->delta == 2, "head a now delta 2");

    nsftmr_plat_probe_reset();
    nsftmr_run(2);             /* a fires; head becomes b (delta 3) */
    CHECK_EQ((long)nsftmr_plat_last_arm(), 3, "after a fires, re-arms b's delta (3)");
    CHECK(nsftmr_plat_disarm_count() == 0, "still armed: no disarm while timers remain");

    nsftmr_plat_probe_reset();
    nsftmr_run(3);             /* b fires; queue drains */
    CHECK_EQ((long)nsftmr_count(), 0, "queue drained");
    CHECK(nsftmr_plat_disarm_count() >= 1, "draining the queue disarms the platform timer");

    /* ---- 11. tmr_start arms on the empty->nonempty bootstrap (ADR-0034 #2) --- *
     * Before the fix, tmr_start into an empty queue did NOT arm -- the timer
     * fired only if something else woke the loop (the "bootstrap hole" the
     * nsfmain heartbeat papered over). Now the first insert arms the STIMER for
     * its own delta. */
    nsftmr_init();
    memset(&a, 0, sizeof(a));
    nsftmr_plat_probe_reset();
    arm(&a, 7, 1);
    CHECK_EQ((long)nsftmr_plat_arm_count(), 1, "insert into empty queue arms once");
    CHECK_EQ((long)nsftmr_plat_last_arm(), 7, "armed for the new timer's delta (7)");

    /* ---- 12. tmr_cancel disarms when it empties the queue (ADR-0034 #3) ---- *
     * The blocker: without this, the self-re-arming STIMER exit keeps firing on
     * an empty queue (spec 6.3 "zero idle interrupts" violated). Cancelling a
     * non-last timer must NOT disarm; cancelling the last one must. */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    arm(&a, 4, 1);
    arm(&b, 9, 2);
    nsftmr_plat_probe_reset();
    tmr_cancel(&b);            /* queue not empty (a remains) */
    CHECK_EQ((long)nsftmr_plat_disarm_count(), 0, "cancel with a timer left does not disarm");
    tmr_cancel(&a);            /* now empties the queue */
    CHECK_EQ((long)nsftmr_plat_disarm_count(), 1, "cancel that empties the queue disarms");

    /* ---- 13. head-shortening tmr_start re-arms for the nearer delta (#3) ---- *
     * a=100 armed; b=10 inserted becomes the head, so the STIMER is re-armed
     * from 100 down to 10 -- the near timer will fire on time, not after 100. */
    nsftmr_init();
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    arm(&a, 100, 1);
    CHECK_EQ((long)nsftmr_plat_last_arm(), 100, "armed for a's delta (100)");
    nsftmr_plat_probe_reset();
    arm(&b, 10, 2);
    CHECK_EQ((long)nsftmr_plat_arm_count(), 1, "head-shortening insert re-arms once");
    CHECK_EQ((long)nsftmr_plat_last_arm(), 10, "re-armed for b's nearer delta (10)");
#endif

    return mbt_test_summary("TSTTMR");
}
