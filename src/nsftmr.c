/*
 * nsftmr.c -- the Timer Manager (see nsftmr.h, spec ch. 06).
 *
 * A single sorted delta queue of embedded TMR blocks (ADR-0010). All state is
 * the one file-scope QUEUE below; TMRs are owned by their caller CBs and only
 * linked here, so this module never allocates. The queue is single-task state
 * (the executive), so there is no locking (goal 3): tmr_start / tmr_cancel /
 * nsftmr_run all run to completion on the mainline.
 *
 * Delta invariant: for the queue h0, h1, ... hk (head first), timer hi fires
 * after sum(delta0..deltai) ticks. Kept true by three local rules --
 *   insert : new->delta = ticks - (sum of deltas before it); the successor it
 *            splices in front of loses that same amount (its deadline is fixed);
 *   cancel : the removed timer hands its delta to its successor (deadline fixed);
 *   fire   : the head's delta has genuinely elapsed and is simply consumed --
 *            the successor's delta already measures from the head's fire, so it
 *            is NOT handed back (this is the one place cancel and fire differ).
 *
 * PLATFORM ARMING CONTRACT (ADR-0034). Every touch of the nsfstim.h seam goes
 * through the two helpers below (tmr_arm / tmr_disarm) so a single module word,
 * g_armed, always equals what the STIMER is physically armed for. The seam
 * clears+re-arms on every arm, so g_armed and the hardware can never diverge;
 * the STIMER therefore fires exactly g_armed ticks after it was armed, and
 * nsftmr_wake advances the queue by exactly g_armed -- the queue clock stays
 * locked to real time (never early, never late at the wake). The three
 * arming responsibilities the contract pins:
 *   1. Wake accounting: the executive calls nsftmr_wake() (NOT nsftmr_run(1u)),
 *      which advances by g_armed -- the ticks the fired STIMER interval covered.
 *   2. Empty->nonempty bootstrap AND head-shortening: tmr_start arms whenever
 *      the inserted timer becomes the head (an empty queue, or a nearer deadline
 *      than the current head). Re-arming from "now" is always safe-late for the
 *      timers already queued (their forgotten sub-interval only delays them, and
 *      v1's long timers -- 2MSL -- tolerate that; the shortened timer, the one
 *      that matters, fires on time). No TTIMER-residual seam change (ADR-0034).
 *   3. Drain: BOTH drain points disarm -- nsftmr_run when a fire empties the
 *      queue, and tmr_cancel when a cancel empties it. INVARIANT: queue empty
 *      <=> STIMER disarmed <=> g_armed == 0. This is what preserves spec 6.3's
 *      "an idle stack takes zero timer interrupts": the async STIMER exit
 *      self-re-arms unconditionally, so a queue emptied WITHOUT a disarm would
 *      keep firing forever.
 */
#include "nsftmr.h"
#include "nsfstim.h"

/* The one sorted delta queue. q_init makes head a circular sentinel; maxcount 0
 * is irrelevant here because timers are linked by hand (not q_enq) -- the
 * population is bounded by the embedded TMRs across a bounded set of CBs. */
static QUEUE g_tmrq;

/* Ticks the platform STIMER is currently armed for (0 == disarmed). Mutated
 * ONLY by tmr_arm / tmr_disarm, which also drive the seam, so g_armed and the
 * hardware never diverge (see the contract note above). */
static UINT  g_armed;

/* The single owners of the nsfstim.h seam: keep g_armed and the STIMER in step. */
static void tmr_arm(UINT ticks)
{
    g_armed = ticks;
    nsftmr_plat_arm(ticks);             /* seam clears the ECB, then arms */
}

static void tmr_disarm(void)
{
    g_armed = 0u;
    nsftmr_plat_disarm();               /* TTIMER CANCEL (idempotent) */
}

void nsftmr_init(void)
{
    q_init(&g_tmrq, 0);
    tmr_disarm();                       /* clean slate: nothing armed */
}

void tmr_start(TMR *t, UINT ticks, TMRFN fn, void *arg)
{
    QELEM *cur;
    UINT   acc;

    if (ticks == 0u) {
        ticks = 1u;                     /* soonest deadline is the next tick */
    }

    if (t->state == TMR_PENDING) {
        tmr_cancel(t);                  /* restart: re-insert at the new deadline */
    }

    t->fn  = fn;
    t->arg = arg;

    /* Find the splice point: the first queued timer whose absolute deadline is
     * strictly later than `ticks`. `acc` accumulates the deltas ahead of it. */
    acc = 0u;
    for (cur = g_tmrq.head.next; cur != &g_tmrq.head; cur = cur->next) {
        TMR *c = Q_ENTRY(cur, TMR, q);
        if (acc + c->delta > ticks) {
            break;
        }
        acc += c->delta;
    }

    t->delta = ticks - acc;             /* delta relative to the predecessor */
    if (cur != &g_tmrq.head) {
        TMR *succ = Q_ENTRY(cur, TMR, q);
        succ->delta -= t->delta;        /* successor now measured after `t` */
    }

    /* Splice `t` in before `cur` (before the sentinel == append at the tail). */
    t->q.next       = cur;
    t->q.prev       = cur->prev;
    cur->prev->next = &t->q;
    cur->prev       = &t->q;
    g_tmrq.count++;
    t->state = TMR_PENDING;

    /* If `t` is now the head, the soonest deadline changed: (re-)arm the STIMER
     * for its delta (ADR-0034 responsibility 2). This covers both the
     * empty->nonempty bootstrap (t is the only timer) and head-shortening (t's
     * deadline is nearer than the old head's). A tail insert leaves the head --
     * and g_armed -- unchanged. Re-arming clears the old interval and starts
     * afresh from now, so no timer ever fires early; only timers already queued
     * lose the elapsed sub-interval (safe-late, immaterial for v1). */
    if (g_tmrq.head.next == &t->q) {
        tmr_arm(t->delta);
    }
}

void tmr_cancel(TMR *t)
{
    QELEM *nx;

    if (t->state != TMR_PENDING) {
        return;                         /* idempotent: IDLE or never armed */
    }

    nx = t->q.next;                     /* successor before we unlink */
    if (nx != &g_tmrq.head) {
        TMR *succ = Q_ENTRY(nx, TMR, q);
        succ->delta += t->delta;        /* successor keeps its absolute deadline */
    }

    q_remove(&g_tmrq, &t->q);
    t->state = TMR_IDLE;
    t->delta = 0u;

    /* Cancel-drain (ADR-0034 responsibility 3): if this emptied the queue,
     * disarm -- the async STIMER exit self-re-arms unconditionally, so an
     * emptied queue left armed would keep taking timer interrupts forever
     * (spec 6.3 "an idle stack takes zero timer interrupts"). Cancelling a
     * non-head or a head-with-successor does NOT re-arm: the successor's
     * deadline is >= the cancelled timer's, so the armed interval fires no later
     * than needed and the next wake reconciles -- never early. */
    if (Q_EMPTY(&g_tmrq)) {
        tmr_disarm();
    }
}

void nsftmr_run(UINT ticks)
{
    UINT remaining = ticks;

    if (Q_EMPTY(&g_tmrq)) {
        return;                         /* nothing armed: do nothing */
    }

    /* Consume `ticks` from the head of the queue, firing each timer whose delta
     * is fully covered. The surviving head keeps the unconsumed remainder. Each
     * timer is detached and marked IDLE before its callback runs, so a callback
     * may re-arm the very timer that fired. The loop is bounded by `ticks` (each
     * fire consumes delta >= 0 and co-scheduled delta-0 timers are finite). */
    while (!Q_EMPTY(&g_tmrq)) {
        TMR *h = Q_ENTRY(g_tmrq.head.next, TMR, q);

        if (h->delta > remaining) {
            h->delta -= remaining;      /* partial: head not yet due */
            break;
        }

        remaining -= h->delta;
        q_remove(&g_tmrq, &h->q);
        h->state = TMR_IDLE;
        h->delta = 0u;
        if (h->fn != NULL) {
            h->fn(h->arg);
        }
    }

    /* Re-arm the platform timer for the new head, or disarm if the queue
     * drained (the fire-drain point of the ADR-0034 invariant; spec 6.3: an idle
     * stack takes zero timer interrupts). Both go through the helpers so g_armed
     * tracks the hardware. */
    if (!Q_EMPTY(&g_tmrq)) {
        TMR *nh = Q_ENTRY(g_tmrq.head.next, TMR, q);
        tmr_arm(nh->delta);
    } else {
        tmr_disarm();
    }
}

UINT nsftmr_wake(void)
{
    UINT n = g_armed;                   /* ticks the fired STIMER interval covered */

    /* Advance the queue by exactly what the STIMER was armed for -- NOT 1
     * (ADR-0034 responsibility 1: the #40 fix). nsftmr_run re-arms the new head
     * through tmr_arm, so g_armed is refreshed for the next wake. A spurious
     * wake with nothing armed (g_armed == 0, e.g. a stray POST) advances 0 and
     * is a no-op; the caller counts it. */
    nsftmr_run(n);
    return n;
}

UINT nsftmr_count(void)
{
    return g_tmrq.count;
}

const TMR *nsftmr_peek(UINT i)
{
    QELEM *cur;
    UINT   k = 0u;

    for (cur = g_tmrq.head.next; cur != &g_tmrq.head; cur = cur->next) {
        if (k == i) {
            return Q_ENTRY(cur, TMR, q);
        }
        k++;
    }
    return NULL;
}
