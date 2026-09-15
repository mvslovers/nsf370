# M5-2e Job B — STOPPED AT P0. No measurement was taken.

**Date:** 2026-09-15 · **Stand:** MVSCE-DEV on mvsdev, freshly rebooted
**Predictions:** `predictions.md`, written before `S NSFS` and **not edited**.
**Proof kind:** none — **this round produced no measurement**, by design.

**`NSF055I` read `1187840`, not `1073152`. The locked condition says stop, and
the round stopped.** Nothing was measured, nothing was adjusted, and no run was
attempted.

---

## The gate, and the reading

The condition, as locked:

> `S NSFS`, then read `NSF055I`. Expected: **1073152**. If it is lower, a
> retained anchor is impossible after an IPL, so a different value means
> something else changed — **stop and report, do not proceed and do not
> adjust.**

What `S NSFS` reported (STC started 10:06:18, console log):

```
NSF851I NSFS NON-SWAPPABLE (SYSEVENT DONTSWAP, NDS=1)
NSF040I NSFS 0.1.0-dev STARTING (PHASE 2)
NSF210I CTCI 0500/0501 UP DD SYS00003/SYS00005 MTU 1500
NSF211I INTERFACE LNK1 CUU 0500 UP
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 1187840
NSF042I SVC 239 STOLEN (EP 00A6E248)
NSF041I NSFS TRANSPORT READY -- ANCHOR=00A6E7C8 ECB=000BE814
NSF001I NSFS INITIALIZATION COMPLETE
```

**1187840 − 1073152 = 114688 bytes = 112 KB MORE free CSA than the baseline.**

**It is higher, not lower.** The condition anticipated *lower* and gave the
reason a lower value could not be CSA debt after an IPL. A **higher** value is
outside what the condition anticipated at all, which is more reason to stop
than less: the round has no entitlement to treat any value but 1073152 as
normal, and `predictions.md` says so in those words before the fact.

## A candidate explanation — NOT established, and not acted on

**Two fewer started tasks are consuming CSA than when the baseline was
recorded.**

| | running |
|---|---|
| now | JES2, NET, TSO, **HTTPD** |
| when `1073152` was recorded (3 Sep, and every earlier reading) | JES2, NET, TSO, **UFSD, FTPD, HTTPD** |

`UFSD` and `FTPD` were **not started after the reboot** — Mike started only
`MVSCE-DEV`'s Hercules, and inside MVS only HTTPD came up. Two absent STCs
freeing ~112 KB of common storage between them is of the right order.

**This is offered as a candidate and nothing was done about it.** It was not
tested, the round did not proceed on the strength of it, and no number was
adjusted to fit it. Confirming it would mean starting `UFSD` and `FTPD`,
restarting NSFS so its `NSF055I` reflects the new CSA state, and reading again
— **which is a change to the stand and Mike's call, not this round's.**

## Why this matters more than it looks

The baseline is not decoration. **`LARGEST FREE BLOCK` before and after every
run is one of the round's discard criteria**, and an `NSF054W` discards a run
outright. A baseline taken under a different set of started tasks would make
every per-run pool reading in this round incomparable with every figure already
in the tree — and the tree's readings (`docs/measurements/40-chk/`,
`40-ident/`, `m5-2e/stage-a.md`) all say 1073152.

**Proceeding would have produced numbers that look like the others and are not
comparable with them.** That is the failure this project has catalogued
repeatedly, and the gate exists to catch exactly it.

## What was and was not done

- **Done:** predictions written and frozen; module currency verified (no commit
  since Job A's deploy touches `src/`, `asm/`, `include/`, `samples/`); TESTLIB
  and `NSF.LINKLIB` confirmed intact after the reboot (`TSTRQXC` present, all
  six modules present); `S NSFS`; the reading; `P NSFS`.
- **Not done:** `MSP`, `MS`, `MA`/`MB` — **no run of any arm was attempted**.
  No swap instrumentation was taken, because there was no run to instrument.

**The stand is left idle and clean:** `P NSFS` gave `NSF043I SVC 239 RESTORED`
→ `NSF044I` → `NSF011I` → `IEF404I`, **no `NSF054W`**; `IEA995I` 0 and
`IEF450I` 0 for the whole IPL, so the zero dumps are "nothing failed" rather
than a suppressed dump. One emulator running, `tun0` still up.

## Instrument note, because it nearly cost the round its tooling

The `zowe` CLI is **broken on the controlling machine** under Node **26.7.0**:
`util.isNullOrUndefined` was removed, and zowe's own logger calls it — so even
`zowe --version` throws **inside the error formatter**, which hides whatever
the real error was. It is driven through a Node 22 wrapper for this round.

**`make deploy` / `make test-mvs` are unaffected** — mbt speaks to mvsMF over
Python `urllib`, not through zowe. Worth recording because the symptom
(everything zowe touches fails with an unrelated-looking TypeError) invites
diagnosing the stand instead of the workstation. **mvsMF answered a direct
`curl` in 67 ms with the expected 401 throughout**, which is what separated the
two.

## What would let this round run

1. **A ruling on the baseline.** Either start `UFSD` and `FTPD`, restart NSFS
   and re-read `NSF055I` — expecting 1073152 if the candidate above is right —
   or re-baseline deliberately, recording the new value and its conditions and
   accepting that it is not comparable with the tree's existing readings.
2. Then the round as locked: `MSP` first as the instrument gate, then three
   valid runs at the 300 s window, with swap instrumentation and per-run pool
   readings.

**Nothing flips to proven, no milestone moves, and no measurement exists to
cite from this round.**

---

# CORRECTION AND SECOND STOP — 2026-09-15

**Appended; nothing above rewritten.** The stop recorded above was right. Its
**reasoning** is corrected here: it was not wrong, it was **narrower than the
case needed**, and the narrow version would not survive being leaned on.

## §1 — the reason on file is the weaker one. The argument is LOAD, not storage.

The record above justifies the stop on **CSA comparability**. That does not
carry as far as it looks:

- **Job B's verb is `XC_FN_UNKNOWN`** — no socket, no app slot, no `ubuf`. The
  size of the largest free block **is not an input to any figure it produces**.
- **`LARGEST FREE BLOCK` before and after each run is a condition marker, and
  it detects retention within the round — by a DELTA.** A delta is comparable
  against any base, so an unusual base does not spoil it.
- **And the reading was HIGHER**, so it could not have been CSA debt under any
  reading.

**The argument that does carry is load.** `UFSD` and `FTPD` absent is not
merely 112 KB of free common storage — it is **two started tasks not consuming
dispatcher time on the emulator**. Job B measures **throughput and the deferral
distribution**. A lighter machine yields better numbers for a reason that has
**nothing to do with the stack**, and those numbers would be incomparable with
`m5-2e/stage-a.md` in **exactly the dimension the baseline exists to fix**.

> **The free block was the SYMPTOM that made the difference visible. The load
> is what makes the run invalid.**

## §2 — the set restored, with the "before" read rather than reconstructed

`task-set-before.txt` (03.14.57, nothing started): JES2, NET, TSO, HTTPD up;
**UFSD and FTPD absent**; NSFS down. **All six of the recorded normal set were
checked, not only the two** — the difference is exactly those two, there is no
third missing task, and nothing beyond them was started.

`task-set-after.txt` (03.15.51): JES2, NET, TSO, HTTPD, **UFSD**, **FTPD** —
the recorded normal set, restored.

## §3 — PB0 IS FALSIFIED, and that is the second finding

The prediction, written before `UFSD`/`FTPD` were started and unedited:

> With `UFSD` and `FTPD` started and the rest of the recorded set up, a fresh
> `S NSFS` will report `NSF055I ... LARGEST FREE BLOCK NOW 1073152` — EXACTLY,
> not approximately. **Falsified by any other value, including higher and
> including close.**

What it reported (STC started 10:16:00):

```
NSF210I CTCI 0500/0501 UP DD SYS00003/SYS00005 MTU 1500
NSF211I INTERFACE LNK1 CUU 0500 UP
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 1069056
NSF042I SVC 239 STOLEN (EP 00A8B248)
```

**1069056. Falsified — by 4096 bytes, LOW.**

| reading | value | Δ from baseline |
|---|---|---|
| before restoring the two STCs | 1187840 | **+114688** |
| after restoring them | **1069056** | **−4096** |
| recorded baseline | 1073152 | — |

**What was running when it was taken:** JES2, NET, TSO, HTTPD, UFSD, FTPD and
NSFS — the recorded normal set plus the STC being started. One emulator,
`tun0` up, host rebooted this morning.

**Nothing was started, stopped or restarted to make the number move**, and it
was not re-read. The round stops here a second time.

### The size of the miss, characterised — and this is NOT a rescue of the prediction

`nsfsx_csa_largest` (`src/nsfsx.c`) doubles until `getmain` fails, then refines
`while (hi - lo > 4096U)` and **returns `lo`**. So the published figure is a
**4 KB-quantised LOWER BOUND**: the true largest free block lies somewhere in
`[value, value + 4096)`.

**1069056 and 1073152 are therefore ADJACENT STEPS of the instrument**, and two
readings one step apart can reflect a true difference of anywhere between 1 and
8191 bytes.

**This does not confirm PB0.** The prediction was an equality, deliberately,
and it is falsified. What the quantisation establishes is a **limit on the
test**: at 4 KB resolution this reading **cannot distinguish** *"the two STCs
account for the whole difference"* from *"they account for nearly all of it"*.
Recorded as a property of the instrument, not as a reason to proceed.

### The decision this leaves, which is Mike's

The two conditions have come apart, and they should be named separately:

- **The LOAD condition — the one §1 establishes actually matters — is now
  satisfied.** The recorded normal set is running.
- **The STORAGE marker is one quantisation step off** the figure every other
  record in the tree quotes.

**Proceeding is not this round's call**, because the locked condition is an
equality on the marker and the instruction on a miss is explicit: record, stop,
report. Both readings and their conditions are above; **the ruling is Mike's.**

## §4 — the tooling fault belongs in the record, not only in a report

`zowe --version` **throws inside zowe's own error formatter** under this
machine's Node 26.7.0: `util.isNullOrUndefined` was removed from Node and
zowe's logger still calls it. The instructive part is that **the symptom points
the wrong way** — a failure in the *error path* hides the real error and reads
as a problem with the stand.

**The positive control that separated the workstation from the machine:** a
direct `curl http://mvsdev.lan:8080/zosmf/info` answering **401 in 67 ms**.
mvsMF was healthy throughout. Driven through a Node 22 wrapper for this round.
**`make deploy` / `make test-mvs` are unaffected** — mbt speaks to mvsMF over
Python `urllib`, not through zowe.

**Running instrument-fault tally**, kept as before — **six faults, five caught
by a control and one by review:**

| # | fault | caught by |
|---|---|---|
| 1 | `getcap` not on the non-interactive PATH | control |
| 2 | `dpkg.log` empty in the date range | control |
| 3 | quote-checker failing **closed** | control |
| 4 | `awk` quoting error printing an empty interface list | control |
| 5 | "started exactly once" — per-start log truncation | **review** |
| 6 | **zowe broken under Node 26, error formatter hiding the error** | control (the direct `curl`) |

## §5 — the stand as left

NSFS running (started for the reading and deliberately not recycled), the
recorded normal set up, one emulator, `tun0` up. **No run of any arm was
attempted in this round either** — no `MSP`, no `MS`, no `MA`/`MB`, and no swap
instrumentation, because there was nothing to instrument.

---

# THE ROUND RAN — 2026-09-15

**Appended; nothing above rewritten.** Both earlier stops stand as recorded.
Entry condition restated by purpose (the ruling, with its substance, is in
`predictions.md` and was written before any run).

**Proof kind: LIVE.** The host figure quoted elsewhere is a no-regression check
and is not part of this round.

## Conditions, recorded because the numbers do not carry them

| | |
|---|---|
| host | rebooted 2026-09-15 ~09:49 |
| emulators | **ONE** — `MVSCE-DEV`; the other four not started |
| started tasks | JES2, NET, TSO, HTTPD, UFSD, FTPD (the recorded normal set, restored) |
| **start order** | **HTTPD by hand immediately after the IPL; UFSD and FTPD ~9 minutes later.** This baseline was taken under a **DIFFERENT START ORDER** than the 1073152 series (`40-chk/`, `40-ident/`, `stage-a.md`). |
| entry pool | **1069056** — within one quantisation step of the clean expectation; a retained anchor is 139264 bytes, 34 steps |
| counters | post-`58dfaab` set; **no reading from before it is comparable** |
| instrument | `zowe` via a Node 22 wrapper (see above); `make` unaffected |

## PB2 — the instrument gate PASSED, so the numbers are believed

`MSP`, 300 s: pace **ARMED** at 1 cs, min **10442 µs**, mean **10691 µs**, max
13834 µs, and **0 samples below 5 ms**. 93 served/s under the pace.

**The gate ran first and no measurement number was read before it passed.**

## The three runs

Each run = one solo arm (`MS`) then one two-client pair (`MA`/`MB`), 300 s
window, 60 s discarded, **240 s reported**. NSFS was stopped and restarted
between runs, which is how the pool is read.

| run | solo served/s | solo mean | MA served/s | MB served/s | **combined** | **ratio** | per-client mean |
|---|---|---|---|---|---|---|---|
| 1 | 2092 | 471 µs | 2491 | 2491 | **4982** | **2.38×** | 395 µs |
| 2 | 2063 | 478 µs | 2457 | 2457 | **4914** | **2.38×** | 401 µs |
| 3 | 2050 | 481 µs | 2477 | 2478 | **4955** | **2.42×** | 397 µs |

**Run-to-run spread:** solo 2050–2092 (2.0 %), combined 4914–4982 (1.4 %).
`refused = 0` and `bad = 0` in every arm of every run — the per-request
identity check held across **3.3 million** served requests.

**No starvation.** Run 3's pair, per 15 s interval: MA 38238 / 37684 / 37334 /
37645 / 37273 / 36328 / 36456 …, MB 38206 / 37533 / 37244 / 37602 / 37405 /
36439 / 36367 … — the two track each other within 0.4 % in every interval, and
neither has a zero interval.

## PB4 IS FALSIFIED — combined is 2.38–2.42x solo, not under 2x

The prediction said combined would exceed solo and be **less than 2×**, with
the falsification clause naming ≥ 2× as *"a finding about ADR-0042 §10, not
about this test"*. It is ≥ 2× in all three runs.

**Stage a's trial saw ~2.4× and recorded it explicitly as a thing to test, not
a result. It reproduces at the 300 s window, three times, with a 1.4 % spread.**

**This does NOT contradict ADR-0042 §10, and the distinction is the finding.**
Serialised *service* — one request in flight — does not imply serialised
*throughput*. A single client's rate is bounded by its own round trip: it
issues, waits, and is idle again before the next request. If one client kept
the executive busy, two could not be 2.4× faster. So what the measurement shows
is:

> **One client does not saturate the transport. At one client the limit is the
> CLIENT's round trip, not the executive's service rate** — the executive is
> idle roughly 58 % of the time, and a second client fills those gaps rather
> than queueing behind service.

Per-client latency *falling* with two clients (471 → 395 µs) is the same fact
from the other side, and is what a queueing model would not predict.

**What it does not establish:** the ceiling. Two clients were measured; where
the curve turns over is unmeasured, and nothing here licenses extrapolating to
three or to sixty-four.

## PB1 HELD — the 4 KB offset is now established as a start-order artifact

Every pool reading in the round, at each NSFS start:

```
10:16:00  1069056   <- entry, before run 1
11:02:52  1069056   <- after run 1 / before run 2
11:15:09  1069056   <- after run 2 / before run 3
11:27:29  1069056   <- after run 3
```

**Delta ZERO across all three runs.** Per the ruling's own test, that
**establishes** the 4096-byte offset as a start-order artifact rather than
assuming it: the reading is stable within a round and under load, so
fragmentation from the changed start order is the remaining explanation rather
than one of several.

**The residue stays residue.** A zero delta does not tell us *what* the ≤ 8 KB
is, and this round does not claim to. It tells us the figure does not move
while the round runs.

## PB3 HELD — no swap transition in any run

Six samplers, 74 samples, 30 s interval:

| run | arm | samples | ASCBSTOR | QFL | SWC | NSW |
|---|---|---|---|---|---|---|
| 1 | MS / MA+MB | 13 / 13 | `0FA20C00` constant | `00` | 0 | set |
| 2 | MS / MA+MB | 12 / 12 | `0FA16C00` constant | `00` | 0 | set |
| 3 | MS / MA+MB | 12 / 12 | `0F8B8C00` constant | `00` | 0 | set |

**`ASCBSTOR` differs BETWEEN runs only because NSFS was restarted between them
— a different address space each time.** Within every run it is constant, which
is what PB3 asserts. `OUCBSWC` is 0 throughout, so no completed cycle was
missed between samples either. **No run was discarded and there is no
ADR-0044 finding.**

**Sampling was at 30 s, not the original 3 s**, because the sampler reads
through HTTPD **on the guest being measured** and this round measures
throughput — the instrument trap this project has recorded twice. A completed
cycle still shows retrospectively in `ASCBSTOR`/`OUCBSWC`, so the coarse
interval loses only mid-flight capture. `docs/measurements/m5-2e-jobb/swapwatch.py`,
derived from `64-3-1/nsfswatch.py`, which is left untouched as that round's
artifact.

## The STC's own side

After run 3 (`F NSFS,STATS`, post-`58dfaab` set): `SERVED=2108931`,
`EVTPASSES=2729226`, `WAKEPOSTS=2086369`, **`COLLISIONS=742361`**,
**`EXHAUSTED=0`**, `BUSY=0 BUSYSLOT=-1 INFLIGHT=0 REAPED=0`, `APPSWEEP
SWEEPS=69 RECLAIMED=0`.

**`EXHAUSTED=0` with `COLLISIONS` in the hundreds of thousands** is the same
reading stage a got and means the same thing: with 64 slots and two clients
nobody is ever turned away, so the contention is **service serialisation, not
slot starvation**.

## Stand

**`NSF054W`: 0. `IEA995I`: 0. `IEF450I`: 0** for the whole IPL — so the zero
dumps are "nothing failed", not a suppressed dump. **No run was discarded.**
One emulator, `tun0` up, NSFS running, the recorded task set up.

## What this round does NOT establish

- **Anything about Job A** (#108), which is judged separately and reported no
  timings. **None are mixed in.**
- **Comparability with stage a's figures**, which are trial-shaped (90 s) and
  were taken under a different host configuration.
- **The concurrency ceiling** — two clients, not three, not 64.
- **What the ≤ 8 KB pool residue is.**
- **A milestone flip.** Nothing here flips to proven.
