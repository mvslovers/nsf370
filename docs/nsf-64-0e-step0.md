# Issue #64, step 64-0e §0 — does the parked client precede the stall, or follow it?

**Documentary analysis. No machine time was spent, nothing was deployed, no round was run.**
This answers the free question 64-0e puts before its live arm, from the four measurement records
already in the tree. It also reports two things that bear on how that arm should be built, and
one defect in a preserved measurement.

Written 2026-08-27, against `main` at `fee1d31` (#72 merged, ADR-0043 landed).
Console times are the MVS clock (UTC-5); host times CEST, as in the source records.

---

## 1. The answer

> **Precedes — in every recorded stall, without exception.**

The condition is therefore a candidate provocation and is not an artefact of the detector
catching a stall before a request was issued. The hypothesis 64-0e §0 was written to exclude —
that the correlation is entirely downstream — is **excluded by measurement**, on two independent
grounds:

1. **`served` was non-zero at the onset of every recorded stall.** `served` only advances when
   the executive completes a request, and it is frozen for the whole of a stall. So a non-zero
   reading is proof that at least one client had published a request *and had it serviced*
   before the stall began.
2. **The client kept publishing throughout each stall**, which is visible in counters the
   *client's* SVC routine increments (`collisions`, `exhausted`) while `served` stays frozen.
   The request stream is not created by the stall — the stall converts it into refusals.

**But "precedes" licenses less than it looks like, and §4 says what.** What precedes is a client
*submitting requests at rate*. A client *parked for minutes* is the stall's own effect, and an
arm designed to hold that state would be designed around the consequence.

---

## 2. Per stall, with timestamps

Nine stalls are on record across four rounds plus issue #64's own. `served` is the load-bearing
column: it is frozen during a stall, so the value read *during* one is the value it held at
onset.

| # | round | stall onset — first evidence | `served` at onset | what was running before | verdict |
|---|---|---|---|---|---|
| 0 | #64's own | not recorded | **397** (`0x18D`) | 397 requests already completed | **precedes** |
| 1 | 64-0b | ≤ `7.59.04` (MODIFY issued, unanswered until `8.02.14`) | **42** | `TSTRQXM` + `TSTRQXC` + the two-client gate; §1 records both reproductions as happening *"during the workload itself"* | **precedes** |
| 2 | 64-0b | ≤ `8.09.05` (unanswered until `8.14.57`) | **450** | the gate — §5: *"A was itself hung by stall 2 mid-gate"* | **precedes** |
| 3 | 64-0c | `20:30:12` last EJST movement; flat from `20:30:22` | **375** | §6: *"Both reproductions fired within 90 seconds of a gate round starting"* | **precedes** |
| 4 | 64-0c | ≤ `13.44.02` (MODIFY; a manual one at `13.43.30` waited 3 m 42 s) | > 375 (same instance) | same arm | **precedes** |
| 5 | 64-0d | **≈ `10:44:06`** (computed in §3) | **341** | ~29 s of full-core work, 341 requests | **precedes** |
| 6 | 64-0d | ≈ `11:08:40` (flat 15 s, detected `11:08:55`) | **5 602** | armed `10:54:34`; 5 602 requests since | **precedes** |
| 7 | 64-0d | ≈ `11:28:29` (flat, detected `11:28:44`) | **8 027** | 8 027 requests since | **precedes** |
| 8 | 64-0d | ≈ `11:46:03` (detected `11:46:18`) | not printed; > 8 648 (stall 7's exit reading) | same campaign, continuous | **precedes** |

Sources: `docs/nsf-64-0b-measurements.md` §1/§4/§5; `docs/nsf-64-0c-measurements.md` §6;
`docs/measurements/64-0d/stall1-passive.log`, `stall2-ladder.log`, `stall3-control.log`,
`stall4-rung-then-ping.log`; `docs/nsf-64-0-measurements.md` §4.2 for #64's own `served = 0x18D`.

**There is no recorded stall with `served = 0`**, and none detected on an instance that had not
yet been given a request. Every round that ran an *idle* arm — 64-0, 64-0b's prescribed window,
64-0c's first 22 samples, 64-1's seven idle windows — produced no stall at all, which is the
same fact from the other side: the configuration in which the condition is absent has never
stalled either.

---

## 3. The best-resolved case: 64-0d stall 1, to the second

`stall1-passive.log` samples `ASCBEJST` every ~10 s, and EJST is accumulated job-step CPU time,
so the deltas date the workload independently of any counter. At 1 CPU-second = 4.096 × 10⁹ TOD
units (bit 51 = 1 µs):

| sample | time | Δ EJST | = CPU-s / 10 s | reading |
|---|---|---|---|---|
| 1–5 | 10:42:48 → 10:43:29 | ~1.4 × 10⁸ | 0.03 | **0.3 % of a CPU — idle, blocking on the WAIT, heartbeat only** |
| 6 | 10:43:39 | 8.61 × 10⁹ | 2.10 | 21 % — the burn starts **≈ 80 % into this interval ⇒ ≈ 10:43:37** |
| 7 | 10:43:49 | 4.446 × 10¹⁰ | 10.85 | **108 % — a full core** |
| 8 | 10:43:59 | 4.410 × 10¹⁰ | 10.77 | 108 % |
| 9 | 10:44:10 | 2.495 × 10¹⁰ | 6.09 | 61 % — the burn **stops ≈ 56 % into this interval ⇒ ≈ 10:44:06** |
| 10+ | 10:44:20 → | **0** | 0 | flat; detector fires at 10:44:40 |

So the timeline is:

```
 ≤10:43:29   fresh instance, 0.3 % CPU, no request yet
                (the ECB is unlatched: on that module the FIRST request starts the spin)
 ~10:43:37   first cross-address-space request      <-- the client's stream begins
  10:43:37   ..  ~10:44:06   full-core spin, serving; `served` reaches 341
 ~10:44:06   EJST stops dead                        <-- the stall begins
  10:44:40   passive reading: inflight=1, served=341, SLOT0 PENDING
```

**The client's request stream began ≈ 29 seconds before the stall, and 341 requests completed
inside that window.** This is the one stall on record where the onset and the workload's start
are separately dated rather than inferred, and the order is unambiguous.

---

## 4. What "precedes" does and does not license — the design correction

Three things are being run together under one name, and 64-0e §2 turns on which of them the arm
reproduces:

| | | evidence |
|---|---|---|
| **(A)** a client **submitting requests at rate** | **precedes** every stall | §2, §3 — measured |
| **(B)** a request **published and outstanding at the instant of onset** | continuously true throughout the workload; **not created by the stall** | §5 below — measured indirectly; per-request resolution is not available at any sampling rate any round used |
| **(C)** a client **parked for minutes** | **follows** — this is the stall's own effect | trivially: the executive services nothing, so whatever is outstanding stays outstanding |

64-1's *"`PENDING` in 3 of 360 reads ≈ 0.8 %"* is a measurement of **(B)** on a **healthy**
executive, and its low value is a property of health, not of a stall: a healthy executive
services a request in ≈ 1.95 passes, so `PENDING` is brief by construction. It is not a figure
about a consequence — 64-1's campaign contained no stall — but it is also not the exposure
figure it was read as, because what preceded the stalls is **(A)**, not **(B)**'s duty cycle.

**The correction that matters for §2: an arm that holds one client parked indefinitely
reproduces (C), the consequence.** The arm that actually reproduces is a client stream at rate.

---

## 5. The client kept publishing *during* every stall — measured

The counters the **client's** SVC routine increments are the ones that move while the
executive's do not. From `stall1-passive.log`, one stall, three passes:

| pass | time | `served` | `inflight` | `exhausted` | `collisions` |
|---|---|---|---|---|---|
| STALL-pass1 | 10:44:40 | 341 | 1 | 3 770 | 241 430 |
| STALL-pass2 | 10:46:54 | **341** (frozen) | 1 | **16 792** | **1 074 838** |
| AFTER | 10:51:19 | 1 418 | 0 | 44 107 | 2 822 998 |

`served` frozen for 2 m 14 s while `collisions` rose by 833 408 and `exhausted` by 13 022 — a
ratio of **exactly 64.0000**, i.e. ≈ 13 022 complete 64-slot scans, ≈ 97 per second (§7). Those
increments happen inside the SVC routine, in the **client's** address space, on a stalled STC —
so the client was hammering the claim scan and being refused throughout. `stall2-ladder.log` and
`stall3-control.log` show the same shape (`inflight` 2 and 1 respectively, held across the whole
stall, with `served` advancing only after it ended).

This is what makes the "downstream artefact" reading impossible: the request stream is
demonstrably present before, during and after, and the stall is what interrupts it.

---

## 6. An instrument gap that changes what §2 can measure

64-0e §1 says the recipe *"simplifies to NSFV plus a client plus idle windows"*. There is no
client on NSFV comparable to the one that reproduces, and the gap is large enough to be a third
residual confound — larger than the two §1 already names, because it is a property of the
**workload**, which is the thing the round varies.

| | the arm that reproduces (NSFS) | what NSFV has today |
|---|---|---|
| driver | `TSTRQXCA` + `TSTRQXCB` — **two address spaces** | `TSTSVC` (53 sequential ECHO round trips: 50 + 3 edge), `TSTUBUF`, `TSTDEATH`, `TSTXFW`, `TSTMVCK` — all **one** address space, all short |
| claim rate | ~3 000 attempts per client per ~10 s round | one at a time, one job at a time |
| pool state | **saturated** — `collisions` 670 052 and `exhausted` 10 364 across 45 rounds (`docs/nsf-64-1-measurements.md` §6); 241 430 → 2 822 998 inside one 64-0d stall | **`COLL=0` across the 126 sequential requests of the Stage-0 four**, measured (ADR-0042 annotation §7) — slot 0 is free every time, so the first `CS` always succeeds |
| second client | by design | `TSTSVC` **cannot** be run concurrently with itself: it asserts `req.seq == base_seq + i`, i.e. a monotonic `served` with no interleaving. `TSTUBUF` / `TSTDEATH` carry no such assertion and could pair, but both are short |

So an NSFV arm built from what is deployed today gives **tens of requests per job with zero
contention**, against an arm that produced thousands of claim attempts per second across two
address spaces with a full pool. A quiet result from it would be weak on an axis §1 did not
anticipate, and the honest report would have to say the arms differ in the workload as well as
in the device.

**Three ways forward, none of them mine to pick:**

- **(a) Run it as written**, with looped `TSTSVC` submissions for exposure, and report a
  triple-confounded arm — device, *plus* supervisor-state WAIT and wait set, *plus* workload.
  Cheapest; a quiet result says very little.
- **(b) Build an NSFV load client first** — a `[[test]]` mirroring the gate's claim loop against
  NSFV's ECHO verb, two address spaces, so the workload is held constant across the arms. A
  test-only change (§4's red line permits an instrument change if named), and it needs its own
  validation before its silence counts.
- **(c) Close 64-1's own gap instead.** 64-1's null is weak because its 45 gate rounds each ran
  ~10 s, while every stall on record fired **40–90 s into a round**, so its campaign never
  reached the window in which stalls occur. Long-running gate rounds against post-reset NSFS
  test the spin-as-provocation hypothesis directly, need no new instrument, and would make any
  later NSFV arm interpretable — a quiet NSFV run means nothing if the NSFS control is also
  quiet, which is what 64-1 already measured.

---

## 7. A "defect" I found, then refuted — the rows are real, and they prove something better

**A first draft of this section reported the per-slot rows of
`docs/measurements/64-0d/stall1-passive.log` as untrustworthy. That was wrong, and the
retraction is worth more than the claim was.** Both stall passes print:

```
  SLOT0  state=1 reply_ecb=809DE5F0[WAIT ] req_ascb=FE7980 req_asid=0007
  SLOT1  state=4 reply_ecb=40000000[POSTED] req_ascb=FE7980 req_asid=0007
  SLOT2  state=4 reply_ecb=00000000[] req_ascb=000000 req_asid=0000
  …  SLOT2–SLOT62 state=4 (CLAIMED),  SLOT63 state=3 (HELD),  all with a zero owner identity
```

CLAIMED with **no owner** looked contradictory, because a claim records the owner's ASCB and
ASID at `CLAIMOK`. **It is not a claim.** `asm/nsfvsvc.asm`'s `DOSLOT` — the `FNSLOT` probe verb,
one of the three that *"NAME a slot instead of claiming one"* and branch out ahead of the claim —
is a bare `CS` of `REQSEXP`→`REQSNEW` on `SLSTATE` and **stores nothing into `SLRASCB`/`SLASID`**.
A slot taken that way never reaches `CLAIMOK`. And that is exactly what the round's workload
does: `test/mvs/tstrqxc.c` phase 2 *"pre-claims slots 1..61, leaving ONLY slot 0"* through
`xc_slot_cas`, and uses slots `XC_FLAG_A = 63` and `XC_FLAG_B = 62` set to **`HELD`** as its
two coordination flags. Slot 1 carries an identity because release is a plain `ST` of `FREE` that
leaves the identity fields as residue from an earlier real request; slots 2–62 never held one.

**The arithmetic confirms it outright, and this is the part worth keeping.** A claim scan that
finds no FREE slot counts one `collision` per slot examined and one `exhausted` for the attempt.
Across both intervals of stall 1:

| interval | Δ `collisions` | Δ `exhausted` | ratio |
|---|---|---|---|
| pass1 → pass2 | 833 408 | 13 022 | **64.0000** |
| pass2 → after | 1 748 160 | 27 315 | **64.0000** |

**Exactly 64 collisions per exhausted attempt, twice, to four decimal places.** That is only
possible if **all 64 slots were non-FREE on every scan** — slot 0 `PENDING` with the parked
request, slots 1–61 pre-claimed, slots 62 and 63 the two `HELD` flags — and it independently
dates ≈ 13 022 full scans inside the 2 m 14 s between the stall passes, ≈ 97 per second. The
log is coherent, the tool was fine, and the census is the gate's own design read back.

**What survives, and it belongs to §6:** the pool saturation in the arm that reproduces is
**manufactured by the test**, not organic contention. That does not weaken §6's gap — NSFV still
has no client that produces it — but it changes the cost of closing it, because `FNSLOT` is
served by the same shared SVC routine and an NSFV pre-claimer would work the same way.

**And §5 is unaffected**, which is why the retraction costs nothing: §5 rests on `collisions` and
`exhausted` moving while `served` is frozen, and those are header fields read through
`watch2.py`'s known offsets. The 64.0000 ratio makes them stronger, not weaker.

---

## 8. An observation, offered as exactly that

Every stall on record occurred on a **spinning** instance, at cumulative `served` between 42 and
~8 600. The one post-reset campaign served **164 570** with none.

| round | instance | requests served | stalls |
|---|---|---|---|
| 64-0b | spinning | ~1 038 (workload arm) | 2 |
| 64-0c | spinning | 375+ | 2 |
| 64-0d | spinning | ~8 648 | 4 |
| **64-1** | **reset — no spin** | **164 570** | **0** |

By request count that is a three-orders-of-magnitude difference in exposure with the outcome
reversed. **It is not a controlled comparison and is not counted as evidence**, and one check
settles why rather than leaving it as an impression.

It is tempting to argue that 45 rounds run *back to back* must have reached the 40–90 s window
that every stall on record fired inside, whatever a single round's length. **They did not, and
the round-log dates it.** The detector was armed at 14:17:06 and idle window 1 began at 14:32:10,
so the campaign occupied **904 s for 45 rounds = 20.1 s per round slot** — ~10 s of load followed
by ~10 s of gap, a **50 % duty cycle with no continuous stretch of load longer than ~10 s**. The
window in which stalls occur was never entered.

So the axis that matters may be sustained-load *duration* rather than cumulative request count,
and round duration is itself downstream of the spin — a spinning executive made everything on
that stand slow. The two readings cannot be separated from the data that exists, and **§6(c) is
live**: it is the arm that separates them, and it needs no new instrument.

---

## 9. What this step did not do

- **No machine time.** Nothing deployed, no STC started, no console command issued, no round run.
  The one remote action was a read-only `tail` of `~/MVSCE/mvslog.txt` over `ssh`, which touches
  the host filesystem and not MVS.
- **Stand state, for whoever runs §2:** reachable; `NSFV` stopped cleanly at `7.50.20`
  (`NSFV095I SVC 239 RESTORED`, `NSFV036I`, `NSFV011I`); `D A,L` at `7.50.51` shows JES2, NET,
  TSO, UFSD, FTPD, HTTPD and no NSFS or NSFV. HTTPD is up, so `/.dm` is available. TESTLIB still
  holds the five NSFV tests (64-1 §8), so the NSFV arm needs no redeploy and the NSFS control arm
  does.
- **No detector was validated**, because none was run. §2's rule stands: validate the read path
  live before quoting silence.
- **It did not run the NSFV arm**, and it does not predict its outcome. The three predictions
  N(i)/N(ii)/N(iii) are untouched.
- **`make test-host`: 2925 PASS / 0 FAIL, 27 tests** — a no-regression check only, and evidence
  of nothing else: the diff is documentation.
