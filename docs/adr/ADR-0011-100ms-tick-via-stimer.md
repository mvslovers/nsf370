# ADR-0011 — 100 ms timer tick via a single re-armed STIMER

**Status:** Accepted (2026-07-07); split out and corrected at M0-5.
**Supersedes:** the `STIMERM` phrasing in Architecture Specification §6 / §18,
ADR-0016 (consequences), and Project Brief v2 (§ timer services).

## Context

NSFTMR needs one real-time interval source to drive its sorted delta queue
(ADR-0010). The tick granularity must be fine enough for TCP's timers — a
200 ms delayed ACK and an RTO floor of 1 s (RFC-conservative) — without taking
an interrupt when the stack is idle.

The spec, ADR-0016 and the Brief all named **`STIMERM`** (the multiple-interval
timer) as the driver. Building NSFTMR at M0-5 established, from primary sources,
that this is wrong for the target:

- **`STIMERM` does not exist on MVS 3.8j / TK4-.** It is an MVS/SP addition. The
  3.8j `SYS1.MACLIB` (the extract the cc370 / as370 toolchain assembles against,
  and the copy in `libc370/sysmac/`) contains `stimer.macro`, `ttimer.macro`,
  `wait.macro`, `post.macro` — but **no** `stimerm.macro`. A `STIMERM` would not
  assemble under IFOX00 / as370.
- The single-interval **`STIMER`** (SVC 47) plus **`TTIMER CANCEL`** (SVC 46) is
  what ships, and it is what the ecosystem already uses for real-time waits
  (`libc370` `sleep` / `ecb_timed_wait`, `@@ecbtwl.c`: `STIMER REAL,(exit),
  BINTVL=` + a timer exit that `POST`s an ECB + `WAIT` + `TTIMER CANCEL`).

## Decision

**Drive the 100 ms tick with a single `STIMER REAL`, re-armed to the head delta
by `nsftmr_run`.**

- One tick = 100 ms. `STIMER` BINTVL is in 0.01 s units, so the interval armed
  is `BINTVL = ticks * 10`.
- NSFTMR only ever needs **one** active interval (the soonest deadline), so the
  single-interval `STIMER` is not a limitation: after firing, `nsftmr_run`
  re-arms for the new head; an idle queue takes zero timer interrupts. This
  reproduces exactly the behaviour the `STIMERM` wording intended.
- The timer exit does nothing but `POST` the timer ECB; all timer processing
  runs on the executive task (spec §6.1). `TTIMER CANCEL` disarms.
- The arming is isolated behind the `nsfstim.h` platform seam
  (`nsftmr_plat_arm` / `_disarm` / `_ecb`): `asm/nsfstim.asm` on MVS,
  `src/nsfstim_host.c` (no-op, records arms for tests) on the host, swapped by
  `project.toml` `[host].replace` — the NSFXQ / NSFTIME pattern (ADR-0016).

## Gate (must MEASURE before freeze)

`STIMER` accuracy under Hercules is an empirical question, so ADR-0011 is **not
frozen** until the on-MVS timer-accuracy job measures it:

- `test/mvs/tsttmacc.c` (`host = false`) times the STIMER **timebase** with a
  task-synchronous `STIMER WAIT` (`test/asm/tststmw.asm`): arm 100 ms ≥ 100
  times, block, read each real interval with `nsf_now` (STCK). It reports mean /
  min / max / jitter. `STIMER WAIT` shares the emulated TOD timebase with
  `STIMER REAL` / `STIMERM`, so it validates the tick's accuracy; the async
  exit-dispatch path is characterized separately at M0-6.
- **PASS:** mean ∈ [90, 110] ms **and** no single interval > 200 ms (so the
  200 ms delayed ACK and ≥ 1 s RTO never fire early). On failure it prints the
  full distribution — never a silent pass — so the tick can be revisited.

**Status of the gate (M0-5): NOT measured — blocked by a runtime prerequisite,
not a clean deferral.** The job builds and cross-links; live `make test-mvs` runs
surfaced (and we fixed) a real seam bug — `asm/nsfstim.asm` addressed its ECB at
base 0 → ABEND S102 — but the run still ABENDed in the cc370 C-runtime call path.
A **staged isolation on MVS** then localized the cause and it is **not the timer**:

| Stage | Contents | Result |
|---|---|---|
| 1 | `printf` only (~TSTSMOKE) | BATCH + TSO **CC 0** |
| 2 | + `nsf_now` (links `asm/nsftime.asm`) | **ABEND S0C6** (cc370 C-runtime path, `CLIBCRT`) |
| 3 | + `STIMER WAIT` | ABEND S0C4 (moot — stage 2 already fails) |

Merely linking `asm/nsftime.asm` (which `nsf_now` — and therefore this job —
depends on) breaks mainline C on MVS; `nsf_now`'s STCK itself executes fine. That
is a **general mainline-runtime blocker**, tracked as **issue #8** and a
**prerequisite for M0-6** (NSFEVT is mainline C on MVS; NSFTRC is affected too).

So the accuracy gate is **not "deferred"** — it is **blocked on issue #8**. Once
#8 is fixed, run `make test-mvs` on `test/mvs/tsttmacc.c` to obtain the
distribution; only then may this ADR be frozen. The async `STIMER`-exit dispatch
validation (`asm/nsfstim.asm`) is a separate M0-6 item that could not be reached
past #8.

## Consequences

- The `STIMER` seam (`asm/nsfstim.asm`) owns one module-resident timer ECB —
  correct, because there is exactly one timer service (one executive task). The
  ECB moves into an executive control block and the loop's WAIT ECBLIST at M0-6.
- **Do not "restore" `STIMERM`** anywhere in the code or docs believing the spec
  mandates it: it will not assemble on 3.8j. (Same class of load-bearing
  correction as ADR-0015's "do not restore a raw GETMAIN SVC".)
- If the accuracy gate fails on real hardware/emulation, the fix is a coarser
  tick or a different interval source — re-open this ADR with the measured
  distribution; do not silently widen the PASS band.

## Alternatives rejected

- **`STIMERM` (multiple intervals).** Not available on 3.8j (see Context); would
  not assemble. Unnecessary anyway — the delta queue needs only the head.
- **Timer wheel.** Rejected in ADR-0010 (fixed memory cost for a tiny
  active-timer population).
- **`STIMER WAIT` (task-synchronous, inline).** Blocks the executive task inside
  the arming call, incompatible with the single event-loop WAIT-on-ECBLIST
  model. The REAL-interval + exit-POST + ECB path keeps arming non-blocking.
