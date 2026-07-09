#ifndef NSFEVT_H
#define NSFEVT_H
/*
 * nsfevt.h -- the Event Dispatcher / executive main loop (spec ch. 05).
 *
 * One executive task runs a single event loop, run-to-completion, no
 * preemption inside the stack (goal 3). Async MVS exits (device I/O, the timer)
 * do the minimum -- enqueue a pre-allocated element onto the interrupt-safe
 * handoff stack (NSFXQ) and POST an ECB -- and the loop does everything real:
 * it WAITs on the ECB list, drains the handoff into the event queue, dispatches
 * each event to its handler, runs due timers, then kicks queued output (§5.3).
 *
 * M0-6 scope: the loop core + the NSFXQ handoff (host-validated) + the timer
 * wakeup validated on MVS via the async STIMER exit (asm/nsfstim.asm). The WAIT
 * list is effectively {timerECB, stopECB}; devECB[]/requestECB/cibECB are slots
 * wired for M1+/M0-8. NSFCFG (M0-7), the STC skeleton / MODIFY / ESTAE / WTO
 * (M0-8) and devices/sockets (M1+) are out of scope here.
 */

#include "nsf.h"
#include "nsfque.h"             /* QELEM (queue linkage embedded in EVT) */
#include "nsfevtp.h"            /* NSFECB (the operator console ECB, M0-8) */

/* Event kinds (spec 5.2). EV_TIMER_EXPIRED is dispatched once per timer wake so
 * a handler can observe the tick; the delta-queue callbacks (NSFTMR) fire
 * independently inside nsftmr_run. The device / socket / operator kinds are
 * wired for M1+/M0-8. */
typedef enum {
    EV_PACKET_RECEIVED = 0,     /* device bottom half            */
    EV_PACKET_SENT,             /* device bottom half            */
    EV_TIMER_EXPIRED,           /* timer manager                 */
    EV_SOCKET_REQUEST,          /* request manager               */
    EV_OPERATOR_CMD,            /* MODIFY / STOP via CIB          */
    EV_SHUTDOWN,                /* orderly stop                  */
    EV_MAX
} EVTYPE;

/* Event control block (spec 5.2). 24 bytes on the S/370 target; carved from the
 * NSFMM EVT pool by evt_post / the async exit, freed by the loop after
 * dispatch. */
typedef struct evt {
    QELEM   q;                  /* 8  event-queue / handoff linkage           */
    USHORT  type;               /* 2  EVTYPE                                   */
    USHORT  flags;              /* 2  reserved                                */
    void   *p1;                 /* 4  e.g. PBUF*, NSFRQE*                      */
    UINT    u1;                 /* 4  e.g. device index                       */
    UINT    rsvd;               /* 4                                          */
} EVT;                          /* 24 bytes */
NSF_SIZE_ASSERT(EVT, 24);

/* Event handler: run to completion on the executive task (§5.3). It may
 * evt_post further events (processed in the same drain under the drain budget)
 * and arm/cancel timers, but must never WAIT, never loop over unbounded input,
 * and never call mm_pool_create. */
typedef void (*EVHANDLER)(EVT *ev);

/* Drain budget (§5.3): the loop dispatches at most this many events before it
 * runs timers and re-WAITs, so an evt_post flood cannot starve the timer. */
#define EVT_DRAIN_BUDGET  64

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module nsfevt/evt_* pins a unique 8-char linker name (scheme NSFEV*):
 *   nsfevt_init NSFEVINI   evt_register NSFEVREG   evt_post NSFEVPST
 *   evt_mainloop NSFEVMLP  nsfevt_stop NSFEVSTP    nsfevt_handoff_push NSFEVHP
 *   nsfevt_alloc NSFEVAL   nsfevt_ticks NSFEVTK   nsfevt_drops NSFEVDRP
 *   nsfevt_inuse NSFEVIU   evt_set_operator NSFEVOPR
 *   evt_set_devices NSFEVDEV   nsfevt_wake NSFEVWK
 */

/* Create the EVT pool and reset the loop state (event queue, handoff stack,
 * stop ECB). Init-window only -- calls mm_pool_create -- so it runs between
 * mm_init and mm_init_complete. Returns 0 on success, non-zero if the pool
 * could not be created (the executive refuses to start). */
int  nsfevt_init(void) asm("NSFEVINI");

/* Register the handler for event type t. Init-time only (before evt_mainloop).
 * Returns 0 on success, non-zero if t is out of range. */
int  evt_register(EVTYPE t, EVHANDLER h) asm("NSFEVREG");

/* Post an event (executive-task use): allocate an EVT from the pool, fill it,
 * enqueue it. Returns 0 on success, non-zero if the pool is exhausted (the
 * event is dropped and counted -- exhaustion is normal, never an ABEND). */
int  evt_post(EVTYPE t, void *p1, UINT u1) asm("NSFEVPST");

/* Run the main loop (§5.3): WAIT, drain the handoff, dispatch under the budget,
 * run timers, kick output. Returns only after a stop is requested and the
 * shutdown sequence (§5.4) has run. */
void evt_mainloop(void) asm("NSFEVMLP");

/* Request an orderly stop from anywhere (operator, a timer callback, a test):
 * set the stop flag and post the stop ECB so a WAITing loop wakes. */
void nsfevt_stop(void) asm("NSFEVSTP");

/* Register the operator interface (M0-8): `ecb` is the console ECB the loop adds
 * to its ECBLIST (the §5.3 cibECB slot), and `drain` is called every loop pass
 * to consume queued operator commands. `drain` runs UNCONDITIONALLY (not gated
 * on the ECB bit): a startup CIB can be queued without a POST, and gating would
 * hold the CIB slot and reject later MODIFYs (IEE342I TASK BUSY). Pass
 * (NULL, NULL) for no operator (the default; the four foundation tests). */
void evt_set_operator(NSFECB *ecb, void (*drain)(void)) asm("NSFEVOPR");

/* Register the device seam (M1-2): NSFDEV wires its three loop hooks so the
 * loop begins servicing devices, without NSFEVT ever naming NSFDEV (the same
 * decoupling evt_set_operator gives the operator seam). `collect_ecbs` appends
 * the device ECB pointers to the loop's ECBLIST at loop entry (returns the
 * count, bounded by the max passed); `poll_input` drains device doneqs up to
 * EV_PACKET_RECEIVED once per pass before dispatch; `kick_output` starts pending
 * output at §5.3 step 5. Pass (NULL, NULL, NULL) for no devices (the default;
 * the foundation tests and the M0-8 STC with no interfaces). */
void evt_set_devices(int  (*collect_ecbs)(NSFECB **list, int max),
                     void (*poll_input)(void),
                     void (*kick_output)(void)) asm("NSFEVDEV");

/* Wake a WAITing loop so it makes one more pass (running the device output kick
 * and operator/timer steps). Used when executive work is queued from outside a
 * loop pass -- e.g. dev_send from a test or, later, the request path -- so the
 * loop does not stay blocked with pending output. Idempotent; a spurious wake
 * just costs one extra pass. */
void nsfevt_wake(void) asm("NSFEVWK");

/* Exit-side handoff (§4.2): interrupt-safely hand a pre-allocated EVT's QELEM to
 * the loop -- xq_push then post the handoff ECB so a WAITing loop wakes. This is
 * the seam an async device I/O exit uses at M1; at M0-6 the host test drives it
 * from a pthread-simulated exit. The pushed EVT is pool-allocated ahead of time
 * by the producer; the loop dispatches and frees it (single owner, §3). */
void nsfevt_handoff_push(QELEM *e) asm("NSFEVHP");

/* Allocate an EVT from the pool for a producer to fill and hand off (embed in a
 * device CB at M1, or pre-allocate on the main thread before a host-simulated
 * exit). Returns NULL when the pool is exhausted. The loop frees it after
 * dispatch (single owner, §3). */
EVT *nsfevt_alloc(void) asm("NSFEVAL");

/* EVT pool objects currently in use -- the leak gate reads this back to zero
 * after evt_mainloop returns (every event dispatched or freed at shutdown).
 * Always available (like nsftmr_count), so a test can gate on it in either
 * build. */
UINT nsfevt_inuse(void) asm("NSFEVIU");

#if NSF_DEBUG
/* Number of timer wakes the loop has processed (introspection; the M0-8 operator
 * DISPLAY reuses it). */
UINT nsfevt_ticks(void) asm("NSFEVTK");
/* Number of events dropped by evt_post because the EVT pool was exhausted. */
UINT nsfevt_drops(void) asm("NSFEVDRP");
#endif

#endif /* NSFEVT_H */
