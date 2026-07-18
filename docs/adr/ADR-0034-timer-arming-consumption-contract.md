# ADR-0034 — The timer arming/consumption contract (fixes the executive tick-advance, issue #40)

**Status:** Accepted (2026-07-17). Pins the contract between NSFTMR (the sorted
delta queue) and NSFEVT (the executive main loop) for arming and consuming the
single MVS `STIMER`. Before this, that contract was undefined at three points;
one of the holes is issue #40 — a delta-N timer fired after **N(N+1)/2** ticks
instead of N. **Foundation change:** it touches `nsftmr`/`nsfevt`/`nsfmain`,
which every component sits on. No TCP/driver/API behaviour changes.
**Relates to:** spec §6 (NSFTMR: 100 ms tick, embedded TMRs, arming cannot fail,
`tmr_cancel` idempotent, "an idle stack takes zero timer interrupts"), §5.3 (the
loop), ADR-0011 (STIMER not STIMERM; the 100 ms accuracy gate), ADR-0017 (the
async STIMER exit that self-re-arms), ADR-0033/ADR-0031 (the TCP timers whose
cadence #40 distorted: rexmit/persist backoff, 2MSL).

## Context

The proven defect (issue #40, found live on MVSCE by the M4-4 persist trace):
`evt_mainloop` consumed `nsftmr_run(1u)` — ONE tick — per STIMER wake, while
`nsftmr_run` re-armed the STIMER for the whole head delta and the async exit
(`asm/nsfstim.asm` `NSFTMEXP`) self-re-arms for that same interval. The two
disagree: the STIMER fires after N ticks, the loop consumes 1 and re-arms for
N−1, fires after N−1, consumes 1 … so a delta-N timer fires after
N + (N−1) + … + 1 = **N(N+1)/2** ticks. Live evidence: a zero-window persist
probe of delta 20 fired at exactly 21.0 s (20·21/2 = 210 ticks), and in
production the TIME_WAIT 2MSL (600 ticks) would fire after ~5 hours instead of
60 s. NSFTCP's own logic is correct — the host suite drives `nsftmr_run` with
explicit tick counts and passes; the bug is purely the platform tick-advance.

The advance bug is one hole in an **undefined arming contract**. Three
responsibilities were never pinned:

1. **Wake accounting** — how many ticks the queue advances on a timer wake.
2. **Empty→nonempty bootstrap** — who arms the STIMER when `tmr_start` inserts
   into an empty (disarmed) queue. The old code never armed here; the
   `nsfmain.c` `nsftmr_plat_arm(1u)` heartbeat papered over it (its self-re-arming
   1-tick interval kept the STIMER firing so `nsftmr_run(1u)` eventually consumed).
3. **Head-shortening / drain** — what happens when `tmr_start` inserts a deadline
   nearer than the armed one, and who disarms when the queue empties.

## Decision

Route **every** touch of the `nsfstim.h` seam through two private helpers in
`nsftmr.c` (`tmr_arm`/`tmr_disarm`) that keep one module word, `g_armed`, equal
to what the STIMER is physically armed for. Because the seam clears-and-re-arms
on every arm, `g_armed` and the hardware **cannot diverge**; the STIMER fires
exactly `g_armed` ticks after it is armed. That single fact makes the whole
contract correct and early-fire structurally impossible.

### 1. Wake accounting — `nsftmr_wake()`

The executive calls **`nsftmr_wake()`** on a timer-ECB post, never
`nsftmr_run(1u)`:

```c
UINT nsftmr_wake(void) { UINT n = g_armed; nsftmr_run(n); return n; }
```

It advances the queue by exactly the tick count the fired STIMER interval was
armed for and returns it (0 on a spurious wake with nothing armed — a no-op the
loop counts). `nsftmr_run` is **semantically unchanged**: it still takes an
explicit elapsed-tick count and re-arms the new head. Only the loop's call site
changes (`nsftmr_run(1u)` → `nsftmr_wake()`), which is what keeps the TCP host
tests — they call `nsftmr_run` with explicit ticks — passing verbatim.

### 2. Bootstrap + head-shortening — `tmr_start` arms when the inserted timer becomes the head

After the splice, `if (queue.head.next == &t->q) tmr_arm(t->delta);`. This one
condition covers both the empty→nonempty bootstrap (the new timer is the sole
head) and head-shortening (a nearer deadline than the old head). A tail insert
leaves the head — and `g_armed` — untouched.

**No TTIMER-residual seam change.** Re-arming from "now" for the new head is
always **safe-late** for the timers already queued: they lose the elapsed
sub-interval of the interrupted arm and therefore fire *late*, never early. The
audit (below) shows that lateness is immaterial for v1, so option 3-re-arm is
taken *without* reading the TTIMER CANCEL residual — no `nsfstim` asm change, no
Stage-0 probe. The alternative (accept-and-document full-interval lateness with
NO re-arm) is rejected: it would delay a rexmit/persist by up to the head it sits
under (e.g. a 2MSL of 600 ticks = 60 s), defeating the timer.

### 3. Drain — INVARIANT: queue empty ⟺ STIMER disarmed ⟺ `g_armed == 0`

Enforced at **both** drain points: `nsftmr_run` when a fire empties the queue
(already did) **and** `tmr_cancel` when a cancel empties it (the gap — the fix).
This is required because the async STIMER exit self-re-arms *unconditionally*: a
queue emptied without a disarm would keep taking timer interrupts forever,
breaking spec 6.3's "an idle stack takes zero timer interrupts." A cancel that
leaves the queue non-empty does **not** re-arm — the successor's deadline is ≥
the cancelled timer's, so the armed interval fires no later than needed and the
next wake reconciles (never early).

### The audit that justifies 3-re-arm-without-residual (evidence, not assertion)

The only timers that exist in v1 (`grep tmr_start`):

| Timer | Delta (ticks) | Character | Who inserts |
|---|---|---|---|
| `t_rexmit` | `rto` = 10 … 640 | urgent, short | data/SYN/FIN in flight |
| `t_persist` | `rto` = 10 … 640 | urgent, short | zero send window |
| `t_2msl` | 600 | long, cleanup | TIME_WAIT / FIN_WAIT_2 |

Head-shortening happens when a rexmit/persist (10) is armed while a 2MSL (600) is
the head. With 3-re-arm the **shortened** timer — the urgent one — fires on time;
only the pre-existing **2MSL** loses the forgotten sub-interval and fires up to
~60 s late, which is immaterial for a TIME_WAIT cleanup timer (pool-pressure
reclaim already handles the common case, ADR-0031). Two 2MSLs never shorten each
other (the later one's deadline is ≥ the earlier one's remaining). rexmit on a
lossless CTCI link is cancelled by the ACK before it fires; persist fires on a
genuine zero window and now does so on time. So no v1 workload suffers material
lateness — the residual-reading machinery buys nothing and is not built.

### Heartbeat (`nsfmain.c` `nsftmr_plat_arm(1u)`) — KEPT

Under the fixed contract the timer path is self-sufficient (responsibility 2), so
the heartbeat's *bootstrap* role is gone. Its only remaining role is
operator-liveness while the timer queue is **empty**: a periodic wake so an
operator `MODIFY` arriving at an idle stack is serviced. It is a direct platform
arm, deliberately outside `g_armed`, and it is harmless: it only operates when
the queue is empty (`g_armed == 0`), where `nsftmr_wake` advances 0; a real
timer's arm replaces the 1-tick interval with its own delta.

It is **kept, not removed.** Removing it would restore true zero-idle-interrupts,
but whether the console CIB ECB alone wakes the WAIT on an idle `MODIFY` is an
empirical question. The existing `test/mvs/tstevtm.c` only counts heartbeats over
an empty queue — it never issues an idle `MODIFY`, so it cannot prove redundancy.
Removing on reasoning alone is forbidden (the kickoff's rule, and the sound one —
this project has a history of misdiagnosing the tick-advance bug as a liveness
bug, e.g. ADR-0023 §6 / issue #21). The gate for removal is a proper idle-MODIFY
liveness test (future work); until then the prior behaviour (10 idle interrupts/s,
ADR-0011's zero-idle traded for liveness) stands, now correctly grounded rather
than accidental.

## Consequences

- **Correct cadence.** A delta-N timer fires after N ticks. Host cadence vectors
  (`test/tsttmr.c` §§6–13, `test/tstevt.c` §5) assert the `nsftmr_wake` return
  sequence directly (a delta-20 fires in one 20-tick wake, a re-arming delta-20
  fires 10/20/40/80…, the #40 shape). The live gate `test/mvs/tsttmcad.c` times a
  delta-20 through the whole loop path at ~2.0/4.0/6.0 s (was 21 s).
- **`nsftmr_run` and the delta-queue local rules are unchanged**, so the M4-4 TCP
  tests (which drive `nsftmr_run` with explicit ticks) pass verbatim — the
  red-line that this is *not* a TCP change.
- **New external:** `nsftmr_wake` (`NSFTMWAK`); NSF_DEBUG probe `nsfevt_tickadv`
  (`NSFEVTKA`). The `tmr_arm`/`tmr_disarm` helpers are `static` (inlined). No
  other timer-API surface change.
- **The ADR-0033/spec-v1.32 "live cadence follows #40" caveat is resolved here.**
  The M4-4 persist/RTO backoff now runs at RFC-plausible intervals live; the
  production 2MSL is 60 s, not ~5 hours.
