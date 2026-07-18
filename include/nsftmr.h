#ifndef NSFTMR_H
#define NSFTMR_H
/*
 * nsftmr.h -- the Timer Manager (spec ch. 06).
 *
 * One timer service for the whole stack (TCP rexmit/persist/keepalive/2MSL,
 * delayed ACK, later ARP/DNS). A sorted delta queue, not a timer wheel
 * (ADR-0010): the active-timer population is tiny (a handful per TCP
 * connection), so O(n) insert with n small costs nothing when idle and is
 * trivially correct.
 *
 * TMR structures are EMBEDDED in their owner control blocks (a TCB carries its
 * four timers inline), so arming can never fail and never allocates -- there is
 * no tmr pool. tmr_start / tmr_cancel just re-link the embedded TMR into / out
 * of the single sorted queue.
 *
 * TICK MODEL. The logical tick is 100 ms (spec 6.3, ADR-0011). On MVS a single
 * STIMER (SVC 47, real-time interval) drives it: nsftmr_run re-arms STIMER for
 * the head delta only, so an idle stack takes zero timer interrupts, and the
 * STIMER exit does nothing but POST the timer ECB -- all timer work happens on
 * the executive task. The platform arming lives behind the nsfstim.h seam
 * (asm/nsfstim.asm on MVS, src/nsfstim_host.c on the host). NOTE (correction to
 * the published spec/Brief): 3.8j provides STIMER, NOT STIMERM -- STIMERM is an
 * MVS/SP addition absent from the 3.8j macro library; a single STIMER re-armed
 * to the head delta reproduces the intended behaviour. See docs/adr/ADR-0011.
 *
 * DELTA QUEUE. Each armed timer stores `delta` = ticks after its PREDECESSOR
 * fires (or after "now" for the head). The absolute time of timer i is the sum
 * of deltas up to and including i. This makes advancing the clock O(1) at the
 * head and keeps insert/cancel local:
 *   - insert: walk accumulating deltas, splice in, shorten the successor's delta
 *     by the amount now owned by the new timer;
 *   - cancel: hand the cancelled timer's delta back to its successor (its
 *     absolute deadline is unchanged);
 *   - fire: the head's delta has genuinely elapsed, so it is simply consumed --
 *     the successor's delta already measures from the head's fire.
 *
 * DRIVING THE CLOCK. nsftmr_run(ticks) advances the queue by `ticks` elapsed
 * ticks. On MVS the executive does NOT call nsftmr_run directly on a timer wake;
 * it calls nsftmr_wake(), which advances by the exact tick count the fired
 * STIMER interval was armed for (the arming/consumption contract, ADR-0034).
 * This closes issue #40: the old loop consumed nsftmr_run(1u) per wake while
 * nsftmr_run re-armed for the whole head delta, so a delta-N timer fired after
 * N(N+1)/2 ticks. On the host there is no STIMER, so tests inject elapsed ticks
 * directly via nsftmr_run (spec 6.3 refined at M0-5: the published
 * `nsftmr_run(void)` gains the elapsed-ticks argument the host needs).
 *
 * OWNERSHIP / LIFETIME. A TMR lives exactly as long as its owner CB. tmr_cancel
 * is idempotent and mandatory in every owner-teardown path (spec 6.4): a stale
 * armed timer firing into a freed CB is the use-after-free this guards against.
 * TMR_IDLE is 0, so a memset-cleared owner CB carries a safe-to-cancel timer.
 */

#include "nsf.h"
#include "nsfque.h"             /* QELEM (delta-queue linkage embedded in TMR) */

/* Timer callback: run to completion on the executive task, like every event
 * handler -- never WAIT, never loop unboundedly. It MAY re-arm itself or arm
 * other timers (tmr_start) and cancel timers (tmr_cancel); nsftmr_run detaches
 * a timer and marks it IDLE BEFORE calling its fn, so a callback may safely
 * re-arm the very timer that is firing. */
typedef void (*TMRFN)(void *arg);

/* Timer state (stored in TMR.state). IDLE must be 0 (see the header note). */
#define TMR_IDLE     0          /* not on the queue                            */
#define TMR_PENDING  1          /* armed: linked in the sorted delta queue     */

/* Timer control block (spec 6.3). 24 bytes on the S/370 target; EMBEDDED in the
 * owner CB, never pool-allocated. */
typedef struct tmr {
    QELEM   q;            /* 8  delta-queue linkage                            */
    UINT    delta;        /* 4  ticks after the predecessor (or now, at head)  */
    TMRFN   fn;           /* 4  callback                                       */
    void   *arg;          /* 4  callback argument, e.g. TCB*                   */
    UCHAR   state;        /* 1  TMR_IDLE / TMR_PENDING                         */
    UCHAR   rsvd[3];      /* 3  pad to a fullword boundary                     */
} TMR;                    /* 24 bytes */
NSF_SIZE_ASSERT(TMR, 24);

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module nsftmr/tmr_* pins a unique 8-char linker name so cc370's 8-char
 * external truncation (upcased, '_' -> '@') can never fold two into one on MVS.
 * Scheme NSFTM + verb:
 *   nsftmr_init NSFTMINI   tmr_start NSFTMSTR   tmr_cancel NSFTMCAN
 *   nsftmr_run NSFTMRUN    nsftmr_wake NSFTMWAK   nsftmr_count NSFTMCNT
 *   nsftmr_peek NSFTMPEK
 */

/* Reset the timer service: empty the queue and disarm the platform timer. Safe
 * to call before any timer is armed and safe to re-call (the tests reset
 * between scenarios). The M0-8 executive startup calls this once. */
void nsftmr_init(void) asm("NSFTMINI");

/* Arm `t` to fire after `ticks` logical ticks (100 ms each). If `t` is already
 * PENDING it is restarted (cancelled, then re-inserted at the new deadline).
 * `ticks` is clamped to a minimum of 1: a timer never fires within the run that
 * arms it, so the soonest deadline is the next tick. Cannot fail and never
 * allocates -- the TMR storage is the caller's. The caller keeps ownership of
 * the TMR; NSFTMR only links it. */
void tmr_start(TMR *t, UINT ticks, TMRFN fn, void *arg) asm("NSFTMSTR");

/* Disarm `t`. Idempotent and valid in ANY state (IDLE or PENDING): cancelling
 * an already-idle timer is a no-op. On removing a PENDING timer, its delta is
 * handed to the successor so the rest of the queue keeps its absolute
 * deadlines. Mandatory in every owner-teardown path (spec 6.4). */
void tmr_cancel(TMR *t) asm("NSFTMCAN");

/* Advance the queue by `ticks` elapsed ticks: fire every timer whose deadline
 * falls within the window, in order (soonest first), then re-arm the platform
 * timer for the new head delta -- or disarm it if the queue drained. Does
 * nothing when the queue is empty on entry. Each fired timer is detached and
 * marked IDLE before its callback runs (callbacks may re-arm/cancel safely).
 * Tests call it directly with an explicit elapsed count; the executive main loop
 * (NSFEVT) drives it through nsftmr_wake, never nsftmr_run(1u) (ADR-0034). */
void nsftmr_run(UINT ticks) asm("NSFTMRUN");

/* Service a timer wake from the executive main loop (NSFEVT). Advances the queue
 * by the exact tick count the fired STIMER interval was armed for and returns
 * that count (0 on a spurious wake with nothing armed). This is the arming/
 * consumption contract that fixes issue #40 -- the loop must call THIS on a
 * timer-ECB post, never nsftmr_run(1u), or a delta-N timer fires after
 * N(N+1)/2 ticks. Host tests drive nsftmr_run directly (no STIMER), so this is
 * exercised through the loop path (tstevt) and the on-MVS cadence gates. */
UINT nsftmr_wake(void) asm("NSFTMWAK");

/* Inspection seam (always available, like nsftrc_count/peek; reused by the M0-8
 * operator DISPLAY and by the host tests). nsftmr_count is the number of armed
 * timers; nsftmr_peek returns the i-th timer in fire order (i == 0 is the head /
 * soonest, NULL once i >= count), so a caller can walk both queue order and each
 * timer's delta after an insert or cancel. */
UINT       nsftmr_count(void) asm("NSFTMCNT");
const TMR *nsftmr_peek(UINT i) asm("NSFTMPEK");

#endif /* NSFTMR_H */
