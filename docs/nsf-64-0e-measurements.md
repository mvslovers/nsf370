# Issue #64, step 64-0e — NSFV: the same transport, with no device

**Measurement record, not an ADR and not a decision. 64-0e fixes nothing, changes no
production source, and deploys no module.** Whether NSF mitigates an MVS condition, and in
what form, remains the maintainer's decision.

Round: MVSCE on `mvsdev`, 2026-08-27, **not IPLed**. Console times are the MVS clock (UTC-5);
host times CEST. **No production module was built or deployed** — `NSF.LINKLIB` is exactly as
64-1 left it, so no module in this round is newer than the binary 64-1 measured. There was
**one** deploy: `make test-mvs --only TSTRQXC`, which replaces TESTLIB only (§7), and its
took-effect check is recorded there.

**Step §0 — the free question this round was told to settle before spending machine time — is a
separate record: `docs/nsf-64-0e-step0.md`.** Its answer (*the parked client **precedes** every
recorded stall*) is what cleared this arm to run, and its §4 and §6 are what shaped it.

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not supplied to this
session — the sixth round in a row.

---

## 1. What NSFV is a control for, and the asymmetry that governs the whole round

NSFV is the Stage-0 probe STC. It runs **the same private SVC 239, the same CSA anchor, the same
`DOPOST` cross-address-space POST** as NSFS, and it has **no device at all** — so no CTCI read is
outstanding, ever. That is the variable. Two further properties make it a better control than it
was two rounds ago: it is **floorless by construction** (its wait set is `{console CIB ECB,
server_ecb}` with no timer ECB, so it needs no TCP workload to reach the floorless state), and
**the spin confound is gone from both sides** — NSFV has always reset its wake ECB
(`nsfv_server_ecb_reset` before its double-check drain, `src/nsfv.c`), and since 64-1 NSFS does
too (ADR-0022's annotation, ADR-0043 §3).

**Residual confounds, named here and not argued away:**

- NSFV's `server_ecb` lives in **CSA** and it **WAITs in supervisor state**; NSFS waits in
  problem state on a private key-8 ECB (ADR-0041's addendum, ADR-0043 §1). Whether that changes
  swap eligibility is **unknown**.
- Different loop structure, different wait set.
- **A third confound, which §0 §6 found and this round then measured on both sides: the
  workload.** See §6.

**Therefore the two outcomes are not symmetric, and §9's conclusion is written to that
asymmetry:**

> **A stall on NSFV would be strong** — it exonerates the outstanding device read, because there
> is no device. **No stall on NSFV is weak** — it is consistent with the device mattering, and
> equally with the supervisor-state WAIT, the wait set, or the workload mattering.

---

## 2. The detector had to be replaced, and the reason is 64-1's own

**64-1's conjunction detector cannot work here.** It requires a slot `req_state == PENDING`, and
64-1 recorded the consequence itself: *"with no client parked, `req_state` is never `PENDING`, so
this round's detector **cannot** fire in an idle window by construction."* Under this arm most of
the wall clock has no outstanding request — the executive answers in milliseconds — so the
detector would be structurally blind across nearly all of it and its silence would be worth even
less than 64-1's.

**The replacement is the client's own elapsed time, and it costs nothing.** MVS writes an
`IEFACTRT` line per job step carrying CPU and elapsed time. A `TSTSVC` step is 53 sequential
cross-address-space round trips; on a healthy executive it completes in a fraction of a second,
and **a step that takes minutes *is* the stall**. It is passive, needs no control-block offsets,
no `/.dm`, and — unlike a `F NSFV,STATS` — pokes nothing: a MODIFY POSTs the cib ECB that sits in
the executive's own ECBLIST.

### Validated live, before its silence is quoted as a result

| run | steps | result |
|---|---|---|
| `JOB02427` baseline | 1 | **CC 0000**, CPU `00:00:00.10` / elapsed `00:00:00.13` |
| `JOB02428` calibration | 250 | **CC 0000**; elapsed `0.09`×6 `0.10`×92 `0.11`×82 `0.12`×30 `0.13`×13 `0.14`×13 `0.15`×6 `0.16`×2 `0.17`×2 `0.18`×2 `0.19`×1 **`0.30`×1** |

250 readings, a real distribution, and an outlier at 0.30 s: the instrument demonstrably
**speaks**, which is what makes its later silence a null rather than an absence.

**A second, independent read path was validated too** — the anchor through `/.dm`:
`eye=NSFVANCR ver=3 flags=80000000 sascb=FF8B20`, and that `sascb` matches `NSFV001I NSFV READY
-- ANCHOR=00A8B7C8 ASCB=00FF8B20` exactly. Identity proven by the STC's own message, not by a
coherent-looking chain.

### And then it was validated against a **known positive**, which was free

The 250-reading baseline shows the instrument *speaks*. It does not show that it detects **this
phenomenon**. That came from a false alarm: the first detector query against `TSTRQXC` reported
**121 slow steps**, and they were not mine — `mvslog.txt` is cumulative and still holds every
earlier round's runs. One of them is this:

```
4000  3.51.20 JOB 2256  IEFACTRT A  /TSTRQXC /00:00:00.23/00:07:17.57/00000/TSTRQXCA
```

Elapsed **`00:07:17.57`**, ending MVS `3.51.20` = CEST `10:51:20`, so the step **started at CEST
10:44:02**. 64-0d recorded **stall 1 as 10:44:10 → 10:51:19**, 7 m 09 s, with `ASCBEJST` going
flat at ≈ 10:44:06 (`docs/nsf-64-0e-step0.md` §3).

**64-0d's stall 1 *is* a single `TSTRQXC` step with a seven-minute elapsed time.** Companion lines
from the same campaign: `00:03:24.33` and `00:03:25.62` on `TSTRQXCB`, and two `00:01:00.1x` steps
carrying `CC 00020` — the gate's own mutual timeout, which 64-0c and 64-0d record as
stall-disrupted.

So this detector is not merely able to speak; **it is proven to fire on the actual phenomenon, on
this stand, in a round where the phenomenon demonstrably occurred.** That converts both arms'
nulls from *"an instrument that can speak was silent"* to *"an instrument proven to fire on this
was silent"* — which is the strongest form the acceptance criterion can take, and it cost
nothing.

**The false alarm is also the caveat:** every detector query in this round is restricted to its
own window (MVS ≥ `8.53` for NSFV, ≥ `9.36` for NSFS). An unrestricted query over a cumulative
console log reports other rounds' stalls as if they were this one's.
**Instrument load, logged.** One 56-byte `/.dm` read per 60 s — against 64-1, where a 64-slot
poller every 2 s cost 17–20 % of the host. The step stream itself writes ~30 lines/s to the 1403
hardcopy; that is guest I/O added by the measurement and is recorded rather than passed over.

---

## 3. The arm

`S NSFV` at 15:53 (MVS `8.53.25`), STC 1485: `NSFV035I SVC ROUTINE LOADED AT 00A820C8`,
`NSFV034I SVC 239 STOLEN (OLD EP 0000CCC8, NEW EP 00A820C8, 40 FREE)`, `NSFV001I NSFV READY --
ANCHOR=00A8B7C8 ASCB=00FF8B20`.

Forty 250-step `TSTSVC` jobs (`JOB02429`…`JOB02468`) submitted 15:56:04–15:57:02 and queued on
the initiators so they run back to back: **10 000 steps × 53 = 530 000 round trips.**

**The arm turned out to be two client address spaces, not one, and that is measured rather than
assumed.** MVS had three initiators, so the queued jobs ran in parallel. Counting distinct load
jobs appearing in the same console second across the arm's 831 seconds:

| distinct client jobs in one second | seconds |
|---|---|
| 2 | **778** |
| 3 | 20 |
| 1 | 33 |

So the arm ran predominantly with **two concurrent client address spaces** — the same shape as
the `TSTRQXCA` + `TSTRQXCB` arm that reproduces on NSFS.

---

## 4. Result — no stall

| | |
|---|---|
| span | MVS `8.56.06` → `9.15.00` = **1 134 s** |
| steps | **10 001** (328 `CC 0000`, 9 925 `CC 0001` — see below) |
| requests | anchor `served` **0 → 543 356** |
| **continuity** | **max inter-step gap within the campaign = 1 second** |
| concurrency | 2 address spaces for 778 of 831 s |
| contention | `collisions` **0 → 57 078**; `exhausted` **0 → 0** |
| **detector** | **zero steps at or above 0.5 s elapsed.** Max **0.45 s** (2 steps), 0.40 s (2), 0.38 s (4) |
| `D A,L` mid-arm | `NSFV NSFV NSFV V=V` — **no `S`**; both client jobs `V=V` |
| **stalls** | **none** |

The only gaps above 5 s anywhere in the step stream are **47 s** and **52 s**, and both fall
*before* the campaign — they are the pauses while I submitted the calibration job and generated
the 40 job files. Inside the campaign the largest gap between consecutive steps is **one second**.

### `CC 0001` is expected, and it was measured rather than assumed

`TSTSVC` asserts `req.seq == base_seq + i` — a monotonic `served` **with no interleaving**. Two
concurrent clients break that by design. `NSFVP01` (`JOB02469`) was submitted **during** the arm
with `SYSPRINT` kept, precisely to see which assertion fails:

```
TSTSVC: SVC PROBE CLIENT DONE (base_seq=542983)
  PASS: client is UNAUTHORIZED (TESTAUTH FCTN=1) and does not self-auth
  PASS: router rc OK
  PASS: token round-trips byte-exact (STC echo = token+1)
                                                        -- CC 0001
```

**The transport is fine under concurrency; only the non-interleaving assertion fails.** The same
shape exists on the NSFS side: `TSTRQXC` SOLO asserts that `collisions` does **not** move, so it
too fails if a second initiator runs it concurrently. Worth knowing before either is used as
load.

---

## 5. The idle windows — and the one thing this round caught that is not a null

Three idle windows, each closed by a **latency probe**: a single 53-request `TSTSVC` step, which
is the only instrument that can say anything about an idle executive (with no client parked, no
`PENDING`-based detector can fire — 64-1's own conclusion).

| window | length | probe | elapsed | `served` |
|---|---|---|---|---|
| 1 | 260 s | `JOB02470` | **CC 0000, `00:00:00.13`** | `base_seq=543357` |
| 2 | 260 s | `JOB02471` | **CC 0000, `00:00:00.13`** — START and DONE in one console second (`9.24.21`) | `base_seq=543410` |
| 3 | **600 s** | `JOB02477` | **CC 0000, `00:00:00.12`** — likewise one console second (`9.34.23`) | `base_seq=543463` |

All three identical to the *fresh* baseline (0.12–0.13 s). The wake works after idle, on NSFV,
with no floor of any kind — the NSFV analogue of ADR-0043 §4's paired idle-and-service reading.

**Fewer and shorter than 64-1's, and stated as such:** three windows totalling **1 120 s**
against 64-1's seven totalling 2 356 s. What this round has instead is the **load** arm, where
64-1's detector could not run at all: counting the span over which the elapsed-time detector was
live, NSFV covers 1 134 s of load **plus** 1 120 s of idle = **2 254 s**.

### NSFV is swap-eligible, swaps out cleanly, and comes straight back

This is the round's positive finding, and it is worth more than the null.

| when | `D A,L` | OUCB (identity asserted: `OUCBASCB` == the ASCB it was chased from) |
|---|---|---|
| **during the load arm** (MVS `09.09.58`) | `NSFV NSFV NSFV V=V` — **no `S`** | resident |
| **idle** (`16:21:27`, `16:22:18`, `16:24:53`) | `V=V  S` — **swapped out** | `QFL=0C[OFF,OUT] SRC=06 RCTF=8E[TMNO,WAIT,OUT,TMLW] DSP1=80` |
| HTTPD, same instant (control) | healthy | `QFL=00[-] SRC=00 RCTF=00[-] DSP1=00` |

**That NSFV row is byte-for-byte 64-0d's *healthy, cleanly-swapped* address space** — its
`CLIENT7`: `RCTF=8E[TMNO,WAIT,OUT,TMLW] DSP1=80[NOQ] QFL=0C[OFF,OUT] SRC=06`. And it is the
**opposite of the stall signature**, which is `QFL=80[GOO] SRC=09 RCTF=00 DSP1=00` — a swap-out
that has *started and not finished*, with the address space still resident and still on the
dispatch queue.

A 60-second sampler caught **both edges** of the cycle:

| time | `QFL` | state |
|---|---|---|
| 16:22:35, 16:23:35 | `0C[OFF,OUT]` | cleanly swapped **out** |
| **16:24:36** — 15 s after window 2's probe | **`00[-]`** with `SRC=00 RCTF=00 DSP1=00` | swapped **in**, fully resident, **no transition bit standing** |
| 16:25:36 | `0C[OFF,OUT]` | **out** again |
| 16:26:37 → 16:33:59 (window 3) | `0C[OFF,OUT]` every sample | **out** for 8+ minutes continuously, then served in 0.12 s |

`QFL` is only ever `0C` (cleanly out) or `00` (cleanly in) — **never a transition bit left
standing**. On stalled NSFS it sat at `80[GOO]` for *minutes*. So the full cycle runs, repeatedly,
and every transition **completes**:

> swapped **out** → a request arrives → swapped **in** → 53 round trips served → swapped **out**
> again.

And it costs nothing: the 0.13 s probe elapsed **includes the swap-in**, so neither the swap-out
nor the swap-in is a cost on this path.

**The caveat, stated rather than buried:** NSFV swapped while **idle**; NSFS got stuck while
**loaded** (and, in every recorded case, spinning). Those are different SRM decisions, so this is
informative but **not a matched comparison on its own**. What matches it is the NSFS control arm
(§7). Offsets are 64-0d's live IFOX00-from-`SYS1.AMODGEN` derivation
(`docs/measurements/64-0d/cboff5-values.txt`), not from memory, and every reading asserts and
prints the OUCB identity — the `/.dm` trap of 64-0c is closed by identity, never by a
coherent-looking chain.

---

## 6. The workload confound, measured on both sides

Step §0 §6 predicted that NSFV had no client comparable to the one that reproduces, and named it
a **third residual confound** on top of the two §1 lists — larger than either, because it is a
property of the **workload**, which is the thing this round varies. Having now run it, the gap is
real but it is **not where I expected**, and both halves are measured rather than argued:

| | NSFS, the arm that reproduces | NSFV, this arm |
|---|---|---|
| client address spaces | 2 (`TSTRQXCA` + `TSTRQXCB`) | **2**, measured (778 of 831 s) |
| continuity | 64-1: ~10 s load, ~10 s gap, **50 % duty** | **max gap 1 s over 1 134 s** |
| requests | 64-1: 164 570 at 182/s | **543 356 at 479/s** |
| `collisions` | 670 052 across 45 rounds (64-1) | **57 078** — real contention |
| `exhausted` | 10 364 (64-1); 3 770 → 44 107 *inside one 64-0d stall* | **0 — the pool never saturated** |

So the arms match on **address spaces**, and this arm is far ahead on **continuity, rate and
count**. They differ on exactly one axis: **pool saturation**. `TSTRQXC`'s gate manufactures it by
pre-claiming slots 1..61 through the `FNSLOT` probe verb (step §0 §7); organic client traffic,
however hard it is driven, claims and releases slot 0 and never exhausts the pool. NSFV was
driven to 57 078 contended claims and **zero** exhaustions.

**That is the honest residual**, and it is narrower than §0 §6 feared: a quiet NSFV run is not
weakened by "the workload was light" — it was heavier and more continuous than the arm that
reproduces — but it *is* weakened by "the pool was never saturated". Whether saturation is part
of the provocation is untested, and closing that would need an NSFV pre-claimer (§0 §6 option b),
which is a test change.

---

## 7. The NSFS control arm — the same shape, with the device

The kickoff's §2 asks for both arms or the reason only one ran. Both ran. The control matters for
a reason step §0 §6 sharpened: **a quiet NSFV run means nothing if the NSFS control is also
quiet**, and 64-1 had already produced a quiet NSFS arm — but at a 50 % duty cycle with no
continuous stretch beyond ~10 s, which never entered the 40–90 s window every stall fired inside.
This arm gives NSFS the **same continuity** the NSFV arm had.

`P NSFV` at 16:35:05 (both STCs steal SVC 239, so NSFV goes first): `NSFV095I SVC 239 RESTORED`,
`NSFV036I SVC ROUTINE UNLOADED`, `NSFV011I`.

`S NSFS` at 16:35:13, STC 1487 — **and the device is up, which is the whole variable**:

```
NSF210I CTCI 0500/0501 UP DD SYS00003/SYS00005 MTU 1500
NSF211I INTERFACE LNK1 CUU 0500 UP
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 1073152
NSF042I SVC 239 STOLEN (EP 00A820C8)
NSF041I NSFS TRANSPORT READY -- ANCHOR=00A8B7C8 ECB=000BCF7C
NSF001I NSFS INITIALIZATION COMPLETE
```

`NSF055I` reads the **post-IPL** figure, so the round carries no CSA debt in and leaves none.

### The one deploy of the round, and its took-effect check

`make test-mvs --only TSTRQXC` — **the only deploy**, and it replaces TESTLIB (the NSFV five are
gone; see §8). `TSTRQXC` **batch CC 0 / TSO CC 0, 16 PASS / 0 FAIL**; no mid-chain `HTTP 500`.

The deploy-took-effect check is 64-1's own observable, and it is the one that says *which build*
of NSFS this control is against:

```
NSF812I WAKEECB=00000000 POSTED=N EVTPASSES=709 WAKEPOSTS=16 WPREG=Y SERVED=16
NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 TMRQ=0 EXHAUSTED=0 COLLISIONS=0 REAPED=0
```

**`POSTED=N` with a non-zero `SERVED` was impossible before the 64-1 reset**, so the deployed
module *is* the reset build; `WAKEPOSTS == SERVED` (16/16) is the post-64-1 counter semantics
exactly (ADR-0043, Consequence 3); and `EVTPASSES=709` after 16 requests is a loop that blocks,
not one that spins. The `F NSFS,STATS` that produced it is **logged as an intervention** — it
POSTs the cib ECB, which sits in the executive's own ECBLIST — and was issued **once, before the
arm, never as a detector.**

### The load

`TSTRQXC` with no PARM is its SOLO path: **8 `NSFRQE` round trips** carrying `XC_FN_UNKNOWN`, so
the dispatcher answers `EINVAL` and the load exercises the **full three-hop crossing** (CSA slot →
STC-private copy → dispatch → reply POST) **without allocating a single socket**. Baseline, single
client, from the mbt run: elapsed **`00:00:00.09`**, `CC 0000`.

120 jobs × 250 steps (`JOB02480`…`JOB02599`), submitted 16:36:47–16:42:16 and queued so they run
back to back: **30 000 steps × 8 = 240 000 round trips**, sized to match the NSFV arm's 1 134 s
rather than stopping at the ~375 s the first 40 would have given. STEPLIB matched to mbt's exactly
(`TESTLIB` + `NSF.LINKLIB`, `REGION=8M`).

---

### Result — no stall, and the detector *did* fire, on something else

| | |
|---|---|
| span | MVS `9.36.48` → `9.53.16` = **988 s** of continuous load |
| steps | **17 403** |
| requests | anchor `served` 16 → **139 240** |
| **continuity** | **max inter-step gap = 1 second** |
| contention | `collisions` 0 → 1 575; `exhausted` **0** |
| **detector** | **zero slow steps.** Max elapsed **`00:00:00.41`** (baseline 0.09) |
| NSFS `OUCB` under load | `QFL=00[-] SRC=00 RCTF=00[-] DSP1=00` — resident, no transition bit, matching HTTPD read at the same instant |
| `D A,L` under load | `NSFS NSFS NSFS V=V` — no `S` |
| **stalls** | **none** |

**Then the arm ended in a fault of mine, and the detector caught it.** At MVS `9.53.16` JES2
reported **`$HASP355 SPOOL VOLUMES ARE FULL`**, later `$HASP050 JES2 RESOURCE SHORTAGE CODE=SMFB`.
The cause is entirely mine: 120 jobs × 250 steps, each step allocating `SYSUDUMP DD SYSOUT=*`.
Two steps then show up as slow — the only two the detector flagged in this window:

```
10.01.26 JOB 2549 IEFACTRT S197 /TSTRQXC /00:00:12.80/00:08:09.21/00000/NSFSL070
10.01.26 JOB 2548 IEFACTRT S208 /TSTRQXC /00:00:12.82/00:08:09.20/00000/NSFSL069
```

**Three readings taken while it was happening discriminate it from a #64 stall, and they are why
the detector firing is a good outcome rather than a confusing one:**

1. **NSFS was healthy** — `QFL=00[-]`, `V=V` with no `S`, resident, read at 16:59:29. A #64 stall
   reads `QFL=80[GOO]` on the *executive's* address space.
2. **`inflight=0`** on the anchor, with `served` frozen. In every recorded #64 stall a client is
   **parked with a published request** and `inflight` is non-zero.
3. **The clients burned 12.8 s of CPU** across those 8 minutes. A client parked in the SVC
   routine's WAIT burns ~0. And the three `INIT` address spaces read
   `RCTF=8E[TMNO,WAIT,OUT,TMLW] DSP1=80[NOQ]` — cleanly swapped out **in a long wait**, waiting on
   JES2, not on NSF.

The cause is also simply named in the console log. So the detector fired on a real slowdown, and
the corroborating instruments identified it as something other than the phenomenon — which is
what a detector and its corroborators are supposed to do.

### The NSFS idle windows — and a difference from NSFV worth recording

| window | length | probe | result |
|---|---|---|---|
| 1 | 260 s | `JOB02600` | **CC 0000**, START and DONE both MVS `10.06.16` — one console second |
| 2 | **600 s** | `JOB02601` | **CC 0000**, both MVS `10.16.18` — one console second; `SOLO slot=0 coll 1575->1575`, so its single-client negative control held |

**NSFS does not swap out when idle, where NSFV does.** A 60-second sampler read `QFL=00[-]` on
**all eleven samples** from 17:05:42 to 17:15:46 — NSFS stayed **resident for ten minutes idle**,
while NSFV had gone to `QFL=0C[OFF,OUT]` within about a minute of going idle (§5).

The obvious candidate is the difference the round was built around: **NSFS holds an outstanding
CTCI read** — an EXCP that never completes on a silent link — and NSFV has no device at all.
**It is recorded as an observation, not a mechanism.** Nothing here shows *why* SRM treats them
differently, and it does not sit comfortably beside 64-0d, where SRM demonstrably *did* try to
swap NSFS out — with the device up and a read outstanding — and got stuck doing it. Those are
readings of different states (idle here, loaded there), and reconciling them is not this round's
to do. It is, however, a cheap and concrete question for whoever takes the next one.

### And a bug in my own filter, which nearly turned this into a false null

The first query reported **zero** slow steps in this window. It used
`awk '$2 >= "9.36"'` — a **string** compare — and `"10.01.26" < "9.36"` lexically, so **every line
after MVS 10:00 was silently dropped**, including both slow steps. Re-run with a numeric compare,
the window holds 17 500 steps and the two above. Every figure in this record was re-derived
numerically after the bug was found. It is CLAUDE.md §8.5 again, in my own instrumentation, and
the second instance in one round (§8) — recorded because the failure mode is a *quieter* null,
which is exactly the shape that gets believed.

---

## 8. Interventions, logged

Every action against the stand, with its time. The red line is that anything that *looks* passive
is logged too — 64-0d's `ext` cost a console switch whose residue was still live a round later.

| # | time (CEST) | intervention |
|---|---|---|
| 1 | 15:53:24 | `S NSFV` |
| 2 | 15:53:57 | submit `NSFVB01` — baseline, 1 step |
| 3 | 15:54:44 | submit `NSFVL01` — 250-step calibration |
| 4 | 15:56:04–15:57:02 | submit 40 × `NSFVLnn` — the NSFV load arm |
| 5 | 15:58:24 | submit `NSFVP01` — 1 step with `SYSPRINT` kept, **during** the arm |
| 6 | 16:15:38–16:34:24 | three idle windows (260 / 260 / 600 s), each closed by a latency probe (`JOB02470`, `JOB02471`, `JOB02477`) |
| 7 | 16:35:05 | `P NSFV` |
| 8 | 16:35:13 | `S NSFS` |
| 9 | 16:35:40 | `make test-mvs --only TSTRQXC` — **the only deploy of the round** |
| 10 | 16:36:20 | `F NSFS,STATS` — deploy-took-effect check, **once**, never as a detector |
| 11 | 16:36:47–16:37:40 | submit 40 × `NSFSLnn` — the NSFS control arm |
| 12 | 16:40:23–16:42:16 | submit 80 more, extending the control arm to match the NSFV arm's span |
| — | 16:53:16 (MVS `9.53.16`) | **incident, not an intervention:** `$HASP355 SPOOL VOLUMES ARE FULL`, caused by 11 and 12 (§7) |
| 13 | 17:01:04 | attempted to purge my own jobs to free spool (returned 0; **the maintainer cleared it**) |
| 14 | 17:01:55–17:16:18 | two NSFS idle windows (260 / 600 s), each closed by a latency probe (`JOB02600`, `JOB02601`) |
| 15 | 17:16:48 | `P NSFS` |

**Passive readings** (not interventions, but recorded): `/.dm` reads of the CSA anchor and of the
ASCB/OUCB, at a 45–60 s cadence — one 56-byte read and one ~0x144-byte read per sample. Against
64-1, where a 64-slot poller every 2 s cost 17–20 % of the host, this is negligible; it is
recorded because a measurement tool that costs part of the machine is a finding about the tool.
The load itself writes ~30 lines/s to the 1403 hardcopy, which is guest I/O added by the
measurement.

**No `ext`, no `i <cuu>`, no `/` console attention, no host `ping`** — this round injected
nothing into the guest. None was needed: no stall occurred to fire a ladder into.

### One tooling artefact, recorded rather than quietly fixed

An early `until` loop of mine reported *"campaign complete"* at 15:58:13, about one minute after
submission and roughly 17 minutes early. Its condition was
`zowe zos-jobs list … | grep -c ACTIVE`, and `grep -c` **exits non-zero when it counts zero**, so
`… || echo 99` produced the two-line string `0\n99` and the numeric test errored out rather than
comparing. The same bug silently disabled the `QFL=80` branch of a later monitor. Neither
affected a measurement — every figure in this record is read directly from the console log or
from storage, never from a loop's idea of when something finished — but it is exactly CLAUDE.md
§8.5's shape (*an operation whose absence is indistinguishable from its success*) in my own
instrumentation, so it is written down.

---

## 9. The three predictions, as written, and which the run supports

> **N(i)** — NSFV stalls. The outstanding device read is exonerated; the condition follows the
> transport or the address-space shape, and the next subject is what the swap-out transition is
> pending on.
>
> **N(ii)** — NSFV does not stall over a comparable window, NSFS (control) does. Consistent with
> the device mattering — and equally with the two residual confounds. A follow-up would have to
> vary one of those.
>
> **N(iii)** — neither stalls. The window was too short or the machine state differs; report it
> as a failed reproduction, not as a result. Six failures across five rounds is the standing base
> rate.

**The run supports N(iii).** Neither arm stalled:

| | NSFV (no device) | NSFS (device up, post-reset) |
|---|---|---|
| continuous load | **1 134 s**, max gap **1 s** | **988 s**, max gap **1 s** |
| requests | **543 356** | **139 240** |
| client address spaces | 2 | 2–3 |
| slow steps | **0** of 10 004 | **0** of 17 403 |
| `QFL=80[GOO]` | never | never |
| stalls | **none** | **none** |

**And it is reported as a failed reproduction, not as a result about the mechanism.** But one
half of N(iii)'s own disjunction can now be retired, and that is this round's contribution.

**"The window was too short" is no longer available.** It was the live explanation because
64-1's campaign never entered the 40–90 s sustained-load window every stall on record fired
inside — 904 s across 45 rounds at 20.1 s per slot, ~10 s of load then ~10 s of gap
(`docs/nsf-64-0e-step0.md` §8). Both of this round's arms are **continuous, with a one-second
maximum gap**, and each runs an order of magnitude past that window without interruption
(1 134 s and 988 s against 40–90 s). And the detector is not
merely able to speak: **it is proven to fire on the phenomenon** (§2 — 64-0d's stall 1 is a
`TSTRQXC` step of elapsed `00:07:17.57`), and it **did** fire in-round on a real slowdown, which
the corroborating instruments then identified as my spool exhaustion rather than a stall (§7).

**So what remains of N(iii) is "the machine state differs".** The one machine-state difference
that is documented, deliberate, and **shared by both arms** is that **the spin is gone** (64-1).
That does not make the spin the provocation — it makes it the candidate the evidence now points
at by elimination, and it is **untestable without reintroducing it**, which is a code change and
the maintainer's call.

### The asymmetry, restated — what a stall would have proved and what this quiet run does not

Stated in §1 before the run and unchanged by it:

- **A stall on NSFV would have been strong.** There is no device, so the outstanding CTCI read
  would have been exonerated outright.
- **No stall on NSFV is weak.** It is consistent with the device mattering, and *equally* with
  the supervisor-state WAIT, the wait set, or pool saturation mattering. **The device is not
  exonerated and is not implicated by this round.**
- **The control arm removes one alternative and adds none.** NSFS with its device up, post-reset,
  under 988 s of continuous two-client load, also did not stall — so "the NSFV arm was quiet
  because NSFV is special" has no support here either. Both arms are quiet under the same
  treatment.

**Nothing here narrows #64's mechanism.** What it narrows is the space of *explanations for the
nulls*: not the window.

## 10. What this round does not establish

- **It does not fix, or claim to fix, issue #64. #64 stays open.**
- **A quiet NSFV run is weak, by §1's asymmetry** — consistent with the device mattering, and
  equally with the supervisor-state WAIT, the wait set, or pool saturation mattering. Only a
  **stall** on NSFV would have been strong, and none occurred.
- **Pool saturation was never reached on either arm** (`exhausted` 0 on NSFV; the control arm
  drives SOLO-shaped traffic, not the pre-claiming gate). If saturation is part of the
  provocation, neither arm tests it. Closing that needs an NSFV pre-claimer — a test change
  (`docs/nsf-64-0e-step0.md` §6, option b).
- **The NSFV swap readings were taken while NSFV was idle**; NSFS's stalls happened under load.
  The control arm matches the load condition, but neither arm can match **the spin**, which 64-1
  removed from both sides and which is the one condition every stall on record shared.
- **It does not test the spin as a provocation and cannot** — reintroducing it is a code change
  this round's red lines forbid. What it does is remove the *last alternative explanation* for
  64-1's null on the continuity axis (§9).
- **Why NSFS stays resident when idle and NSFV does not is unexplained.** Eleven of eleven
  samples over ten minutes (§7). The outstanding CTCI read is the obvious candidate and is
  **not established**, and it sits awkwardly beside 64-0d, where SRM did try to swap NSFS out
  under load. Reconciling the two is open.
- **It did not read the OUCB swap chain** (`OUCBFWD` / `OUCBBCK` / `OUCBACT`), so *which* SRM
  queue an address space is parked on is still unread — 64-0d left the same gap.
- **One stand, one configuration, one day.** Six non-reproductions across five rounds is the
  standing base rate for the idle configuration, and this round adds to it rather than settling
  it.

---

## 11. Closing state

- `P NSFS` at 16:16:48 (MVS `10.16.48`): `NSF043I SVC 239 RESTORED`, `NSF044I NSFS TRANSPORT
  STOPPED`, `NSF011I NSFS SHUTDOWN COMPLETE` — **no `NSF054W`**, so the transport drained and
  nothing was retained. `P NSFV` earlier was equally clean (`NSFV095I` / `NSFV036I` / `NSFV011I`).
- **Zero dumps** for the whole round (`IEA995I` / `SYS1.DUMP` count 0).
- **No CSA debt in or out.** `NSF055I` read the post-IPL **1 073 152** at `S NSFS`, and both STCs
  stopped without taking a retain branch.
- **Stand left with NSFV and NSFS both stopped**, JES2/NET/TSO/UFSD/FTPD/HTTPD up, and **TESTLIB
  holding `TSTRQXC` alone** — the `--only` deploy replaced it, so the next round must re-deploy
  whatever it needs rather than assume the NSFV five are present (the b4 `S806` precedent, and
  64-1 left the same note in the other direction).
- **The JES2 spool needed clearing by the maintainer** after my load filled it (§7). Anyone
  repeating this shape should use `SYSOUT=DUMMY` for `SYSUDUMP` or run far fewer steps per job;
  30 000 job steps is a lot of spool.
- **`make test-host`: 2925 PASS / 0 FAIL, 27 tests**, before and after — a no-regression check
  only, and evidence of nothing else: the diff is documentation.
