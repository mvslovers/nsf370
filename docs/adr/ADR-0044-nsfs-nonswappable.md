# ADR-0044 — NSFS makes itself non-swappable: `SYSEVENT DONTSWAP` at init, `OKSWAP` at shutdown

**Status:** Proposed (2026-08-27). Settles **whether and how the Phase-2 STC keeps MVS from
swapping it out**, after 64-0f measured a swap cycle that left the address space
non-dispatchable for about twelve minutes.

**It cures the symptom completely and explains nothing.** That sentence is the point of this
ADR, not a caveat appended to it. The mitigation removes the *consequence* — an address space
other address spaces depend on going away for minutes — and establishes **nothing whatever**
about the cause. The twelve minutes remain unexplained, the measured chain still ends at
`OUCBQFL = X'80'` (`GOO`) inside MVS in fields NSF does not write and cannot see, and
**issue #64 stays OPEN with its investigation deferred rather than closed.**

**Relates to:** ADR-0043 (the cross-AS wake contract — the wake is *not* the defect, which is
what left the swap-out as the subject), ADR-0038 (the private-SVC transport whose STC this is),
ADR-0042 §10 (one unit of work per pass — what a stalled executive is failing to do),
ADR-0005 (no Xinu code: the ancestor is cited as precedent, never copied), spec §5.3.

**Evidence pins:** `docs/nsf-64-0c-measurements.md` (the executive is non-dispatchable during a
stall, not waiting; and *"An idle stack does not stall"*, §6), `docs/nsf-64-0d-measurements.md`
(the stall is an MVS swap-out, `OUCBQFL = X'80'`), `docs/nsf-64-0f-measurements.md` (the cycle
**completed** — `ASCBSTOR 0FAF3C00 → 0FC26C00`, `OUCBSWC 0 → 1`, ~12 minutes),
`docs/nsf-64-3-0-noswap-survey.md` (the mechanism exists, NSFS already holds the state, the cost
and the PPT question), `docs/nsf-64-3-1-dontswap.md` (this step: Stage A and the gate).
Source pins: `asm/nsfsevt.asm`, `src/nsfswap.c`, `src/nsfsmain.c`, `src/nsfopr.c`.

---

## 1. Context

64-0f re-read #64 and changed what it is. The address space is **not stuck**: `ASCBSTOR` moved
and `OUCBSWC` incremented, so MVS began a swap-out of NSFS, took about twelve minutes over the
transition, then completed it normally and resumed service — the client's job ended `CC 0000`.
"Stuck" is better read as **"very slow, and observed to finish."**

That reading does not make it tolerable. A TCP/IP stack is a *dependency*: while NSFS is
non-dispatchable every client parked on a reply ECB is parked, and twelve minutes is not a
latency, it is an outage. The question this ADR answers is therefore not "why did SRM do that"
— which is unanswered — but **"should NSFS be swappable at all."**

## 2. Decision

**NSFS issues `SYSEVENT DONTSWAP` at initialisation and `SYSEVENT OKSWAP` at shutdown.**

- **`DONTSWAP` immediately after `clib_apf_setup`** — the earliest point at which SYSEVENT is
  possible, and the same place the ancestor put it.
- **`OKSWAP` after `nsfsx_stop()` and the device quiesce, before the ESTAE is deleted.**
  Placement is deliberate on both sides: those two steps are exactly the ones that must not be
  swapped out mid-flight (`nsfsx_stop` nudges clients parked on reply ECBs in *this* address
  space and drains for up to 10 s; the quiesce closes live channels), and a fault in the
  release should still have recovery. **One call covers both of `nsfsx_stop`'s branches** —
  drained and retain — because it returns either way and the teardown below it is linear. That
  matters: **a pinned address space left pinned after `P NSFS` is the same class of debt as a
  retained anchor**, and the retain branch is the one with no live gate (M5-2b3).
- **The Phase-1 `NSF` module never issues it**, and the guarantee is **structural**:
  `src/nsfswap.c` and `asm/nsfsevt.asm` are in the NSFS `[[module]]` source list and not in
  NSF's. Phase 1 has no `ac = 1`, no `clib_apf_setup` and no `__super`, so it could not issue
  SYSEVENT even if asked; the separation is a source list, not a runtime test.

### 2.1 DESIGN PIN — a refusal warns and continues

**If `DONTSWAP` is not accepted, NSFS says so and starts anyway** (`NSF852W`). It does not
refuse to start.

The SVC steal refuses because getting *that* wrong is a **correctness** failure — a stolen slot
that is not ours corrupts an unrelated service. This is a **latency mitigation**, and a stack
running with a known latency risk beats no stack at all. The message names the condition so an
operator reads it in the log instead of inferring it from a stall three hours later.

*This is the one judgement call in the change and it is a pin, not a discovery: the maintainer
can flip it to refuse-to-start without disturbing anything else.*

## 3. The mechanism, and where every fact came from

Nothing here is from memory; each is read live from the target (64-3-0 §2, re-read in 64-3-1).

| fact | source |
|---|---|
| `DONTSWAP` = 41, `OKSWAP` = 42 | `SYS1.AMODGEN(SYSEVENT)` lines 91 / 93 (`&EVENTO SETA` preceding each `AIF`) |
| entry is `SVC 95` | same member, line 209 — `.SVC SVC 95  SYSTEM RESOURCES MANAGER SVC` |
| SVC 95 is **type 1**, `svcapf` **off** | this target's live `SVCTABLE` entry (64-3-0 §2.3) |
| requires **key 0** | the ancestor states it and acts on it — `mvsevent.asm:89` |
| `OUCBNSW` (X'80' in `OUCBSFL`) = live non-swappable status | `SYS1.AMODGEN(IRAOUCB)` lines 102-103 |
| `OUCBNDS` = outstanding DONTSWAPs | same, line 209 |
| `OUCBASW` = "AUTHORIZED FOR DONTSWAP" | same, line 127 |
| `ASCBNSWP` is a program *attribute*, **not** the live status | `SYS1.AMODGEN(IHAASCB)` lines 117-118 ("or will run in V=R region") |

**The seam is derived, not copied.** as370 has no `SYSEVENT` macro, so `asm/nsfsevt.asm` writes
out the two instructions the macro generates for the no-ASID / `ENTRY=SVC` case: load the code
into R0, `SVC 95`. ADR-0005 stands — `mvs38j-ip`'s `mvsevent.asm` is the reason we knew this
works on 3.8j, and none of its code is reused.

**Leaf form, and the reason is the SVC type, not "it issues an SVC."** `NSFCIHLT` carries
`FUNHEAD SAVE=` because **SVC 33 is type 2** and needs proper save-area linkage through the
caller's R13 chain. **SVC 95 is type 1** — measured — so it saves into the SVRB the FLIH owns
and never walks that chain; the plain leaf form applies, which is also why `nsftmr_plat_arm`
issues `STIMER` (SVC 47, type 1) as a leaf and is correct. *CLAUDE.md §3's sentence — "a
routine that issues an OS macro/SVC must give its callee a save area" — is too broad as
written, and `NSFTMARM` has contradicted it since M0-5. The real rule is the type.*

**The seam contains no `SPKA` of its own.** libc370 `__super(PSWKEY0, …)` does `MODESET
MODE=SUP` **and** `SPKA`, so the caller supplies supervisor state and key 0 together, and "what
executes under a borrowed key" stays one short block in `nsfswap.c` rather than two.

## 4. The proof is the read-back, and R15 is not the proof

**Accepted, rejected and silently ignored are three outcomes, and the third is this project's
standing failure class** (CLAUDE.md §8.5 — an operation whose absence is indistinguishable from
its success needs a third state or an assert).

R15 cannot be that discriminator here: **SRM documents no return code for these codes**, which
the ancestor recorded outright — *"R15 whatever SYSEVENT returns / The doc I have doesn't
indicate a return code"* (`mvsevent.asm:15-16`). So R15 is **reported and never branched on**;
there is no `if (r15 == 0)` anywhere in `src/nsfswap.c`, by construction.

The verdict comes from reading `OUCBNSW` / `OUCBNDS` / `OUCBASW` back, through **one** code path
(`nsfswap_read`) with **one** identity assertion — the `'OUCB'` eyecatcher **and** `OUCBASCB`
pointing back at the ASCB it was chased from, which is the 64-0c LSQA trap one control block
over — so the baseline and the post-call reading are comparable rather than merely both taken.

**`OUCBNDS` is a COUNT, and the release is proven against the baseline VALUE**, not against
zero. A probe that leaves NSFS pinned would have changed the machine as a side effect of
measuring it.

## 5. Stage A, and the question it settled that the survey could not

The probe runs **inside NSFS's own address space**, reached only through a new operator verb
`F NSFS,SWAP` — never on the startup path, so a wrong answer cannot break a normal `S NSFS`,
and the action is reversible. Two runs, byte-identical:

```
1 BASELINE        NSW=N ASW=N NDS=0   SFL=00 AFL=48
2 DONTSWAP ISSUED  R15=0 (unspecified -- not the proof)
3 AFTER DONTSWAP  NSW=Y ASW=N NDS=1   SFL=80 AFL=48
4 OKSWAP ISSUED    R15=0 (unspecified -- not the proof)
5 AFTER OKSWAP    NSW=N ASW=N NDS=0   SFL=00 AFL=48
```

**Unambiguous acceptance, and a clean release.**

**And `ASW` is CLEAR throughout while `DONTSWAP` was accepted.** 64-3-0 left one question open —
whether `OUCBASW` ("authorized for DONTSWAP") is granted at ATTACH from the PPT's `PPTNSWP`, or
set when a DONTSWAP is accepted — and said no discriminating case existed on this stand,
because the only three address spaces with `ASW` were also the only three with `NDS > 0`. **The
discriminating case is NSFS itself**, and it kills both halves at once:

- `ASW` is **not set by an accepted DONTSWAP** — it stayed clear across an accepted one;
- `ASW` is **not a precondition** for one — NSFS has no PPT entry, `ASW` is clear, and the
  request took.

**So the PPT entry is not required, and the self-issued route costs an installation nothing.**
That resolves 64-3-0's S1/S2 boundary in favour of S1 by measurement rather than correlation.

**State the negative precisely.** What is proven is that the PPT entry is not a precondition
*for the request taking effect*. It is **not** shown that `ASW` has no other consequence — SRM
may weigh a non-PPT DONTSWAP differently under storage pressure, or in a shortage algorithm,
and nothing here reads SRM's code.

## 6. What this does NOT establish

- **It does not explain #64.** Not the twelve minutes, not what SRM was pending on, not why
  a swap-out began against this address space and not others. **#64 stays open.**
- **It does not inherit the ancestor's diagnosis, and that is deliberate.** `mvs38j-ip` reached
  `DONTSWAP` for a hang of its own, but his trigger was `STIMER WAIT` from **MVSDOZE, a
  governor against WTO buffer exhaustion** — not a device condition — he discounted the CTCI
  MIH line himself (*"perhaps it's related, perhaps not (probably not)"*), and he called his SRM
  conclusion *"merely my best guess."* **What transfers is the mechanism and the shape** — SRM
  making a long-running server unavailable for minutes, cured by DONTSWAP. The diagnosis does
  not transfer and is not being adopted.
- **It does not make the stack immune to going unavailable.** It removes one cause.
- **It does not measure the cost of being pinned beyond storage.** Effects on SRM's domain and
  dispatching decisions were not measured.

## 7. Alternatives considered

**The PPT route — rejected on cost, and now also on necessity.** `PPTNSWP` in `IEFSDPPT` is how
MVS itself authorises JES2, VTAM, TCAM, GTF and the master scheduler. It was the serious
alternative until Stage A, and it loses on both counts:

- **Cost.** There is no `SCHEDxx` on 3.8j; `IEFSDPPT` is not a member of
  LPALIB/LINKLIB/NUCLEUS/SVCLIB but a CSECT link-edited into another module; and the sysgen
  `&PGM` input adds CPU-affinity entries with `DC X'00' SLOT FOR PROPERTIES` — no attribute
  bits. So it is a **USERMOD plus an IPL for every nsf370 installation**, and it would turn a
  stack installable by copying a PROC and a load library into one that requires modifying the
  operating system.
- **Necessity.** Stage A shows it is not needed: DONTSWAP was accepted with `OUCBASW` clear.

**Refuse to start when DONTSWAP is refused — rejected, and it is the pin in §2.1.** A latency
mitigation is not a correctness precondition. Recorded as a pin precisely so it can be flipped
without argument if the maintainer weighs it the other way.

**Do nothing and keep investigating — rejected, but only as an *addition*, not an
alternative.** The investigation is not being closed; #64 stays open and the cause is still
unknown. What is rejected is leaving a production stack exposed to a twelve-minute outage while
that continues.

**A watchdog that detects the stall and recovers — rejected.** Every recovery action available
to us (restart, POST, cancel) requires the executive to be dispatchable, and 64-0c measured
that it is precisely not. A watchdog in another address space could detect it but could not
fix it, and detection without recovery is what the measurement rounds already provide.

## 8. Consequences

**Good — and the standing of the claim matters more than the claim.** The mechanism that
produced every recorded #64 stall should not be able to occur: SRM does not swap a
non-swappable address space, so there is no transition to be slow about. **That is REASONED
from what `OUCBNSW` means (`IRAOUCB` line 103), not measured on this stand**, and the gate
built to confirm it **failed to discriminate**: with `DONTSWAP` reverted, nine minutes of heavy
non-vacuous load produced no swap transitions either, so the pinned arm's null says nothing
(64-3-1 §3.2). 64-0e had already measured NSFS resident on 54 of 54 samples over 38 minutes, so
NSFS simply does not swap under any load this stand can produce — which is also why #64's
stalls are rare. **What IS measured is that DONTSWAP is accepted, takes effect and releases**,
in five separate observations. The change is small, reversible at runtime by an operator
(`F NSFS,SWAP` exercises both directions), and costs an installation nothing.

**`NSF853I` is not proof that a pin was released.** `nsfswap_okswap` returns success
whenever `OUCBNSW` ends *clear*, which is also true when nothing was ever set — observed live,
since the revert build (which never pins) still reported `NSF853I` at shutdown. That is the
right semantic for an operator message and the wrong thing to cite as evidence; the release
evidence is the probe's step 5, which compares `OUCBNDS` against a baseline value read first.

**The cost is real and is storage.** A non-swappable address space holds its frames
permanently — MVS cannot trim it under pressure, which is the point and the price. §9 of the
round record carries the measured peak. On a 16 MB machine that pressure does not disappear; it
moves onto every other address space, and the ones already pinned (`*MASTER*`, JES2, VTAM) are
the ones that cannot absorb it.

**It hides the defect from the only detector we have — though the detector was already
close to blind.** Every #64 instrument keys on the swap-out, and with NSFS pinned none of them
can fire on NSFS. The sharper statement is that they could barely fire before: NSFS was
observed swapping exactly once across this entire investigation (64-0f), and never in 54
samples in 64-0e or in 163 swappable samples under load here. If the investigation resumes it
needs the revert build (which exists and is source-diff verified as exactly one change) or a
different subject — **NSFV**, the probe STC, which runs the same transport with no device and
*does* swap readily (12 of 16 samples in 64-0e).

**It is not a precedent for the rest of the ecosystem.** HTTPD, UFSD and FTPD are swappable
and measured healthy that way; FTPD swapped out and back on 11 of 12 samples while idle without
harm (64-3-0 §3.1a). Nothing here argues they should change.
