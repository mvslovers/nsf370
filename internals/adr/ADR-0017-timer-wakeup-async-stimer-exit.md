# ADR-0017 — Timer wakeup via the async STIMER REAL exit (not a timer subtask)

**Status:** Accepted (2026-07-08); validated on MVS 3.8j at M0-6.
**Relates to:** §5 (NSFEVT main loop), §6 / ADR-0011 (NSFTMR / STIMER),
CLAUDE.md §3 ("C-callable HLASM" — and the OS-invoked-exit exception).

## Context

The executive main loop (NSFEVT, §5.3) blocks on an ECB list and must be woken
every ~100 ms so it can run due timers (NSFTMR). Something has to turn the STIMER
interval into a POST of the loop's timer ECB. Two shapes were on the table:

- **A. Async STIMER REAL exit.** Arm `STIMER REAL` with an exit routine; the exit
  runs asynchronously on the executive's own task when the interval expires, does
  the minimum (POST the timer ECB, re-arm STIMER), and returns.
- **B. A dedicated timer subtask.** ATTACH a second TCB that loops on
  `STIMER WAIT` (or `ecb_timed_wait`) and cross-memory / cross-task POSTs the
  executive's timer ECB each interval.

## Decision

**Option A — the async STIMER REAL exit.** The exit is `NSFTMEXP` in
`asm/nsfstim.asm`; it POSTs the timer ECB and re-arms `STIMER REAL` (a one-shot,
so re-arming from within the exit gives the periodic heartbeat), then returns.

Rationale:
- **Architecturally cleaner / fewer moving parts.** One task, one address space,
  no ATTACH, no second save-area/ESTAE environment to own, no inter-task POST
  serialization to reason about. The loop's only concurrency stays the CS-based
  exit→mainline handoff (NSFXQ) plus the ECB POST — exactly the model §0/§5
  already commit to.
- **It pays forward to M1.** Device I/O completion exits (the CTCI driver, M1)
  are the *same asynchronous-exit class*: they run on an interrupt, do the
  minimum (`xq_push` a pre-allocated element + POST), and return. Getting the
  async-exit entry convention right now — the hard part — is reused verbatim by
  every device exit later. A timer subtask would solve the timer in isolation
  and leave the device-exit convention still to be discovered.
- **No measured downside.** The 100 ms tick is accurate through the exit: M0-6's
  on-MVS run observed 10/10 ticks at a **100.2 ms** mean; the raw timebase is
  frozen at ADR-0011 (100.1/100.2 ms, jitter 0).

Rejected — the timer subtask (B): a second TCB, cross-task POST, and its own
recovery environment for a job the async exit does in a few instructions; and it
does not advance the M1 device-exit convention.

## The exit entry-convention contract (do not re-derive — it cost an S0C6)

An async STIMER exit is **OS-invoked, not called from C**, so it does NOT get the
standard cc370 `FUNHEAD` prologue (CLAUDE.md §3). It MUST instead follow the
documented MVS 3.8 STIMER-exit linkage (OS/VS2 Supervisor Services & Macros,
GC28-0683). `NSFTMEXP` implements exactly this, and a hand-rolled shortcut here
(self-`BALR` base, no own save area) is what produced the earlier ABEND S0C6:

1. **On entry:** `R15` = exit entry address, `R14` = return, `R13` = a save area
   the timer SLIH provides.
2. `STM 14,12,12(13)` — save into the provided save area.
3. `LR 12,15` ; `USING NSFTMEXP,12` — base via **R12**, not R13 or a self-`BALR`,
   because the exit issues system macros (`POST`, and `STIMER` to re-arm) and
   R15 is not a safe base once a macro is used.
4. **Chain its OWN static save area** (`ST 13,4(,ownSA)` ; `ST ownSA,8(,13)` ;
   `LR 13,ownSA`) — it issues SVCs, so it must not hand them the SLIH's area.
5. **Address the timer ECB by explicit displacement from the entry** (R12/R15
   relative; the ECB sits right after the exit code), never a bare-label `USING`
   (as370 drops those to base 0 — the S102 class; the ECB must never be base 0).
   `POST` it (minimal work only).
6. **Re-arm** `STIMER REAL` for the next interval (`STIMER REAL` is one-shot;
   without re-arming you get exactly one tick).
7. Unchain (`L 13,4(,13)`), restore (`LM 14,12,12(13)`), `BR 14`.

## Consequences

- `asm/nsfstim.asm` `NSFTMEXP` is now **runtime-validated** on 3.8j (M0-6
  `test/mvs/tstevtm.c` = CC 0, 10 ticks, clean shutdown, EVT pool at baseline).
  It graduates from the "deferred seam" status carried since M0-5.
- The three C-callable arming entries (`NSFTMARM` / `NSFTMDIS` / `NSFTMECB`)
  stay on `FUNHEAD` (they *are* C callees); only the exit is hand-rolled, per the
  §3 OS-invoked-exit exception.
- M1 device I/O exits follow this same contract; the NSFXQ handoff they feed is
  already host-validated (M0-6 `test/tstevt.c`, pthread-simulated exit).
