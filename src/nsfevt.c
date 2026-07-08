/*
 * nsfevt.c -- the Event Dispatcher / executive main loop (see nsfevt.h, ch. 05).
 *
 * All state is the file-scope block below; it is single-task (the executive),
 * so there is no locking except the CS-based exit->mainline handoff (NSFXQ) and
 * the ECB POST. The loop (evt_mainloop) is the §5.3 contract: WAIT on the ECB
 * list unless there is pending work, drain the interrupt-safe handoff into the
 * event queue, dispatch up to a drain budget, run due timers, kick output.
 */
#include "nsfevt.h"
#include "nsfevtp.h"
#include "nsfmm.h"
#include "nsfxq.h"
#include "nsftmr.h"
#include "nsfstim.h"            /* nsftmr_plat_ecb / _disarm (the timer ECB) */

/* EVT pool size. Comfortably larger than the drain budget so the budget test
 * (post > 64 events, prove the loop still re-WAITs and fires timers) has events
 * to spare; a few dozen live events per stack is the working set. */
#define EVT_POOL_COUNT  128

static EVHANDLER g_handlers[EV_MAX];    /* registered per event type          */
static QUEUE     g_evq;                 /* the event queue (FIFO)             */
static XQ        g_xq;                  /* exit->mainline handoff (LIFO)      */
static MMPOOL   *g_evtpool;             /* the EVT pool                       */
static NSFECB    g_stopecb;             /* stop request                       */
static NSFECB    g_handoffecb;          /* handoff ready (M0-6 = devECB[0])   */
static int       g_stop;                /* orderly-stop flag                  */
static UINT      g_ticks;               /* timer wakes serviced               */
static UINT      g_drops;               /* evt_post pool-exhaustion drops     */

int nsfevt_init(void)
{
    int i;

    for (i = 0; i < EV_MAX; i++) {
        g_handlers[i] = NULL;
    }
    q_init(&g_evq, 0);                  /* pool-bounded, so unbounded here    */
    xq_init(&g_xq);
    g_stopecb    = 0u;
    g_handoffecb = 0u;
    g_stop       = 0;
    g_ticks      = 0u;
    g_drops      = 0u;

    /* Create the pool once (init window); later calls only reset the loop state
     * and reuse it -- so a host test can re-init between scenarios without a
     * second mm_pool_create (which would ABEND after mm_init_complete). */
    if (g_evtpool == NULL) {
        g_evtpool = mm_pool_create("NSFEVT  ", (USHORT)sizeof(EVT),
                                   EVT_POOL_COUNT);
    }
    return (g_evtpool != NULL) ? 0 : 1;
}

int evt_register(EVTYPE t, EVHANDLER h)
{
    if ((UINT)t >= (UINT)EV_MAX) {
        return 1;
    }
    g_handlers[t] = h;
    return 0;
}

int evt_post(EVTYPE t, void *p1, UINT u1)
{
    EVT *e;

    if ((UINT)t >= (UINT)EV_MAX) {
        return 1;
    }
    e = (EVT *)mm_alloc(g_evtpool);
    if (e == NULL) {                    /* exhaustion is normal: drop + count */
        g_drops++;
        return 1;
    }
    e->type  = (USHORT)t;
    e->flags = 0u;
    e->p1    = p1;
    e->u1    = u1;
    e->rsvd  = 0u;
    (void)q_enq(&g_evq, &e->q);         /* unbounded queue: always succeeds   */
    return 0;
}

void nsfevt_handoff_push(QELEM *e)
{
    xq_push(&g_xq, e);                  /* interrupt-safe (CS / atomic)       */
    nsfevt_plat_post(&g_handoffecb);    /* wake a WAITing loop                */
}

void nsfevt_stop(void)
{
    g_stop = 1;
    nsfevt_plat_post(&g_stopecb);
}

EVT *nsfevt_alloc(void)
{
    return (EVT *)mm_alloc(g_evtpool);  /* NULL when exhausted -- caller's job */
}

/* Drain the exit handoff stack (LIFO) and enqueue its elements onto the event
 * queue in FIFO order (spec 4.2 / 5.3: the executive reverses LIFO to FIFO). */
static void evt_drain_handoff(void)
{
    QELEM *chain = xq_drain(&g_xq);
    QELEM *prev  = NULL;
    QELEM *cur;

    while (chain != NULL) {             /* reverse the ->next chain           */
        QELEM *nx = chain->next;
        chain->next = prev;
        prev = chain;
        chain = nx;
    }
    for (cur = prev; cur != NULL; ) {   /* enqueue FIFO                       */
        QELEM *nx = cur->next;          /* capture before q_enq relinks cur   */
        (void)q_enq(&g_evq, cur);
        cur = nx;
    }
}

/* Dispatch a synthetic EV_TIMER_EXPIRED to its handler once per timer wake. Uses
 * a stack EVT -- timer ticks are frequent and must not churn the pool; the
 * delta-queue callbacks already ran inside nsftmr_run. */
static void evt_dispatch_timer(void)
{
    if (g_handlers[EV_TIMER_EXPIRED] != NULL) {
        EVT tev;

        tev.q.next = NULL;              /* synthetic event, not on any queue  */
        tev.q.prev = NULL;
        tev.type  = (USHORT)EV_TIMER_EXPIRED;
        tev.flags = 0u;
        tev.p1    = NULL;
        tev.u1    = 0u;
        tev.rsvd  = 0u;
        g_handlers[EV_TIMER_EXPIRED](&tev);
    }
}

/* Shutdown sequence skeleton (spec 5.4). Steps 1-3 (requests / sockets /
 * devices) and 5 (final stats) are stubs at M0-6; step 4 disarms the timer;
 * pending events are freed so the pool returns to baseline (the leak gate).
 * Step 6 (mm_shutdown) belongs to the owner of pool lifetime (the STC / the
 * test), run after the leak-gate check, so it is NOT done here. */
static void evt_shutdown(void)
{
    QELEM *qe;

    nsftmr_plat_disarm();               /* 4. stop the platform timer         */
    evt_drain_handoff();                /* fold any late handoff into the evq */
    while ((qe = q_deq(&g_evq)) != NULL) {
        mm_free(g_evtpool, Q_ENTRY(qe, EVT, q));
    }
}

void evt_mainloop(void)
{
    NSFECB *timerecb  = (NSFECB *)nsftmr_plat_ecb();
    NSFECB *ecblist[3];

    ecblist[0] = timerecb;
    ecblist[1] = &g_handoffecb;
    ecblist[2] = &g_stopecb;

    for (;;) {
        int    budget;
        QELEM *qe;

        /* 1. WAIT for a source, unless there is already pending work. */
        if (Q_EMPTY(&g_evq) && g_xq.head == NULL) {
            nsfevt_plat_wait(ecblist, 3);
        }

        /* 2. Drain the interrupt-safe handoff into the event queue. */
        g_handoffecb = 0u;              /* clear before draining (no lost push) */
        evt_drain_handoff();

        /* 3. Dispatch up to the drain budget, then re-loop (so a flood cannot
         *    starve the timer -- step 4 runs every iteration). */
        budget = EVT_DRAIN_BUDGET;
        while (budget-- > 0 && (qe = q_deq(&g_evq)) != NULL) {
            EVT *ev = Q_ENTRY(qe, EVT, q);
            if (ev->type < EV_MAX && g_handlers[ev->type] != NULL) {
                g_handlers[ev->type](ev);
            }
            mm_free(g_evtpool, ev);
        }

        /* 4. Run due timers on a timer wake, then signal the tick. */
        if (*timerecb & NSFECB_POSTED) {
            *timerecb = 0u;
            nsftmr_run(1u);
            g_ticks++;
            evt_dispatch_timer();
        }

        /* 5. Kick queued output -- a no-op stub until M1 (nsfdev_kick_output). */

        /* 6. Orderly stop? */
        if (g_stop != 0 || (g_stopecb & NSFECB_POSTED) != 0u) {
            break;
        }
    }

    evt_shutdown();
}

UINT nsfevt_inuse(void)
{
    MMSTATS s;

    mm_stats(g_evtpool, &s);
    return s.inuse;
}

#if NSF_DEBUG
UINT nsfevt_ticks(void) { return g_ticks; }
UINT nsfevt_drops(void) { return g_drops; }
#endif
