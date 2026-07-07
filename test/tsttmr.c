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

#if NSF_DEBUG
    /* ---- 6. nsftmr_run re-arms only the head, disarms on drain, no-op empty --- *
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
#endif

    return mbt_test_summary("TSTTMR");
}
