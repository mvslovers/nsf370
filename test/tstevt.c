/*
 * tstevt.c -- NSFEVT host unit tests (spec ch. 05).
 *
 * Exercises the portable loop logic natively (make test-host, NSF_DEBUG=1):
 *   1. events dispatch in FIFO order;
 *   2. the drain budget prevents timer starvation -- with a flood of events the
 *      loop still services the timer after exactly EVT_DRAIN_BUDGET events;
 *   3. a PTHREAD-simulated async exit hands QELEMs to the loop through the NSFXQ
 *      handoff and the loop drains + dispatches them (the xq's real MVS producer
 *      is the M1 device I/O exit; host-simulated here);
 *   4. the shutdown sequence frees pending events (the leak gate: the EVT pool
 *      returns to baseline in-use after evt_mainloop returns).
 *
 * The WAIT is the pthread cond-var host shim (src/nsfevt_plat_host.c); the timer
 * ECB is nsftmr_plat_ecb() (src/nsfstim_host.c on the host). A registered
 * handler ends each scenario by calling nsfevt_stop().
 */
#include "nsfevt.h"
#include "nsfevtp.h"            /* NSFECB, nsfevt_plat_post (drive the timer) */
#include "nsftmr.h"
#include "nsfstim.h"            /* nsftmr_plat_ecb */
#include "nsfmm.h"
#include <mbtcheck.h>
#include <string.h>            /* memset (scenario 5 stack TMR) */
#ifndef __MVS__
#include <pthread.h>            /* host-only: the pthread-simulated exit */
#endif

/* ---- scenario 1: dispatch order ---- */
static int g_order[8];
static int g_order_n;
static void h_order(EVT *ev)
{
    if (g_order_n < 8) {
        g_order[g_order_n] = (int)ev->u1;
    }
    g_order_n++;
    if (g_order_n >= 4) {
        nsfevt_stop();
    }
}

/* ---- scenario 2: drain budget ---- */
static UINT g_ev2, g_tick2, g_at_tick2;
static void h_ev2(EVT *ev)
{
    (void)ev;
    g_ev2++;
    if (g_ev2 >= 100u) {
        nsfevt_stop();
    }
}
static void h_tick2(EVT *ev)
{
    (void)ev;
    if (g_tick2 == 0u) {
        g_at_tick2 = g_ev2;         /* how many events had run at the tick */
    }
    g_tick2++;
}

/* ---- scenario 3: pthread-simulated exit (host only) ---- */
#ifndef __MVS__
static UINT g_ev3;
static EVT *g_pushevt[5];
static void h_ev3(EVT *ev)
{
    (void)ev;
    g_ev3++;
    if (g_ev3 >= 5u) {
        nsfevt_stop();
    }
}
static void *exit_thread(void *arg)   /* the "async exit": only xq_push + post */
{
    int i;

    (void)arg;
    for (i = 0; i < 5; i++) {
        nsfevt_handoff_push(&g_pushevt[i]->q);
    }
    return NULL;
}
#endif

/* ---- scenario 4: shutdown frees pending ---- */
static UINT g_ev4;
static void h_ev4(EVT *ev)
{
    (void)ev;
    g_ev4++;
    if (g_ev4 >= 3u) {
        nsfevt_stop();
    }
}

/* ---- scenario 5: a timer wake advances the ARMED amount (#40) ---- *
 * The timer callback stops the loop so exactly one wake is serviced. */
#if NSF_DEBUG
static void tmr_stop_fn(void *arg)
{
    (void)arg;
    nsfevt_stop();
}
#endif

int main(void)
{
    int i;

    printf("=== nsf370 NSFEVT tests ===\n");

    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init creates the EVT pool");
    mm_init_complete();

    /* ---- 1. FIFO dispatch order ---- */
    nsfevt_init();
    evt_register(EV_PACKET_RECEIVED, h_order);
    g_order_n = 0;
    evt_post(EV_PACKET_RECEIVED, NULL, 1u);
    evt_post(EV_PACKET_RECEIVED, NULL, 2u);
    evt_post(EV_PACKET_RECEIVED, NULL, 3u);
    evt_post(EV_PACKET_RECEIVED, NULL, 4u);
    evt_mainloop();
    CHECK_EQ((long)g_order_n, 4, "all four events dispatched");
    CHECK(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3
          && g_order[3] == 4, "events dispatch in FIFO order");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after scenario 1");

    /* ---- 2. drain budget prevents timer starvation ---- */
    nsfevt_init();
    evt_register(EV_PACKET_RECEIVED, h_ev2);
    evt_register(EV_TIMER_EXPIRED, h_tick2);
    g_ev2 = 0u; g_tick2 = 0u; g_at_tick2 = 0u;
    for (i = 0; i < 100; i++) {
        evt_post(EV_PACKET_RECEIVED, NULL, (UINT)i);
    }
    nsfevt_plat_post((NSFECB *)nsftmr_plat_ecb());   /* simulate one timer tick */
    evt_mainloop();
    CHECK_EQ((long)g_ev2, 100, "all 100 flooded events dispatched");
    CHECK(g_tick2 >= 1u, "timer fired despite the event flood");
    CHECK_EQ((long)g_at_tick2, EVT_DRAIN_BUDGET,
             "timer serviced after exactly the drain budget (not starved)");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after scenario 2");

    /* ---- 3. pthread-simulated exit -> NSFXQ handoff (host only) ---- */
#ifndef __MVS__
    nsfevt_init();
    evt_register(EV_PACKET_RECEIVED, h_ev3);
    g_ev3 = 0u;
    for (i = 0; i < 5; i++) {          /* pre-allocate on THIS thread (no race) */
        g_pushevt[i] = nsfevt_alloc();
        CHECK(g_pushevt[i] != NULL, "pre-allocated a handoff EVT");
        g_pushevt[i]->type  = (USHORT)EV_PACKET_RECEIVED;
        g_pushevt[i]->flags = 0u;
        g_pushevt[i]->p1    = NULL;
        g_pushevt[i]->u1    = (UINT)i;
        g_pushevt[i]->rsvd  = 0u;
    }
    {
        pthread_t th;
        pthread_create(&th, NULL, exit_thread, NULL);
        evt_mainloop();
        pthread_join(th, NULL);
    }
    CHECK_EQ((long)g_ev3, 5, "loop drained + dispatched the 5 handed-off events");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after scenario 3");
#endif

    /* ---- 4. shutdown frees pending events (leak gate) ---- */
    nsfevt_init();
    evt_register(EV_PACKET_RECEIVED, h_ev4);
    g_ev4 = 0u;
    for (i = 0; i < 100; i++) {        /* > budget, so the stop leaves pending */
        evt_post(EV_PACKET_RECEIVED, NULL, (UINT)i);
    }
    evt_mainloop();
    CHECK(g_ev4 >= 3u, "handler requested stop");
    CHECK_EQ((long)nsfevt_inuse(), 0,
             "shutdown freed every pending event (leak gate)");

    /* ---- 5. the loop advances the queue by the ARMED tick count (#40) ---- *
     * Arm a delta-8 timer, then post the timer ECB once (as the STIMER exit
     * would after 8 ticks). The loop's step 4 must call nsftmr_wake(), which
     * advances 8 and fires the timer -- NOT nsftmr_run(1u), which would advance
     * 1 and leave the timer pending. nsfevt_tickadv() reads back the ticks the
     * loop advanced. (NSF_DEBUG-gated: nsfevt_tickadv is the debug probe.) */
#if NSF_DEBUG
    {
        TMR t;
        nsfevt_init();
        nsftmr_init();
        memset(&t, 0, sizeof(t));
        tmr_start(&t, 8u, tmr_stop_fn, NULL);   /* g_armed = 8 */
        nsfevt_plat_post((NSFECB *)nsftmr_plat_ecb());  /* one STIMER post */
        evt_mainloop();
        CHECK_EQ((long)nsfevt_tickadv(), 8,
                 "one timer wake advanced the armed 8 ticks (not 1)");
        CHECK_EQ((long)nsfevt_ticks(), 1, "exactly one timer wake serviced");
        CHECK_EQ((long)nsftmr_count(), 0, "the delta-8 timer fired in that wake");
    }
#endif

    mm_shutdown();
    return mbt_test_summary("TSTEVT");
}
