# Issue #64, step 64-3-1 — `SYSEVENT DONTSWAP`: the probe, and the mitigation

**This step MITIGATES #64 and does not fix it.** It removes the consequence — an address space
other address spaces depend on going away for minutes — and establishes nothing about the
cause. **The twelve minutes remain unexplained and #64 stays OPEN**, with its investigation
deferred rather than closed. Every claim below says which side of that line it is on.

Round: MVSCE on `mvsdev`, 2026-08-27. Two stages, and the second was only reachable because
the first came back unambiguous.

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not supplied to this
session — the ninth round in a row.

---

## 0. What was re-derived rather than carried over

The red line for this step was that nothing comes from memory. Re-read live off the target
**this round**, not taken from 64-3-0's write-up or from the kickoff:

| | value | source, read live |
|---|---|---|
| `DONTSWAP` | **41** | `SYS1.AMODGEN(SYSEVENT)` line 91 — the `&EVENTO SETA` immediately preceding its `AIF` |
| `OKSWAP` | **42** | same member, line 93 |
| entry | **`SVC 95`** | same member, line 209 (`.SVC SVC 95  SYSTEM RESOURCES MANAGER SVC`) |
| `OUCBQFL/SFL/AFL/ASCB/NDS` | `X'10'/X'11'/X'13'/X'28'/X'84'` | `(IRAOUCB)` through the DSECT gate |
| `ASCBOUCB` / `ASCBFMCT` | `X'90'` / `X'98'` | `(IHAASCB)` through the same gate |
| `OUCBNSW` / `OUCBASW` / `OUCBGOO` bit positions | BIT0 / BIT7 / BIT0 | **read directly as `EQU`s** (`IRAOUCB` lines 103 / 127 / 96) — the gate proves *computed offsets*, and a bit position inside a named byte is not one |

**The gate is the survey's, and it was re-run on fresh captures: `IRAOUCB` 17/17 and `IHAASCB`
13/13 reproduce every offset a prior IFOX00 `CBOFF` job proved.** Only then were the two
derived offsets used.

---

## 1. Stage A — the probe

### 1.1 Why a probe at all, and why it had to run inside NSFS

`SYSEVENT` / SVC 95 is a seam this project has never used, so it earns an isolated check before
any production path issues it. The question was one sentence: **is NSFS authorised to make
itself non-swappable, and does the request actually take?**

It could not be answered from a batch job. 64-3-0 left open whether `OUCBASW` ("AUTHORIZED FOR
DONTSWAP") is granted at ATTACH from the PPT or set when a DONTSWAP is accepted, and could not
separate them because the only three address spaces with `ASW` were also the only three with
`NDS > 0`. **The discriminating case has to be an address space that issues DONTSWAP without a
PPT entry — which is NSFS.**

### 1.2 Shape: operator-gated, never on the startup path

The probe is reached only through a new verb, **`F NSFS,SWAP`**. A path that ran unconditionally
at `S NSFS` could break a normal start; a MODIFY is reversible and costs nothing when not
used. The verb rides the existing `g_statsextra` seam pattern — a handler pointer that is
**NULL until a build registers one** — so **Phase 1 is unchanged**: `F NSF,SWAP` draws
`NSF808E` like any unknown verb and the `NSF880I` help text does not grow. Host-tested both
ways, and the assertions were **verified to discriminate** (dropping the registered-only guard
turns the suite red on the Phase-1 inertness check).

### 1.3 The three states, and what is actually the proof

**R15 is not the proof.** Accepted, rejected and *silently ignored* are three outcomes and the
third is this project's standing failure class. R15 cannot discriminate here because **SRM
documents no return code for these codes** — the ancestor recorded exactly that,
*"R15 whatever SYSEVENT returns / The doc I have doesn't indicate a return code"*
(`mvsevent.asm:15-16`). So it is reported, labelled unspecified, and **never branched on**;
there is no `if (r15 == 0)` in `src/nsfswap.c` by construction.

The proof is the read-back, taken through **one** code path with **one** identity assertion —
the `'OUCB'` eyecatcher **and** `OUCBASCB` pointing back at the ASCB it was chased from (the
64-0c LSQA trap one control block over) — used at all three read points so the readings are
comparable rather than merely all taken.

### 1.4 The readings — two runs, byte-identical

```
NSF844I SWAP PROBE (64-3-1 STAGE A) -- DONTSWAP=41 OKSWAP=42 VIA SVC 95
NSF841I 1 BASELINE        OUCB=FE7660 NSW=N ASW=N GOO=N NDS=0 FMCT=186
NSF842I 1 BASELINE        RAW SFL=00 AFL=48 QFL=00
NSF846I 2 DONTSWAP ISSUED, R15=0 (UNSPECIFIED -- NOT THE PROOF)
NSF841I 3 AFTER DONTSWAP  OUCB=FE7660 NSW=Y ASW=N GOO=N NDS=1 FMCT=186
NSF842I 3 AFTER DONTSWAP  RAW SFL=80 AFL=48 QFL=00
NSF846I 4 OKSWAP ISSUED,   R15=0 (UNSPECIFIED -- NOT THE PROOF)
NSF841I 5 AFTER OKSWAP    OUCB=FE7660 NSW=N ASW=N GOO=N NDS=0 FMCT=186
NSF842I 5 AFTER OKSWAP    RAW SFL=00 AFL=48 QFL=00
NSF847I VERDICT: DONTSWAP ACCEPTED (NDS 0->1, NSW SET)
NSF849I RELEASE PROVEN: NDS BACK AT BASELINE 0
```

**Unambiguous acceptance.** `OUCBNSW` `00 → 80`, `OUCBNDS` `0 → 1`, and released cleanly.
**Step 5 is not tidy-up:** `OUCBNDS` is a count, and a probe that left NSFS pinned would have
changed the machine as a side effect of measuring it — so the release is proven against the
baseline **value**, which is read and not assumed to be zero.

### 1.5 The finding the gate did not ask for: `ASW` stays CLEAR

`AFL = X'48'` before **and** after, decoded against `(IRAOUCB)` lines 121-127: `OUCBAPG`
(X'40', APG algorithm applicable) + `OUCBJSR` (X'08', JOBSELECT received). **`OUCBASW` (BIT7,
X'01') is clear throughout — while the DONTSWAP was accepted.**

That kills both of 64-3-0's candidate readings in one observation:

- `ASW` is **not set by an accepted DONTSWAP** — it stayed clear across one;
- `ASW` is **not a precondition** for one — NSFS has no PPT entry, `ASW` is clear, and the
  request took effect anyway.

**So the PPT entry is not required and the self-issued route costs an installation nothing** —
64-3-0's S1/S2 boundary resolved in favour of S1, by measurement rather than by correlation.

**The negative, stated precisely:** what is proven is that the PPT entry is not a precondition
*for the request taking effect*. It is **not** shown that `ASW` has no other consequence; SRM
may weigh a non-PPT DONTSWAP differently under storage pressure, and nothing here reads SRM's
code.

---

## 2. The seam

`asm/nsfsevt.asm`, one C-callable entry, **derived from primary source and not copied**: as370
has no `SYSEVENT` macro (checked — `mvsmacs.macro` and `pdptop.copy` define none), so the two
instructions the macro generates for the no-ASID / `ENTRY=SVC` case are written out. ADR-0005
stands: `mvs38j-ip` is cited as the reason we knew this works on 3.8j, and none of its code is
reused.

**Leaf form, and the reason is the SVC TYPE.** `NSFCIHLT` carries `FUNHEAD SAVE=` because
**SVC 33 is type 2** and needs save-area linkage through the caller's R13 chain. **SVC 95 is
type 1** — measured from this target's live SVCTABLE entry in 64-3-0 — so it saves into the
SVRB the FLIH owns and never walks that chain. That is also why `nsftmr_plat_arm` issues
`STIMER` (SVC 47, type 1) as a leaf and is correct. *CLAUDE.md §3's sentence — "a routine that
issues an OS macro/SVC must give its callee a save area" — is too broad as written, and
`NSFTMARM` has contradicted it since M0-5; the real rule is the type.*

**No `SPKA` in the seam.** libc370 `__super(PSWKEY0, …)` does `MODESET MODE=SUP` **and** `SPKA`,
so the caller supplies both and "what executes under a borrowed key" stays one short block.

**The as370 listing is the gate on the emitted bytes, and it was checked:**

```
000012 5800 1000     L     R0,0(,R1)     base R1, NOT dropped to base 0
000016 0A5F          SVC   95            0A = SVC, 5F = 95
```

plus the `FUNHEAD` eyecatcher intact and `LM R0,R12` leaving R15 untouched, so `FUNEXIT
RC=(R15)` carries the SVC's return. **All 8 source statements present in the listing in source
order** — and that check was **verified to discriminate**: a deliberately over-long comment on
the `L` line made as370 merge the next statement and **drop the `SVC 95` entirely**, emitting
`L R0,0(,R1)95   SYSEVENT -- SRM`. That is the failure mode this project has paid for three
times, reproduced on this very file and then removed. Zero instruction lines past column 71.

---

## 3. Stage B — the mitigation

### 3.1 What it is

`SYSEVENT DONTSWAP` at STC initialisation, `SYSEVENT OKSWAP` at shutdown — the ancestor's
lifecycle, sourced from `sysinit.c:67` and `xdone.c:51-54`.

**`DONTSWAP` immediately after `clib_apf_setup`**, the earliest point at which SYSEVENT is
possible and the same place the ancestor put it. Its message is consequently the FIRST line the
STC writes, ahead of `NSF040I`.

**`OKSWAP` after `nsfsx_stop()` and the device quiesce, before the ESTAE is deleted.** Both
sides of that placement are deliberate: those two steps are exactly the ones that must not be
swapped out mid-flight — `nsfsx_stop` nudges clients parked on reply ECBs in *this* address
space and drains for up to 10 s, and the quiesce closes live channels — and a fault in the
release should still have recovery. **One call covers both of `nsfsx_stop`'s branches**,
drained and retain, because it returns either way and the teardown below it is linear. A pinned
address space left pinned after `P NSFS` is the same class of debt as a retained anchor, and
the retain branch is the one with no live gate (M5-2b3).

**Phase 1 never issues it, structurally.** `src/nsfswap.c` and `asm/nsfsevt.asm` are in the
NSFS `[[module]]` source list and not in NSF's; Phase 1 has no `ac = 1`, no `clib_apf_setup`
and no `__super`, so it could not issue SYSEVENT even if asked. The guarantee is a source list,
not a runtime test.

**DESIGN PIN — a refusal warns and continues.** If `DONTSWAP` is not accepted, NSFS says so
(`NSF852W`) and starts anyway. The SVC steal refuses because getting *that* wrong is a
correctness failure; this is a latency mitigation, and a stack running with a known latency
risk beats no stack. The message names the condition so an operator reads it in the log rather
than inferring it from a stall hours later. *This is the one judgement call in Stage B and it
is a pin: it can be flipped to refuse-to-start without disturbing anything else.*


### 3.2 The gate — and it did NOT discriminate

**This is the round's most important negative result, and it is stated here rather than
buried.** The gate was the two-client contention round that reproduced the stall twice within
90 seconds (`docs/nsf-64-0c-measurements.md` §6): `TSTRQXC PARM='A'` and `PARM='B'` run
concurrently and repeatedly, with `OUCBQFL`, `OUCBSFL`, `OUCBNDS`, `ASCBSTOR`, `OUCBSWC` and
`ASCBFMCT` sampled continuously and identity asserted on every reading. The instrument design
was right: `QFL` alone cannot carry it — `QFL = 00` before and after is equally consistent with
"no transition began" and with "one began and completed between samples", which is exactly how
64-0e's null turned out unreadable — so `ASCBSTOR` and `OUCBSWC`, which show a **completed**
cycle retrospectively, were sampled beside it.

**The revert arm produced no swap transitions either.** With `DONTSWAP` removed and NSFS
verified swappable (`SFL = 00` on all 163 samples), nine minutes of heavy and demonstrably
**non-vacuous** load — `SERVED = 12997`, `COLLISIONS = 33538`, `EXHAUSTED = 517` on a freshly
started STC — gave **one** distinct `ASCBSTOR`, `OUCBSWC` never off zero, `QFL` never anything
but `00`, and not one sample with `ASCBFMCT = 0`.

**So the pinned arm's null is uninterpretable.** "No transitions while pinned" says nothing
when the control, under the same load for the same duration, shows none either. **The gate has
no discriminating power on this stand, and the mitigation is NOT demonstrated to work by it.**

**A prior round had already refuted the premise, and that is the part worth keeping.** The gate
assumed the two-client round produces swap transitions. 64-0e measured NSFS **resident on 54 of
54 OUCB samples across 38 minutes** — including an unplanned 8-minute idle stretch — while
NSFV, the same transport with no device, swapped out on 12 of 16. **NSFS does not swap on this
stand under any load this round can produce**, which is also why #64's stalls are rare: the
swap-out is itself the rare event, not merely its slowness. The gate was built on a premise the
record already contradicted, and reading 64-0e more carefully first would have caught it.

**What the mitigation therefore rests on**, with each standing named:

1. **MEASURED.** `DONTSWAP` is accepted, takes effect, and releases — Stage A twice, at init
   (`NSF851I … NDS=1`), at shutdown (`NSF853I`), and again on the restored build.
2. **REASONED, NOT MEASURED HERE.** MVS does not swap a non-swappable address space, so the
   transition underneath every recorded stall cannot begin. That follows from what `OUCBNSW`
   *means* (`IRAOUCB` line 103, "NON-SWAPPABLE STATUS"). It is not a result of this round, and
   this round could not confirm it.

**The control for the pinned side remains 64-0f's stall** — same gate shape, `QFL=80[GOO]
SRC=09`, `ASCBSTOR 0FAF3C00 → 0FC26C00`, `OUCBSWC 0 → 1` — on record and not re-run. One stall
in four attempts is the base rate, and neither arm here ran long enough to expect one.

### 3.2a Two instrument faults of mine, both in the record

**Arm 1 was contaminated and is SUPERSEDED, not caveated.** A sampler started with `nohup`,
which I had concluded was dead, was still writing to `gate-pinned.log` when a later run
truncated the file underneath it: two producers, one file, 228 interleaved samples and a
non-monotonic timestamp at index 223. Every *value* in it is a genuine reading of NSFS — both
processes ran the same code against the same ASCB with identity asserted per sample — and all
of them agree with the clean arm. But the window, the sample count and the `ASCBFMCT`
change-points are untrustworthy, so the arm was re-run under a script that kills strays and
**asserts none survive** before starting exactly one.

**The sampler took the password on `argv`**, so it stood in `ps` output for the life of every
run. Changed to read `MBT_MVS_USER` / `MBT_MVS_PASS` from the environment before the script was
committed.

### 3.3 Three deployed states, each proven positively

No state is inferred from the absence of a message.

| state | check | result |
|---|---|---|
| **pinned** (Stage B) | `NSF851I … NDS=1` at startup | present |
| **reverted** | `NSF851I` absent — *ambiguous alone*, so: `F NSFS,SWAP` baseline on a fresh STC | **`NSW=N ASW=N NDS=0`** — a swappable baseline is impossible on the pinned build |
| **restored** (shipping) | `NSF851I … NDS=1` at startup | present; source verified `git diff --quiet` identical to the committed Stage B |

The revert itself was verified as **exactly one change** by a comment-stripped source diff
against the pinned version: the `nsfswap_dontswap` call removed, nothing else.

**One reading that must not be over-claimed.** `NSF853I` appeared at shutdown on the **revert**
build too — which never pins anything. `nsfswap_okswap` returns 0 whenever `OUCBNSW` ends
*clear*, and that is also true when nothing was ever set. It is the right semantic for an
operator message ("this address space is swappable") but it is **not** evidence that a pin was
released. The release evidence is the probe's step 5, which compares `OUCBNDS` against a
baseline value it read first.

---

## 4. The cost, measured like-for-like

Both clean arms ran **163 samples over 9 minutes** under the same rounds, one sampler each,
timestamps monotonic — so the two columns differ in one variable.

| | RESTORED (pinned) | REVERTED (swappable) |
|---|---|---|
| `OUCBSFL` | **`80`** — `OUCBNSW` set | `00` |
| `OUCBNDS` | **1** | 0 |
| distinct `ASCBSTOR` | 1 | 1 |
| `OUCBQFL` | `00` only | `00` only |
| `OUCBSWC` | 0 | 0 |
| `ASCBFMCT` peak | **186** (744 KB) | **186** (744 KB) |
| `ASCBFMCT` floor | **56** (224 KB) | **46** (184 KB) |
| samples with `FMCT = 0` | 0 | 0 |

**The only fields that differ are the two `DONTSWAP` sets.** Everything about swapping reads
identically, which is §3.2's finding stated as a table.

**Pinning does NOT prevent page stealing, and that is what makes the cost small.** `ASCBFMCT`
fell from 186 to 56 frames *while pinned* — MVS trims a non-swappable address space perfectly
well; only the **swap** is prevented. So the cost is not "744 KB held permanently", which is
what a reading of `OUCBNSW` alone would suggest. Against the swappable control under the same
load it is **10 frames — 40 KB — more resident at the floor**, on a machine with 4096 frames
(`MAINSIZE 16`). Peak is identical.

*This corrects a claim made earlier in this round's own working notes, that a pinned address
space is not trimmed. It is trimmed; 64-3-0's 39-frame idle figure is therefore comparable
with the 46-56 floors here rather than anomalous beside them.*

---

## 5. What this round establishes, and at what strength

**Measured, live, repeatedly:**

1. **`DONTSWAP` is accepted and takes effect on NSFS** — `OUCBNSW` `00 → 80`, `OUCBNDS`
   `0 → 1` — in five separate observations: Stage A twice, at init on the Stage B build, at
   init on the restored build, and once more via the probe on the pinned baseline
   (`NDS 1 → 2 → 1`).
2. **It releases cleanly**, proven against the baseline **value**, not against zero.
3. **The `OKSWAP` shutdown path runs**, in the right order — after `NSF044I` (transport
   stopped) and before `NSF011I` — which nothing before this round had exercised.
4. **The PPT entry is not required** (§1.5), which resolves 64-3-0's open question and makes
   the self-issued route cost an installation nothing.
5. **Three deployed states, each proven positively** (§3.3), never by absence.

**Reasoned, NOT measured here:** that pinning prevents the #64 stall. It follows from what
`OUCBNSW` means, and **the gate built to confirm it could not** (§3.2).

---

## 6. What this round does NOT establish

- **That the stall stops occurring.** The gate does not discriminate; the control produced no
  swap transitions either.
- **That swap transitions stop.** Same reason.
- **That #64 is fixed.** It is **mitigated**, and the distinction is the point. **#64 stays
  OPEN.**
- **Anything about the cause.** The twelve minutes are still unexplained and the measured chain
  still ends at `OUCBQFL = X'80'` inside MVS.
- **That `OUCBASW` has no other consequence.** Only that it is not a precondition for the
  request taking effect.
- **That `NSF853I` proves a release** — it reports that `OUCBNSW` is clear, which is also true
  when nothing was pinned (§3.3).

---

## 7. Regression

| | |
|---|---|
| host suite | **2934 PASS / 0 FAIL**, 27 tests (TSTOPR 25 → 34; the new assertions **verified to discriminate**) |
| cross-build | clean, 6 modules; alias scan **224 unique**, five new |
| NSFV round | `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` **484 PASS / 0 FAIL**, all CC 0 batch **and** TSO |
| NSFS round | `TSTRQXC`/`TSTRQXF` **122 PASS / 0 FAIL**, CC 0 batch **and** TSO |
| `TSTRQXM` | **batch CC 0**, host peer verifying **9353 bytes byte-exact**; the batch run passes *"CONNECT across the boundary (the first PARKED request to complete)"*. The TSO re-run FAILs **by design** — `errno=61`, the one-shot listener consumed by the batch run (the TSTTCPW precedent); batch is the gate |
| dumps | none |

**A procedural note worth keeping:** the first NSFV attempt failed `ABEND SFEF` on four of five
tests because **NSFV was never started** — the Stage-0 tests are clients of the probe STC, and
`P NSFS` alone leaves no SVC router installed. The round order is `P NSFS` → **`S NSFV`** →
run → `P NSFV` → `S NSFS`.
