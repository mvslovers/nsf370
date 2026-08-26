# Issue #64, step 64-0b — the untried arm

**Measurement record, not an ADR and not a decision.** 64-0b fixes nothing. Decision 2 is
back with Mike and is not the evaluator's to apply.

Round: MVSCE on `mvsdev`, 2026-08-26, module built from `m5-64-0b-untried-arm`
(64-0's instrumentation from #68, plus `BUSY=` / `BUSYSLOT=`).
Console times are the MVS clock (UTC-5).

---

## 1. Headline: the stall reproduced, twice, and **none of the three predictions fits**

The kickoff's own instruction was to say so plainly rather than choose the nearest. The
measured state is **`EVTPASSES` flat AND `POSTED=Y`** — half of P2 and half of P3, and a
combination none of the three anticipated.

| | predicted | measured |
|---|---|---|
| **P1** reproduces on workload arm, not control | — | **partly**: reproduced **twice** on the workload arm, never on the control arm — but *not* during the workload arm's own prescribed idle window or timed request. It is **intermittent**, not a property of the end state. |
| **P2** reproduces, `EVTPASSES` **climbing**, `POSTED=Y` | climbing | **`POSTED=Y` ✅, climbing ❌** |
| **P3** `EVTPASSES` **flat**, `POSTED=N` | `POSTED=N` | **flat ✅, `POSTED=N` ❌** |

### The two reproductions

| # | MODIFY issued | answered | **stall** | CPU during | at wake |
|---|---|---|---|---|---|
| 1 | 7.59.04 | 8.02.14 | **3 m 10 s** | **0.5 %** | `POSTED=Y EVTPASSES=846907 SERVED=42 BUSY=0 INFLIGHT=1` |
| 2 | 8.09.05 | 8.14.57 | **5 m 52 s** | **0.9 %** | `POSTED=Y EVTPASSES=1728104 SERVED=450 BUSY=0 INFLIGHT=1` |

Both durations are **lower bounds** — each ended because a host `ping` was used to break it,
exactly as #64 reports. Stall 2 was left undisturbed for 5 m 41 s and showed no sign of
self-limiting. Both were broken by inbound device traffic and nothing else.

**The executive made ZERO passes during each stall, and that is a certainty rather than an
inference from a counter:** `nsfopr_drain` runs *unconditionally* on every pass of
`evt_mainloop`, and the MODIFY went unanswered for minutes. The CPU readings agree (0.5–0.9 %
against the 26 % the same instance draws while spinning).

## 2. What this establishes, and it is the point of the round

**An instance whose wake ECB is latched POSTED can still stall.** That is the question the
kickoff said becomes unaskable after 64-1, and the answer is **yes**.

A posted ECB in the ECBLIST makes `WAIT` return immediately. So during these stalls the
executive **cannot have been waiting on `g_wake_ecb`** — and it was not running either. It
was not dispatched.

**Consequence for 64-1, stated plainly: resetting `g_wake_ecb` cannot be the fix for #64.**
The stall occurs in precisely the state where that ECB provably is not what the loop is
waiting for. 64-1 remains justified on its own footing — an ADR-0022 violation costing a
permanent host core — and its gate is the CPU and pass-rate drop, never a #64 reproduction.

## 3. Which of §0's three facts this bears on

1. **"Nothing clears `g_wake_ecb`"** — unchallenged. `POSTED` stayed `Y` across both stalls
   and every later reading.
2. **"The POST lands"** — confirmed again, and independently: the CSA fallback ECB
   (`anchor+X'10'`) read **0** throughout, while `server_ecb_ptr` (`anchor+X'24'`) read
   **`000BCF94`**, identical to `NSF041I`'s `ECB=000BCF94`. The POST target ASCB
   (`00FDCE10`) is a valid `ASCB` with ASID `X'000B'`. **The fallback branch is excluded on
   this stand by direct storage read, not by inference.**
3. **"The loop was blocked during #64's stall"** — **this is the one measured, and it splits
   in two.** The loop was definitely **not running**. But "blocked" in the sense of *waiting
   in `WAIT` for the wake ECB* is **false** — that ECB was posted.

**Untested, and now the open question:** what the executive task actually *is* between passes
during a stall. Not swapped (§4), not spinning (CPU), not stuck in the drain (`BUSY=0`).
Establishing it needs a task-state read or a dump, which this round did not take.

## 4. What was excluded, by measurement

- **Swap-out — excluded, measured DURING a stall** (not merely after): `D A,L` showed
  `NSFS  NSFS  NSFS  V=V` with **no `S`** while the MODIFY sat unanswered, against `TSO` and
  `UFSD` which both carry it. (`D A,NSFS` and `D J,NSFS` are `IEE535I INVALID PARAMETER` on
  3.8j.)
- **The in-service slot — excluded, and this is what `BUSY`/`BUSYSLOT` were added for.**
  `BUSY=0` in **every** stall reading. The hypothesised structure — `g_busy` set with
  `g_busy_slot` pointing at a slot whose private ECB never gets posted, so step 1 waits
  forever, step 2 dispatches nothing and `nsfsx_any_pending_other` skips the in-service slot
  by design — **did not occur.** A clean negative result from instrumentation added
  specifically to test it.
- **A wake that never arrived — excluded** (fact 2 above).

The live state during stall 1, read through httpd `/.dm` while it was stalled: anchor
`00AAD7C8` eyecatcher `NSFVANCR`, version 3, flags `80000000` ACTIVE, `inflight=1`,
`served=42`; **slot 0 `req_state = 1` (PENDING)**, `reply_ecb = 809DE5F0` — WAIT bit set,
POST bit clear, i.e. the client parked and never posted. This is issue #64's own reported
shape, field for field. During stall 2: `served = 0x1C2 = 450`, `exhausted = 22196`,
`collisions = 1 420 182`. (#64's slow instance stood at `served = 397`.)

## 5. The two arms

Same idle duration (300 s) on both, `SERVED` confirmed unchanged across each window.

| arm | prior workload | POSTED | EVTPASSES before → after | rate | WAKEPOSTS | BUSY/BUSYSLOT | TMRQ | INFLIGHT | SERVED | latency |
|---|---|---|---|---|---|---|---|---|---|---|
| baseline (STC 1450, fresh) | none | **N** | 261 → 2 753 (250 s) | **9.97 /s** | 0 | 0 / −1 | 0 | 0 | 0 | — |
| **workload** (STC 1450) | `TSTRQXM` + `TSTRQXC` + two-client gate + **LEAK** | **Y** | 3 031 496 → 5 606 135 | **8 553 /s** | 3 028 587 → 5 603 226 | 0 / −1 | 0 | 1 | 1038 (unchanged) | **< 11 s** |
| **control** (STC 1452, fresh) | none | **N** → Y after | 261 → 3 367 (311 s) | **9.99 /s** | 0 | 0 / −1 | 0 | 0 | 0 (unchanged) | **< 9 s** |

The workload arm's *prescribed* window did not stall — both reproductions happened earlier,
during the workload itself. So this table is **not** where the finding lives; §1 is. Reported
this way deliberately rather than re-running until the window happened to catch one.

Two-client gate results, for the record: A `phase1 bad=0 nonzero=150 coll delta=150`,
**A CC 0000**; B `ok=177 nobuf=19823 bad=0 nonzero=0`, CC 0001 (B asserts less by design).
A was itself hung by stall 2 mid-gate and completed after the ping. LEAK left
`slot=0 state=4` (CLAIMED), `inflight 0->1`.

## 6. Cost, on purpose

`P NSFS` after the LEAK took the **retain branch** as predicted: `NSF054W 1 CLIENT(S) STILL
IN FLIGHT -- CSA AND SVC ROUTINE RETAINED (EXHAUSTED=22196)`. ~137 KB of CSA plus the SVC
routine are retained until IPL. Confirmed by two independent facts: the next STC came up on a
**different anchor** (`00ACF7C8`, was `00AAD7C8`), and `NSF055I`'s largest free block fell
**933 888 → 794 624**. An IPL already stood before (e); this round adds to what it reclaims.

## 7. Round hygiene, and one thing I got wrong mid-round

Deploy order `P NSFS` → `make deploy` → `S NSFS`; no mid-chain HTTP 500. Deploy-took-effect
confirmed by `BUSY=`/`BUSYSLOT=` being present at all. Every stop clean (`NSF043I SVC 239
RESTORED`, `NSF044I`, `NSF011I`); the one `NSF054W` was induced on purpose. **No dumps.**
Stand left with **NSFS stopped** and TESTLIB holding `TSTRQXC` alone.

**A stale log read as live evidence, caught by `mtime`.** The first `TSTRQXM` run reported
its host peer had verified 9353 bytes byte-exact — from a log file **40 minutes old**. The
`pkill -f shortwrite_listener` in the same `ssh` command matched *its own command line* and
killed the shell before the listener could start, so the listener was never up (both
`CONNECT`s genuinely got `errno 61`) and the old log was never truncated. Caught by checking
the file's timestamp rather than its contents. Exactly the CLAUDE.md §8.5 class — an
operation whose absence looked identical to its success — and worth the rule: **verify a
background listener is in `LISTEN` state, and never `pkill -f` a pattern contained in the
command doing the pkill.**

**A defect in this round's own instrumentation, reported not fixed.** The Hercules console
log truncates a message line at ~107 characters of text, so `BUSYSLOT`'s *value* was cut from
`NSF812I` once the counters grew to seven digits — `BUSY=0` (the load-bearing half) always
survived, and `zowe zos-console` returns the full line, so nothing was lost this round. But
`BUSYSLOT` sits on the wrong line: it is unreadable in the console log exactly when the
counters are large, which is exactly when a stall is being investigated. If a future round
ever sees `BUSY=1`, move `BUSYSLOT` to the short `NSF813I` first. (`wto()` itself is not the
cause — it splits at a space above 124 characters and loses nothing.)
