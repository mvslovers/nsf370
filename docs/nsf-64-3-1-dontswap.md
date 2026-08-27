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

### 3.2 The gate, and what it does and does not assert

The gate is the **two-client contention round that reproduced the stall twice within 90 seconds
of starting** (`docs/nsf-64-0c-measurements.md` §6) — `TSTRQXC PARM='A'` and `PARM='B'` run
concurrently, repeatedly — with `OUCBQFL`, `OUCBSFL`, `OUCBNDS`, `ASCBSTOR`, `OUCBSWC` and
`ASCBFMCT` sampled continuously throughout.

**`OUCBQFL` alone cannot carry it.** `QFL = 00` before and after is equally consistent with "no
transition began" and with "one began and completed between samples" — which is exactly how
64-0e's null turned out unreadable, and why `ASCBSTOR` became the instrument in the first
place. A **completed** cycle shows retrospectively in `ASCBSTOR` (and in `OUCBSWC`) even when
`QFL = 80` is missed between samples. Both are sampled, and identity is asserted on every
reading.

**What the gate asserts is that no swap transition occurs at all** — not that no stall occurs.
The stall is roughly a one-in-four event (64-0f) and its absence over one arm would prove
little; a swap *transition* is the mechanism underneath it, is far more frequent, and is
directly observable. **The control is already on record and is not re-run:** 64-0f's stall,
same gate shape, `QFL=80[GOO] SRC=09`, `ASCBSTOR 0FAF3C00 → 0FC26C00`, `OUCBSWC 0 → 1`.
