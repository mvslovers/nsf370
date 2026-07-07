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
 * Platform arming (nsfstim.h seam) is touched only by nsftmr_init (disarm, clean
 * slate) and nsftmr_run (re-arm the head, or disarm on drain). Per spec 6.3 at
 * M0-5, tmr_start / tmr_cancel that shorten the head do NOT re-arm STIMER; that
 * belongs with the executive loop that owns the pending-STIMER state (M0-6).
 */
#include "nsftmr.h"
#include "nsfstim.h"

/* The one sorted delta queue. q_init makes head a circular sentinel; maxcount 0
 * is irrelevant here because timers are linked by hand (not q_enq) -- the
 * population is bounded by the embedded TMRs across a bounded set of CBs. */
static QUEUE g_tmrq;

void nsftmr_init(void)
{
    q_init(&g_tmrq, 0);
    nsftmr_plat_disarm();               /* clean slate: nothing armed */
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
     * drained (spec 6.3: an idle stack takes zero timer interrupts). */
    if (!Q_EMPTY(&g_tmrq)) {
        TMR *nh = Q_ENTRY(g_tmrq.head.next, TMR, q);
        nsftmr_plat_arm(nh->delta);
    } else {
        nsftmr_plat_disarm();
    }
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
