# Issue #64, step 64-0f — the spin arm

**This step fixes nothing, and it is the only step in this investigation that
deployed a module known to be wrong.** Deliberately running a defective build is
not this project's habit and is not becoming one; it was authorised by the
maintainer for this round and this question, and the round ends with `main`'s
module back on the stand and verified live (§7).

Round: MVSCE on `mvsdev`, 2026-08-27. **MVS local = host CEST − 7 h**, measured
not assumed (STC01491 at MVS `12.23.07` / host `19:23:07`). Modules built from
`main` with the 64-1 reset reverted; **the revert is not in this branch** (§1).

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not
supplied to this session — the seventh round in a row (64-0, 0b, 0c, 0d, 64-1, 0e, 0f).

---

## 0. The correction that reshaped the round, and it is a correction to the kickoff

The kickoff's §1 rests on a premise this round could not adopt:

> "The nine stalls all occurred in **idle** windows. An arm 1 under continuous
> load would … stay inside a regime where no stall has ever been observed with
> or without it."

**Primary source says the reverse, in so many words.** `docs/nsf-64-0c-measurements.md`:

> "**An idle stack does not stall.** 64-0's two arms, 64-0b's prescribed window
> and this round's first 22 samples over ~8 minutes of post-workload idle all
> failed to reproduce — that is now **four** non-reproductions in the idle
> configuration."

and, on what the stalls *did* share, `docs/nsf-64-0e-step0.md` §6(c) and §8:

> "every stall on record fired **40–90 s into a round**, so [64-1's] campaign
> never reached the window in which stalls occur."

So idle is the configuration with **five** non-reproductions on record, and
sustained load is the one every stall on record occurred inside. Had this round
run §3.3 as written — deliberate idle windows as the sole measurement — F(i)
could not have fired, for two independent reasons: the arm is documented never
to reproduce, **and** the detector is structurally blind there (§3).

**Both shapes were therefore run, labelled, idle first** (so a stall in the load
arm could not contaminate the idle arm's swap data):

| | shape | what it can answer |
|---|---|---|
| **B1** | deliberate idle windows, `served` frozen | observable 2 only; the matched control against 64-0e's accidental 8-minute idle stretch |
| **B2** | **sustained** load, continuous, far past the 40–90 s onset band | observable 1 **and** 2 — the arm that can fire F(i) |

B2 is also the exact experiment 64-0e's own §6(c) asked for, in the one cell
that was missing: 64-0e ran this load shape with the spin **off** and got a
quiet 988 s. B2 runs the same shape with the spin **on**, so the revert is the
single variable across the two rounds.

This is a deviation from §3.3 and it is flagged as one.

---

## 1. The revert, verified before the build — and kept out of this branch

The change is the 64-1 reset, backed out at its single site: `g_wake_ecb = 0u;`
in `nsfsx_drain`. The assignment in `nsfsx_start` is **retained** — backing that
out too would be a different defect.

**Comment-stripped source diff** against `main` (489 → 488 lines, one deletion,
no additions):

```
405d404
<     g_wake_ecb = 0u;
```

**A first attempt at that diff was a false null and is recorded as one.** Using
`gcc -fpreprocessed -dD -E -P` to strip comments produced **zero lines from both
files**, so `diff` reported them identical — a clean-looking result from a
filter that had matched nothing. This is CLAUDE.md §8.5 in its purest form, and
it was caught only because the line count was checked before the diff was
believed. The check that stands is a stripper whose output was asserted
non-empty (489 and 488 lines).

**`cc370 -S`, which is what "instruction level" means for C here.** Both sides
compiled at the *same path*, so nothing is name-derived noise. The entire diff:

```
1904,1905d1903
<          L     2,=A(@V7)
<          MVC   0(4,2),=F'0'
```

`@V7` is a 4-byte static (`DC 4X'00'`), i.e. `NSFECB g_wake_ecb`, and the
survival of `nsfsx_start`'s assignment is asserted positively rather than
assumed: zero-stores to `@V7` go **2 → 1**, and total `@V7` references **10 → 9**.

**A load-module hash is not usable as evidence** and none was taken: 64-1
measured two builds of byte-identical source differing in exactly two bytes at
offset 10560 (a build timestamp).

**The revert is not committed.** This branch is docs-only; the one-line change
and its `-S` diff are recorded here instead, so a docs-only PR cannot land a
defective module.

---

## 2. Instruments

### `ASCBSTOR` was derived, not remembered — and it was not already on record

The red lines forbid a control-block offset from memory, and `ASCBSTOR` was
**not** in 64-0d's `cboff5-values.txt` / `cboff6-values.txt`. It is this round's
instrument, so it was derived first, by IFOX00 over `SYS1.AMODGEN` (job
`CBOFF7`, `docs/measurements/64-0f/CBOFF7.jcl`):

```
000000 002C   ASTOR    DC    AL2(ASCBSTOR-ASCB)
000002 0040   AEJST    DC    AL2(ASCBEJST-ASCB)
000004 0070   ASWCT    DC    AL2(ASCBSWCT-ASCB)
000006 0024   AASID    DC    AL2(ASCBASID-ASCB)
000008 006C   AASXB    DC    AL2(ASCBASXB-ASCB)
00000A 0020   ACPUS    DC    AL2(ASCBCPUS-ASCB)
```

The job ends **CC 0008** for one reason, stated so it is not mistaken for a
defect in the values above: `ASCBLEN` is not a symbol this macro defines, so
that one `DC` took `*** ERROR ***` and assembled `0000`. It is not used. Every
other offset assembled clean and each is quoted above with its object code.

The OUCB set and `ASCBRCTF`/`ASCBFLG1`/`ASCBDSP1`/`ASCBOUCB`/`ASCBOUXB` are
cited from 64-0d's `CBOFF5`, not re-derived.

### Two cadences, because the expensive read is not the one that matters

`docs/measurements/64-0f/swapwatch.py`. The 64-slot scan is what cost 64-1 a
contaminated 17–20 % CPU sample; a single ASCB read is nearly free. So the ASCB
— carrying `ASCBSTOR`, `EJST`, `RCTF`/`DSP1` — is read **every 5 s**, and the
OUCB flags are added **every 45 s**. The fast cadence is what materially narrows
the fast out-and-back gap 64-0e could not close.

**Identity, never internal consistency:** every full sample re-proves the
address space by OUCB eyecatcher **and** `OUCBASCB == ASCB` **and**
`ASCBASID == ASID`. A sample failing any of those prints `BADID` and is excluded
from the distinct-`ASCBSTOR` count. Across the whole round: **badid = 0**.

### Every query was run against data that must match, and watched to match

- **`anchor.py`** (the `served` reader, `/.dm` only — no MODIFY, which would
  POST the cib ECB in the executive's own ECBLIST) was cross-checked against the
  STC's own `NSF813I`: `served=33 inflight=0 exhausted=0 collisions=0` from
  `/.dm`, against `INFLIGHT=0 EXHAUSTED=0 COLLISIONS=0 SERVED=33` from the
  MODIFY. Identical.
- **`swapwatch.py`** was run for 70 s before being armed, and its OUCB identity
  proof observed to **pass** (flags printed, not `IDENTITY FAILED`).
- **`pendwatch.py`** validates the stall detector's load-bearing conjunct
  **in band** — see §4.

---

## 3. Deploy, and the checks that prove which module was live

`P NSFS` was not needed — `D A,L` at 18:26 showed **no NSFS or NSFV running**.
`make modules` + `make deploy` completed with no mid-chain
`HTTP 500 … DELETE /restfiles/ds/NSF.LINKLIB` (§5's signature), and
**`S NSFS` at host 19:23:06** brought up STC01491:

```
NSF210I CTCI 0500/0501 UP DD SYS00003/SYS00005 MTU 1500
NSF211I INTERFACE LNK1 CUU 0500 UP
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 1073152
NSF042I SVC 239 STOLEN (EP 00A820C8)
NSF041I NSFS TRANSPORT READY -- ANCHOR=00A8B7C8 ECB=000BCF7C
```

**Segment A** — `TSTRQXM`, whose purpose is to reach the post-workload state
(and, per ADR-0034, to let the TCP timers cancel and drain the STIMER):
**batch CC 0**. Its TSO re-run is CC 1 **by design** — the one-shot host
listener was consumed by the batch run, so `CONNECT` and its dependent `CLOSE`
fail (the TSTTCPW precedent; batch is the gate). 50 PASS / 2 FAIL over both.

**The deploy check, taken after the workload so `SERVED` is non-zero:**

```
NSF812I WAKEECB=40000000 POSTED=Y EVTPASSES=529481 WAKEPOSTS=526503 WPREG=Y SERVED=33
NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 TMRQ=0 EXHAUSTED=0 COLLISIONS=0 REAPED=0
```

**`POSTED=Y` together with `SERVED=33`** is the proof: on the reset build that
combination is impossible, because the drain clears the word before every WAIT.
Its complement is ambiguous and is not used as a check. `WAKEPOSTS` tracking
`EVTPASSES` at a near-constant offset (2 978) is the latched semantics 64-1
described, and is a second, independent tell of the same thing.

### The floor was measured, not inherited

The advisor's expectation was that this could only be asserted by construction,
because with the ECB latched the executive never blocks and the `EVTPASSES`
*rate* cannot show whether the heartbeat drained. It did not have to be
inherited: **`TMRQ=0`** is a direct read of the timer-queue depth, and ADR-0034's
invariant is *queue empty ⟺ STIMER disarmed ⟺ `g_armed == 0`*. The floor is
gone, measured on this build, in this round.

---

## 4. The detector, and why B1 cannot speak for observable 1

`stallwatch.py` (64-1's, unchanged) tests a **conjunction**: `ASCBEJST`
bit-identical **AND** at least one slot `req_state == PENDING` **AND** `served`
frozen. The reset destroyed the plain EJST-flat detector — a correctly idle
executive with no floor reads exactly as flat as a stalled one — and the
surviving discriminator is a published request that stays PENDING.

**In a truly idle window no client is submitting, so no slot is ever PENDING,
and the detector is structurally silent no matter what the executive does.**
Reporting B1's silence as "no stall" would be exactly the
absence-indistinguishable-from-success failure CLAUDE.md §8.5 exists for. B1's
silence is therefore reported as **blindness, not quiet** — and §0 gives the
second, independent reason B1 could not have fired: idle is the configuration
with five non-reproductions on record.

**B2 makes the detector self-validating in band.** Rather than a separate census
(64-1's approach), `pendwatch.py` watches slots 0–3 *during the arm whose
silence is being quoted*, and observes `PENDING` appear and clear while `served`
advances. That is stronger evidence the read path works than an out-of-band
census, because it is the same path, the same anchor and the same arm.

---

## 5. What the defective build actually cost, and one number that does not match 64-1

`ASCBEJST` is the guest's own accounting for this address space, so it measures
the spin from inside, independent of the host.

| when | EJST delta | over | = |
|---|---|---|---|
| **before any request** (ECB never posted, no spin) | 0.157 CPU-s | 46 s | **0.34 %** |
| **after segment A** (ECB latched `POSTED`) | 53.6 CPU-s | 47 s | **114 %** |
| B1 idle window 1 | 53.9 CPU-s | 47 s | **114.6 %** |

and from the host side, sampled as a `/proc/<pid>/stat` **delta** (never `ps`
`pcpu`, which is a since-boot average — 64-1's trap):

```
hercules CPU during B1 idle: 112.7% of a host core
```

The two agree, and they say NSFS alone accounts for essentially all of the
guest's CPU consumption while doing nothing.

**They do not agree with 64-1, and that is reported rather than explained
away.** 64-1 measured the same defective source at **25.9–30.5 %** of a host
core (states B and C). This round measures **112.7 %** on a stand whose host
load average was 0.12 at round start. Nothing in this round can attribute the
difference: the sampler here polls HTTPD at 0.2 reads/s against 64-1's
contaminating 32 reads/s, so instrument load runs the *wrong* way to explain it.
It is recorded as an unexplained discrepancy between two rounds measuring one
build.

---

## 6. The predictions, quoted as written, and which one the run supports

The kickoff's §4, verbatim:

> - **F(i) — arm 1 stalls in an idle window**, where 64-0e's matched idle
>   stretch did not. The spin is the provocation; 64-1 is retroactively the fix
>   for #64, found from the far end … Arm 2 then separates spin from read.
> - **F(ii) — no stall, but `ASCBSTOR` shows SRM beginning swap-outs of NSFS**
>   where the spin-off arm showed none. The spin changes swap eligibility
>   without producing a stall …
> - **F(iii) — no stall and no transition observed at all.** The spin is not the
>   provocation either, and the question returns to what began the swap-out in
>   the nine recorded stalls, with the spin eliminated alongside idleness, load
>   and the device.

> None of the three has fitted in any round so far, and saying so has been the
> useful outcome every time. If the readings fit none, report them raw.


---

## 7a. B1 — the idle arm: healthy, and informative precisely because it is

Two deliberate windows, nothing touching the stack, `served` **frozen at 33
across both** (read through `/.dm`, never a MODIFY):

| window | start | end | length | `served` |
|---|---|---|---|---|
| B1-W1 | 19:30:32 | 19:37:12 | **400 s** | 33 → 33 |
| B1-W2 | 19:37:43 | 19:44:23 | **400 s** | 33 → 33 |
| **total** | | | **800 s** | frozen throughout |

Sampler output over the arm (`summarise.py`, positive-controlled — it was run
first against a window known to contain samples and watched to find them):

```
B1-total   19:30:32-19:44:23  DISTINCT ASCBSTOR = 1  ['0FAF3C00']
           full samples=17  fast samples(cum)=181  badid=0
           QFL {'00[-]': 17} | RCTF {'00[-]': 17} | CPUS {'1': 17}
```

**Observable 2 for B1: distinct `ASCBSTOR` = 1 — no completed swap cycle, and
`QFL=00` on every sample means none was even attempted.** `CPUS=1` on all 17:
the address space held a CP continuously for the whole 800 s.

**This is the round's cleanest mechanistic finding, and it is not a null.** A
spinning address space is the *least* swap-eligible thing on the system — it is
always dispatchable, always running, and SRM never considers it. Against
64-0e's matched idle stretch (spin **off**, NSFS resident 12/12) the answer is
the same, so **on the swap axis the spin makes no difference while idle.** F(ii)
predicted the opposite and B1 is where it fails.

**B1 cannot speak for observable 1 and its silence is not quoted as quiet** —
§4: with no client submitting, no slot is ever `PENDING`, so the detector is
structurally blind, and §0 gives the independent second reason (idle is the
configuration with five non-reproductions on record).

---

## 7b. B2 — **a stall, reproduced and fully measured** (but not where F(i) put it)

**The stall reproduced at the first attempt, on the spin build.** B1 closed at
19:44:23; `make test-mvs --only TSTRQXC` began at ~19:44:30; the last healthy
sample is **19:44:30** and the first stalled sample is **19:45:16**.

**What was running in that 46-second interval is stated rather than
characterised, because it is not what this round set out to run.** It was a
`make test-mvs` cycle — cross-build, XMIT upload, `RECEIVE` into TESTLIB, then
the job — **not** a steady request stream. The 12 `B2LOAD` jobs prepared for the
sustained-load arm **had not been submitted yet** (§B2b). The arithmetic happens
to coincide with the documented 40–90 s onset band, but the *condition* may not:
that band is 40–90 s into a **sustained-load round**, and this was a deploy
burst whose first client execution falls somewhere inside the interval. **Band
membership is therefore not asserted here** — the interval and its contents are
reported, and §B2b runs the comparison that was actually intended.

Consequently **64-0e's 988 s NSFS arm is not a matched control for this onset**,
and the "single variable is the revert" claim is not available for it. It is
available for §B2b.

### It announced itself as a *tooling* failure, and that is worth keeping

The first sign was `make test-mvs` reporting

```
TSTRQXC    FAIL NO RC     FAIL NO RC
job MBTTEST JOB02610  | assertions (batch+tso): 0 PASS, 0 FAIL
```

which reads exactly like a broken test. It was not: CLAUDE.md §5 records that
mbt's job-poll times out and reports `NO RC` while the job is still running, and
`zowe zos-jobs view job-status-by-jobid JOB02610` returned **`status: ACTIVE`**.
The client was not failing — it was **parked**, which is the stall.

### The readings, taken before any intervention

Identity proven, never inferred (`asread.py`, `OUCBASCB=FF8B20 IDENTITY OK`):

```
ASID=000B CPUS=0 EJST=0000046D6D338C80 SWCT=4237 ASCBSTOR=0FAF3C00
RCTF=00[-]  FLG1=80[-]  DSP1=00[-]
OUCB @FE7530 eye=OUCB OUCBASCB=FF8B20 IDENTITY OK
     QFL=80[GOO] SFL=00[-] EFL=04[-] UFL=00[-] CFL=00[-]
     SRC(swapout reason)=09 SWC=0 NDS(dontswaps)=0 DMN=1
     FWD=04A318 BCK=FE4D58 ACT(action q)=000000 ACN=0000
OUXB @FE7498 eye=OUXB OUXBRSW(REQSWAP ecb)=00000000
```

**This is 64-0d's stall signature, field for field:** `QFL=80[GOO] SRC=09
RCTF=00 DSP1=00` — an address space stuck part-way through an MVS swap-out.
`ASCBSTOR` is **unchanged**, which is the same statement from the other side:
the cycle never completed. `CPUS=0`, where every one of B1's 17 samples read
`CPUS=1`.

### The detector fired on the phenomenon, in this round, in band

```
19:50:11 *** STALL: EJST flat, served=33 frozen, 1 slot(s) PENDING for 22s ***
19:50:11       SLOT0  PENDING reply_ecb=809DE5F0 ascb=FD0F18 asid=0008 xfunc=6
19:50:11       inflight=1 exhausted=0 collisions=0
19:51:20     still stalled, 91s (served=33)
```

The full conjunction — EJST bit-identical **and** a slot `PENDING` **and**
`served` frozen — satisfied by the live phenomenon. This is a stronger
validation than any census: the detector is not merely *able* to speak, it is
**proven to fire on the thing whose absence it would otherwise be reporting**.
`SLOT0`'s client is ASCB `FD0F18` ASID `0008` — the `MBTTEST` batch job — parked
on `reply_ecb=809DE5F0` with its wait bit set.

### It cleared by itself after ~12 minutes — and `ASCBSTOR` moved

**This is the reading the round was built to take, and it could not have been
taken any other way.** Had the STC been stopped to "recover" the stall, the
answer would have been destroyed.

```
19:56:59 STOR=0FAF3C00 EJST=0000046D6D338C80 CPUS=0 ... QFL=80[GOO] SRC=09 SWC=0000
19:57:15 *** ASCBSTOR CHANGED 0FAF3C00 -> 0FC26C00 -- a swap cycle COMPLETED ***
19:57:15 STOR=0FC26C00 EJST=00000473DFC21080 CPUS=0 FLG1=82 ... QFL=00[-] SRC=00 SWC=0001
19:57:22 *** ENDED after 299s -- served=49 (was 33) EJST moved=True ***
```

- **`ASCBSTOR` 0FAF3C00 → 0FC26C00.** The segment-table origin was reassigned,
  which is the retrospective signature of a **completed** swap cycle — the exact
  thing 64-0e could not see and the reason this field was made the instrument.
  **Distinct `ASCBSTOR` for the B2 arm = 2.**
- **`OUCBSWC` 0 → 1**, and it reads `0000` on all 38 prior samples and `0001`
  on this one. That is an **independent** corroboration, from a different
  control block, that **exactly one** swap cycle completed — not a fast
  out-and-back series, one.
- `QFL` `80[GOO]` → `00[-]`, `SRC` `09` → `00`: the transition retired.
- `served` 33 → 49, and **`JOB02610` ended `CC 0000`**. The client was never
  failing; it was parked, and it completed normally once the address space came
  back.

**Duration: onset between 19:44:30 and 19:45:16, clear at 19:57:15 — ~12.0 to
12.7 minutes.**

### This answers a question 64-0d left open, and it revises its wording

64-0d described the address space as *"stuck part-way through an MVS swap-out"*
and #64 was retitled *"tasks non-dispatchable while `OUCBQFL = 80`"*. Both are
accurate for the window in which they were observed. **This round measures the
transition COMPLETING** — so at least this instance was not *stuck*, it was
**very slow**: about twelve minutes in `QFL=80[GOO]`, then a normal swap-in at a
new segment-table origin and immediate resumption of service.

Whether the nine earlier stalls also eventually completed is not established
here — 64-0d's were observed within their windows and terminated by
intervention. But "stuck" should be read as "not yet observed to complete"
rather than "never completes", and this round supplies one that did.

---

## 7c. B2b — the sustained-load arm that was actually intended

§B2's onset coincided with a `make test-mvs` deploy burst, so this arm runs the
comparison that was meant: a steady request stream against the same instance,
**no deploy activity**, on the same defective build.

### It failed silently on the first attempt, and `served` is what caught it

12 jobs were submitted and the anchor then read `served` **frozen at 49 with all
64 slots FREE** — which means no client was submitting at all. Every job had
ended `JCL ERROR`:

```
IEF602I EXCESSIVE NUMBER OF EXECUTE STATEMENTS
```

MVS 3.8j caps a job at **255 EXEC statements** and the generated jobs had 400.
**Had `served` not been checked, this arm would have been reported as a quiet
load arm** — a null produced by a load that never ran. That is the same failure
shape as §1's false-null diff and as the deploy-burst confusion, three in one
round.

The fix was 250-step jobs, and the lesson was applied rather than just noted:
**one job was submitted alone and its effect verified before the rest went in** —
`served` **49 → 753 in 25 s**.

### The arm, and the detector validated inside it

14 × 250 steps, each step 8 cross-AS requests. Under load the anchor shows

```
pendwatch: 180 slot reads over 120s -> {'PENDING': 3, 'FREE': 148, 'DONE': 29}
pendwatch: served 3329 -> 15534 (delta 12205)      ~102 requests/s
VALIDATION: PENDING observed = YES
```

so the detector's load-bearing conjunct is **observed to fire and clear in
band**, on the same anchor and the same read path whose silence is being
quoted — while `served` advances at ~102 requests/s.

### B2b did not stall, and that is a result about the provocation

| | requests | duration | stall |
|---|---|---|---|
| **§B2** (deploy burst after 800 s idle) | `served` 33 at onset | ~12 min stalled | **YES** |
| **§B2b** (sustained load) | `served` 49 → **28 049** | ~15 min at up to ~102/s | **no** |

**28 000 requests did not provoke it; 33 did.** So sustained request load is not
the provocation, and the sentence this round was originally going to write —
"the arm that can fire F(i) is the load arm" — is wrong in the way that matters,
even though the stall did fire in the interval labelled B2.

---

## 7d. B2c — the exact provoking cycle, repeated: **no stall**

If the deploy burst were the provocation, repeating it should reproduce. It was
repeated, on the same instance and the same defective module, immediately after
B2b:

```
TSTRQXC    ok CC 0        ok CC 0
job MBTTEST JOB02640  | assertions (batch+tso): 16 PASS, 0 FAIL
```

**Clean.** So the stall is **not reproducible on demand**, and the honest count
for this round is **one onset in three deliberate attempts** at conditions that
included the one that produced it.

That is what makes the specified **arm 2 unable to do its job**. Its purpose was
to separate spin from read by seeing whether a device-less instance stalls; a
quiet arm 2 attributes nothing when the phenomenon fires roughly one time in
three, and it is confounded besides — taking the device offline also removes the
CTCI read subtasks and two TCBs from the address space, so "quiet" would be
consistent with the read mattering, the TCB shape mattering, or nothing
mattering. **Arm 2 was therefore deliberately not run**, on the maintainer's
decision, in favour of testing the one condition the data actually points at
(§B2d).


---

## 7e. B2d — the one candidate condition the data pointed at: **no stall**

The only stall followed a specific sequence: STC start → a small workload →
**833 s of idle** → a deploy burst. B2b (heavy load *then* a burst) and B2c (a
burst right after load) were both clean, so the *idle-then-activity* transition
was the one untested candidate. On the maintainer's decision this was run in
place of the specified arm 2 (§B2c explains why arm 2 could not do its job).

Matched deliberately to the stall's own shape: **840 s idle**, `served` frozen
at 28 065, nothing touching the stack, then the identical `make test-mvs --only
TSTRQXC` cycle.

```
TSTRQXC    ok CC 0        ok CC 0
job MBTTEST JOB02642  | assertions (batch+tso): 16 PASS, 0 FAIL
```

**Clean**, and `QFL=00` / `CPUS=1` throughout the idle phase.

### The count, stated plainly

| # | attempt | condition | stall |
|---|---|---|---|
| 1 | **B2** | deploy burst after 800 s idle | **YES**, ~12 min |
| 2 | B2b | 28 000 requests, sustained | no |
| 3 | B2c | the identical burst, after load | no |
| 4 | B2d | the identical burst, after 840 s idle — **the same shape as #1** | no |

**One onset in four deliberate attempts, including one attempt built to match
the successful one as closely as the stand allows.** The provocation is
therefore **not identified by this round**: not request rate, not the deploy
cycle, not the idle-then-burst sequence. What #1 had that #4 did not is not
visible in anything measured here.

---

## 7f. B1 and B2 together, which is more than either

The two arms differ in one thing — whether client address spaces were competing
— and they separate cleanly:

| arm | spin | other work | `CPUS` | `QFL` | `ASCBSTOR` | outcome |
|---|---|---|---|---|---|---|
| **B1** (800 s, 2 windows) | **on** | none, `served` frozen | **1** on 17/17 | `00[-]` on 17/17 | 1 distinct | resident throughout; **SRM never started a swap-out** |
| **B2** (onset ~19:44:50) | **on** | a client executing | **0** | **`80[GOO]`** | 1 distinct | **stuck mid-swap-out**, ≥ 10 min |

**A spinning address space, left alone, is the *least* swap-eligible thing on
the system** — it holds a CP continuously and SRM never even considers it. That
is B1, and it is why the idle configuration has never reproduced in six rounds:
there is nothing to swap *for*. The spin does not make NSFS swap-prone by
itself.

What B2 adds is a second address space wanting resources. Then SRM does begin a
swap-out of NSFS — and on the spin build, this once, it stuck.

So the first reading was that the provocation is a **conjunction** — spin plus a
competing address space. **§B2b refutes that as a sufficient condition and the
correction is kept here rather than quietly edited away:** B2b ran the spin
build with competing address spaces and **28 000 requests** and did not stall.

What survives is narrower and is stated as such:

- **spin alone** (B1, 800 s idle) → SRM never even *attempted* a swap-out —
  `CPUS=1`, `QFL=00` on 17/17. This is solid, and it explains six rounds of idle
  non-reproduction without calling the idle arm a sampling failure: there is
  nothing to swap *for*.
- **spin + heavy sustained load** (B2b, 28 000 requests) → no stall.
- **spin + a deploy burst after a long idle** (B2) → `QFL=80[GOO]`, ~12 min.
- **the same deploy burst after heavy load** (B2c) → no stall.

So the one thing all of this establishes about the provocation is that **it is
not request rate.** The stall fired at `served = 33`; B2b served 28 000 without
it.

---

## 8. Which prediction the run supports: **none of the three, as written**

The kickoff anticipated this outcome explicitly — *"None of the three has fitted
in any round so far, and saying so has been the useful outcome every time. If
the readings fit none, report them raw."*

| | as written | what happened |
|---|---|---|
| **F(i)** | "arm 1 **stalls in an idle window**, where 64-0e's matched idle stretch did not" | **A stall occurred — but NOT in an idle window.** B1's two 400 s idle windows were clean, `QFL=00` on 17/17, `served` frozen and nothing wrong. The stall arrived with activity, after the idle ended |
| **F(ii)** | "**no stall**, but `ASCBSTOR` shows SRM beginning swap-outs" | Half right in the wrong combination: `ASCBSTOR` **did** move — but there **was** a stall, and the movement was the stall *ending* |
| **F(iii)** | "**no stall and no transition** observed at all" | Both a stall and a completed transition were observed |

**The closest is F(i), and the difference is not a quibble.** F(i)'s premise was
that idle windows are where stalls occur; §0 shows primary source says the
opposite, and B1 then measured 800 s of spinning idle as completely healthy —
`CPUS=1`, no swap-out even attempted. So the round supports *"a stall occurred
on the spin build where the spin-off arms were quiet"* while **refuting the
location F(i) assigns it.**

**And F(i)'s consequences do not follow.** F(i) concluded that if it fired,
"the spin is the provocation" and "64-1 is retroactively the fix for #64". §B2b
and §B2c block both: 28 000 requests on the spin build did not stall, and the
exact provoking cycle repeated did not stall. One onset in three attempts is not
a provocation identified — it is a phenomenon caught once, with good readings.

### What the round does establish, at the strength it carries

1. **A reproduction with complete readings**, identity-proven, carrying 64-0d's
   signature field for field (`QFL=80[GOO] SRC=09 RCTF=00 DSP1=00`, `CPUS=0`,
   EJST bit-identical).
2. **The first measured COMPLETION of the transition** — `ASCBSTOR`
   `0FAF3C00 → 0FC26C00` with `OUCBSWC 0 → 1` corroborating from a second
   control block. ~12 minutes in `QFL=80[GOO]`, then a normal swap-in and
   immediate resumption; the client's job ended **CC 0000**. #64's "stuck" is
   better read as **"very slow, and observed to finish"**.
3. **Observable 2, per arm:** B1 (idle, spin) **distinct `ASCBSTOR` = 1** — SRM
   never attempted a swap-out of a spinning address space, which is a mechanism
   for why idle has never reproduced. B2 **= 2**, the one completed cycle.
4. **The provocation is not request rate** (§B2b), and it is not the deploy
   cycle by itself (§B2c).

---

## 9. What this round does NOT establish

**A stall was reproduced and fully measured. That is ONE onset in FOUR
deliberate attempts, and the list below is not hedging — each item is a specific
thing a reader might otherwise take from the paragraphs above.**

- **It does not identify the provocation at all.** The onset coincided with a
  `make test-mvs` deploy burst, and every attempt to reproduce that — sustained
  load (§B2b), the identical burst after load (§B2c), the identical burst after
  a matched 840 s idle (§B2d) — was clean. Request rate is excluded (33 vs
  28 000); nothing else is established.
- **It does not make 64-1 the fix for #64.** 64-1 removed the spin; if the spin
  is the provocation, then it plausibly removes the stall as a side effect. But
  that inference runs from one positive against a set of prior negatives
  collected under different conditions, and 64-1's own arm is dated (§0). #64
  stays open. Whether NSF should mitigate an MVS condition at all, and in what
  form, is the maintainer's call and is not a change made on the way past.
- **It does not show the spin *causes* the stuck swap-out.** The measured chain
  ends at `QFL=80[GOO]` — inside MVS, in fields NSF does not write and cannot
  see. 64-0d established that and this round does not advance it.
- **It does not rule out that the stall was always reachable and simply rare.**
  The base rate of failed reproductions across this investigation is high, and a
  single success does not measure a rate.
- **The 112.7 % vs 26–30 % CPU discrepancy against 64-1 is unexplained** (§5),
  and it is a difference in the *same build* between two rounds. If the spin's
  cost differs by 4× between rounds, the stand differed in some way neither
  round captured — which is a caution about every cross-round comparison here,
  including this one's.
- **`SWCT` is reported but not leaned on.** It climbs during workload and
  freezes when the executive does, which is not the behaviour of a swap counter;
  64-0d's logs show the same. `ASCBSTOR` is the instrument, and it did not move.

---

## 10. Restore — the acceptance item, done before this document was finished

| | |
|---|---|
| `P NSFS` | **20:37:55** — `NSF011I NSFS SHUTDOWN COMPLETE`, `$HASP395 NSFS ENDED` |
| source | `src/nsfsx.c` copied back to `main`; **`git diff --stat` empty** — verified, not assumed |
| build + deploy | clean, **no mid-chain `HTTP 500`** on `DELETE /restfiles/ds/NSF.LINKLIB` |
| `S NSFS` | 20:38:28, STC01493, anchor `00AAF7C8` |
| workload | `TSTRQXC` **ok CC 0 batch AND TSO**, 16 PASS |

**The check, quoted:**

```
NSF812I WAKEECB=00000000 POSTED=N EVTPASSES=345 WAKEPOSTS=16 WPREG=Y SERVED=16
NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 TMRQ=0 EXHAUSTED=0 COLLISIONS=0 REAPED=0
```

**`POSTED=N` with `SERVED=16` non-zero** — the reset build is live. Two
independent corroborations in the same line: `EVTPASSES=345` where the spin
build read 529 481, and **`WAKEPOSTS == SERVED` exactly (16 = 16)**, which is
64-1's wake-event semantics rather than the latch that tracked `EVTPASSES` at a
constant offset.

**Wall clock the defective build was live: 19:23:06 → 20:37:55 = 1 h 14 min 49 s.**

---

## 11. Acceptance

| # | item | |
|---|---|---|
| 1 | `make test-host` | **2925 PASS / 0 FAIL, 27 tests** — no-regression only; `src/nsfsx.c` is MVS-only, so this is evidence of nothing else |
| 2 | revert re-diffed vs `main` at instruction level **before** the build | §1 — comment-stripped diff (one deletion) **and** `cc370 -S` (exactly two instructions), plus the positive check that `nsfsx_start`'s store survived |
| 3 | deploy check + restore check quoted | §3 `POSTED=Y … SERVED=33`; restore `POSTED=N … SERVED=16` |
| 4 | idle windows: count, lengths, total, `served` frozen | §B1 — 2 windows × 400 s = **800 s**, `served` 33 → 33 across both. Plus B2d's 840 s |
| 5 | both observables per arm | §B1 distinct `ASCBSTOR` **= 1**; §B2 **= 2** (the completed cycle), corroborated by `OUCBSWC 0 → 1` |
| 6 | the three predictions quoted, and which fired | §6 quotes them; the verdict is **none as written** |
| 7 | detector read path validated before its silence is quoted | it **fired on the live phenomenon** (§B2), and `pendwatch` observed `PENDING` appear and clear in band under load (§B2b) |
| 8 | wall clock the defective build was live | **1 h 14 min 49 s** |
| 9 | PR separates host-verified from live | the PR body does |

### Deviations from the kickoff, each with its reason

1. **§3.3's idle-only measurement was not the sole arm.** Primary source
   contradicts the premise that stalls occur in idle windows (§0). Both shapes
   were run, idle first.
2. **Arm 2 (§3.5) was deliberately not run**, on the maintainer's decision: its
   purpose is unattainable against a phenomenon that fires one time in four, and
   it is confounded (§B2c). B2d was run in its place.
3. **The `B2LOAD` sustained-load arm was added** (§B2b) and the deploy-burst
   repeat (§B2c) and idle-then-burst (§B2d) after it — none were in the kickoff;
   all three exist because the first onset did not occur under the conditions
   the round had prepared.
