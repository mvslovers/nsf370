# d1c — the #67 rejection, and the encoding question settled

**Status: §1 DONE, offline-gated and LIVE-GREEN. §2.1's blocking question
SETTLED offline. §2.3 (Phase 1 live) GREEN. §2.1's code change and §2.2 NOT
STARTED — they need PR #100, which is still open.**

Branch `m5-2d1c`, on `main` at `6046003`. Host **3469 PASS / 0 FAIL**, unchanged
— and that is a **no-regression check only**: every file this round changes is
MVS-only (`asm/nsfvsvc.asm` never compiles on host; `src/nsfv.c` and
`src/nsfsx.c` are Phase-2 STCs; `test/mvs/tstrqxf.c` is `host = false`).

## Against the kickoff's acceptance list

| # | item | status |
|---|---|---|
| 1 | §1 refusing pre-claim, rc in the caller's block, comment naming c3 | **MET** — and the shape changed to *permit one* on evidence (§1) |
| 2 | instruction-stream diff confined; `as370 -a=` listing quoted | **MET** — 364 → 368, delta exactly the four inserted, 0 non-displacement changes |
| 3 | §2.1 with its ASCII/EBCDIC question settled, refusal attributed to the key check | **PARTIAL** — the *question* is settled (§2); the *case* needs #100's file |
| 4 | §2.2 in three states for both checks | **NOT MET** — R8 arm's instrument absent, ownership arm's known-defective (§5) |
| 5 | §2.3 live | **MET** (§3) |
| 6 | #67 updated — proposed, not applied | **MET** — drafted in `issue-67-comment-draft.md`, not posted |
| 7 | `make test-host` ≥ 3469 PASS, 0 FAIL | **MET** — 3469 / 0, unchanged |
| 8 | PR text separates host- from live-verified; names §2.3-of-d1 as blocked on `ulen` | **MET** in this report; the PR body follows it |

Two items the kickoff did not list and this round added, both because CLAUDE.md
required them: **§1's live gate** (§8.4 — `asm/*.asm` is validated on MVS before
it merges) and the **ADR-0042 annotation** (§8.7 — a decision that changes
behaviour is recorded where a reader looks).

---

## 1. The #67 rejection — DONE

### What it does

A probe verb reaching the **production** STC claims a CSA slot and an in-flight
count, is set `HELD` by the dispatch arm because its `xfunc` is not `RQE`, and is
never re-examined: the client parks on its reply ECB forever and the slot is gone
for the life of the STC. The SVC routine now refuses **ahead of the claim**, so
the refusal costs **no slot and no in-flight count by position rather than by
argument** — the form M5-2c2 established for the retired `FNORPH`. The rc goes
into the **caller's block** (`BADFUNC`, not `BADREQ`'s R15-only), so a client
cannot read back its own initialised value and take it for "the SVC never ran".

### The shape changed, and the reason is empirical

The kickoff said *refuse by name*. **Tracing the staging dispatch first showed
that would not have discharged the round's own justification.** `CLAIMOK`'s
dispatch is a fall-through chain:

```
L R3,REQFUNC(,R8)
C R3,=A(FNXFER)  -> XFERIN
C R3,=A(FNRQE)   -> RQEIN
[fall through]   -> ECHO staging
```

So **`ECHO` is not the only way in.** Any `REQFUNC` the routine does not
recognise — one wrong word in a client — falls through, stages as `ECHO`, and
hangs identically. Naming `ECHO` and `XFER` leaves that open, and it is the
*cheapest* instance of the fault: it needs no knowledge of the verb set at all.

**So the production STC permits exactly `FNRQE` and refuses everything else.**
One compare-branch pair instead of two, and it closes the whole class. Mike
ratified the change of shape before it was written.

`QUERY` / `UNSTAGE` / `SLOT` are unaffected either way: they branch out of the
chain **above** the insertion point and take no slot.

**Placement is LAST in the pre-claim chain, not first**, and the kickoff's
"first" is the *positional guarantee* (ahead of the claim ⇒ no slot, no count),
not the literal position. It has to follow the `QUERY`/`UNSTAGE`/`SLOT` branches
because those verbs are serviced at **both** servers. Said at the code, because a
reviewer reading "first" literally will flag it.

### The gate: an anchor capability bit

`NSFV_ANCHOR_PROBE` (`X'40000000'`) in the existing `flags` word. Set by the
probe STC (`src/nsfv.c`) and by nothing else; the production STC leaves it clear,
and **the omission is stated at that line**, because an omission is invisible.

* **Polarity is fail-closed.** The bit says *permitted*, never *forbidden*, so
  the zero a zeroed or unset anchor carries means REFUSE. A server that has not
  said it services probe verbs is not assumed to.
* **`flags`, not `rsvd0`.** `rsvd0` is the **last** slack word in the header and
  its own comment names the use it was reserved for (the lost-race counter);
  spending it here would put the next addition back to a full Stage-0 round.
  `ANCFLAG` is already loaded and `TM`'d one instruction above, so the marginal
  cost is one instruction.
* **`NSFV_ANCHOR_VER` stays 3, and neither skew direction is silent.** No field
  moved (the offset asserts are byte-identical to `main`). A stale router does
  not test the bit and services probe verbs as it does today — correct at NSFV,
  no worse than today at NSFS. A new router against a stale STC reads the bit
  clear at NSFV and refuses every probe verb there, taking the **whole Stage-0
  set red at once**. A version bump is the honest answer to a *silent* skew;
  there is none here.

### What it does NOT do

**It does not close #67.** `ECHO` and `XFER` still strand a slot each at the
probe STC, which services them. M5-2c3 retires the verbs and **deletes this block
with them** — said at the code, so it is not left as unattributable dead code.

The `HELD` arm in `src/nsfsx.c` stays as the backstop it always was. Making it
unreachable from a client is not the same as making it correct: a slot set `HELD`
there is still never re-examined.

### Offline gates

| gate | result |
|---|---|
| card columns | OK — every card inside column 71 |
| `as370` | rc 0, no diagnostics |
| statement presence | all **1368** source cards in the listing, in source order |
| `TM ANCFLAG(R2),ANCPROBE` | `9140 200C` — mask `40`, disp `00C`, **base R2 not dropped to 0** |
| `BO PROBEOK` | `4710 609C` — base R6, target `00009C` = `PROBEOK` |
| `C R3,=A(FNRQE)` | `5930 6540` — base R6; the literal at `000540` reads `00000006` |
| `BNE BADFUNC` | `4770 64DA` — base R6, target `0004DA` = `BADFUNC` |
| instruction stream | 364 → **368** emitted own statements, **delta exactly the four inserted**; 274 identical, **90 differing by a uniform displacement shift, 0 by anything else** |
| offset asserts | **byte-identical to `main`** — anchor layout unmoved |
| cross-build | 6 modules + 55 test modules, no warnings |
| alias scan | **248 unique, all ≤ 8**, **none added** |
| host suite | 3469 PASS / 0 FAIL (no-regression only) |

**All three assembler gates were verified to discriminate**, not assumed to: a
deliberate 74-byte comment on the `TM` line makes the card check FAIL, `as370`
report *"This card was consumed as a continuation and the statement on it
discarded"* at **severity 8**, and the statement check name **both** victims —
the overlong card and the `BO PROBEOK` it ate.

### The live gate — RUN, and green

`asm/*.asm` must be validated on MVS before it merges (CLAUDE.md 8.4), and §1 has
never run. Section **(E)** of `TSTRQXF`, the NSFS-side probe gate, drives two
requests:

* **`ECHO`** — the verb #67 names.
* **an unrecognised `REQFUNC`** — and this one is the point. A test driving only
  `ECHO` would pass against a refuse-by-name gate that leaves the fall-through
  open, so the gate would not discriminate between the two designs.

**The assertion that carries the gate is `rc == NSFV_RC_INVALID` against an
initialised −1**: it proves the request was refused *and* that the caller's block
was written (`BADFUNC`'s shape, not `BADREQ`'s R15-only), and it is the one that
can go **red** rather than hang.

`inflight` unchanged and no slot changed state — both read straight out of CSA
before and after — are kept as a record, but they are **not** what proves "no
slot claimed", and are not presented as such: without the gate the request parks,
so the "after" side is never evaluated and those two can only ever run on the
passing path. **What proves no slot was claimed is that the request returned at
all**, with rc 4. (`busy_before` is read after sections (A)–(D), which claim and
release slots; any residue they leave is seen by both sides, so the equality
holds regardless.)

**Its failure mode is a HANG, not a failed assertion**, so each request is
bracketed by `wtof` markers: the console log survives the cancel a hang forces,
where buffered SYSPRINT does not (the M4-5 lesson, applied in advance).

**The "before" arm is on record from #100's live round** — a cancelled job, a
leaked slot, `NSF054W`, an anchor retained to IPL — and is **deliberately not
re-induced**, because inducing it costs exactly that again.

**The negative control is free and sits in the other half of the round.**
`TSTSVC` (ECHO) and `TSTUBUF`/`TSTXFW` (XFER) run against NSFV, where the bit
**is** set; those staying green is what makes this a **conditional** gate rather
than "the verbs are refused everywhere".

### The live result

Stand: MVSCE, real 0500/0501, MTU 1500. Unauthorised client throughout
(`TSTRQXF` asserts `__isauth() == 0`).

**DEPLOY-TOOK-EFFECT IS POSITIVE IN BOTH DIRECTIONS, and deliberately not
"no error appeared".** An absence-of-complaint check is the shape §8.5 warns
about, and it would sit badly in a round whose headline finding is a value that
was never observed. What rules out the partial deploy the sequence warns about
(a stale `NSFVSVC` against a new `NSFV`) is that **both modules came out of the
same `make deploy`, hence the same XMIT of the same build** — and each half then
confirms its own module from its own result:

* **the new router is loaded** — `REQFUNC 99` answering `rc=4` is *impossible* on
  the old one, which would have staged it as ECHO and parked the client;
* **the new `nsfv.c` is loaded** — `TSTSVC`'s ECHO succeeding at NSFV requires
  the `PROBE` bit, which only the new anchor sets. Against a stale `NSFV` the
  fail-closed direction would have refused it `rc=4` and taken the set red.

**NSFV half — the negative control.** `TSTSVC` / `TSTMVCK` / `TSTUBUF` /
`TSTDEATH` / `TSTXFW`, **438 PASS / 0 FAIL, all CC 0 batch+TSO** (STC 1726).
`TSTMVCD` excluded, #53. **438 is exactly the figure the record predicts for
this set**, so no test was silently lost. `TSTSVC` drives `ECHO` and
`TSTUBUF`/`TSTXFW` drive `XFER`, both against the anchor with the bit **set** —
so this is simultaneously the positive check that the bit took and the proof the
gate is conditional.

**`TSTDEATH` corroborates §1 from a second direction, and the credit is worth
being exact about.** Post-c2-stage-c it drives row 1 (LIVE) plus the retired
`FNORPH`, and that rejection returns `rc=4` from **`BADFUNC` — the same label the
new gate branches to**, whose address this insertion *moved* (`0004CA` →
`0004DA`). So `TSTDEATH` green proves `BADFUNC` still writes the rc into the
caller's block after the relocation. It proves **nothing about the new `TM`/`BNE`
pair**, which is bypassed at NSFV: the reconciliation of 438 must not be read as
covering it.

**NSFS half — the gate.** `TSTRQXC` / `TSTRQXF`, **130 PASS / 0 FAIL, CC 0
batch+TSO** (STC 1727). **Was 122; the delta is exactly 8 = the four new
assertions × batch and TSO.** From the spool, in both runs:

```
TSTRQXF: (E) ECHO at NSFS -- HANGS if the #67 gate is absent
TSTRQXF: (E) ECHO RETURNED rc=4 (it did not park)
TSTRQXF: (E) REQFUNC 99 at NSFS -- the fall-through case
TSTRQXF: (E) REQFUNC 99 RETURNED rc=4
  PASS: (E) ECHO at the production STC is refused, rc in the block
  PASS: (E) an unrecognised REQFUNC is refused, not staged as ECHO
  (E) inflight 0->0, non-FREE slots 0->0
  PASS: (E) the refusals took NO in-flight count
  PASS: (E) the refusals claimed NO slot
```

**`REQFUNC 99 RETURNED rc=4` is the line the design change exists for.** It is
the fall-through case — the instance a refuse-by-name gate would have left open
— and it is now measured rather than reasoned.

**HOW THIS DISCRIMINATES, stated exactly.** No revert arm was run, and the
"before" behaviour is **not** re-measured here: it is on record from #100's live
round (a parked client, a cancelled job, a leaked slot, `NSF054W`, an anchor
retained to IPL), and re-inducing it costs precisely that again. What this round
*does* show, on **one deployed binary with one axis varied**, is the conditional:
**the same verb at two servers gives opposite outcomes** — `ECHO` serviced at
NSFV (`TSTSVC` green) and refused `rc=4` at NSFS. That is the axis the design
says controls it, and it is what a "the verbs are refused everywhere" bug would
fail. It is not a before/after arm and is not offered as one.

---

## 2. §2.1's blocking question — SETTLED

`NSFV_REQ_EYE` is **EBCDIC** (`D5 E2 C6 E5`), and #100's `4E 53 46 56` was
**derived from the C source, not observed** — the captured line stops after eight
bytes and never contains the `(wanted …)` segment at all. Full chain, both
corroborations and the four consequences for case 2 in
[`eyecatcher-encoding.md`](eyecatcher-encoding.md).

The kickoff made this a precondition for quoting any case-2 result. It is
discharged, and it was discharged **offline** — no stand time.

---

## 3. §2.3 — Phase 1 live, GREEN

The `NSF` module was redeployed in this round, so this is a real no-regression
check on those bytes and not a formality.

* `S NSF` clean — `NSF000I` → `NSF210I CTCI 0500/0501 UP ... MTU 1500` →
  `NSF211I INTERFACE LNK1 CUU 0500 UP` → `NSF001I`, **no abend**.
* **`F NSF,APPS` → `NSF808E UNKNOWN COMMAND APPS`** — the structural red line
  holds on the machine: Phase 1 registers no classifier and no APPS verb, so
  d1's ownership check and c1's sweep are inert there by construction, not by
  care.
* Ping **20/20, 0 % loss**, 0.607/0.884/1.311 ms — the receive path is
  unaffected.
* `P NSF` clean, same second.

## 4. The stand

Zero dumps for the whole round (`IEA995I` count **0**), and no `IEF450I` inside
the round window. `NSF043I SVC 239 RESTORED` / `NSFV095I SVC 239 RESTORED` on
every stop, **no `NSF054W`**, `INFLIGHT=0` before and after every stop, so
nothing was retained. `EXHAUSTED=2 COLLISIONS=138` after the NSFS half are
`TSTRQXF`'s own (D) pool tests, which deliberately fill the pool. The stand was
left as found, with NSFS running.

## 5. NOT STARTED, and why

**PR #100 is still open.** `test/mvs/tstd1r.c` exists only on
`m5-2d1-live-b`, and #100 also carries the repaired `tstd1b.c` that §2.2's
ownership arm must be measured with. Copying either onto this branch would
duplicate #100's diff into this PR — the trap #98's retarget check exists to
prevent — and merging is the maintainer's countersign, not mine.

The two remaining items are blocked for **different reasons**, and the difference
decides the remedy:

* **§2.1's code change — INSTRUMENT ABSENT.** `test/mvs/tstd1r.c` does not exist
  on `main`. The change itself is specified precisely in
  `eyecatcher-encoding.md` §"Consequences"; it is four edits to a file this
  branch does not have.
* **§2.2's R8 arm — INSTRUMENT ABSENT.** Same file.
* **§2.2's descriptor-ownership arm — INSTRUMENT PRESENT BUT KNOWN-DEFECTIVE.**
  `tstd1a.c` and `tstd1b.c` are both on `main`, so this one *could* be run here.
  It is not, and the reason is #100's own defect 2: **a zero produced by a sweep
  range that does not cover the target is indistinguishable from a zero that
  means refused.** Running the revert against main's `tstd1b.c` would produce
  exactly that uninterpretable result — a measurement whose null cannot be told
  from a broken instrument — and #100's repair (sweep before owning anything,
  positive-control the range, derive A's descriptor at run time) is on its
  branch. Running it here would not be blocked; it would be **worthless**, which
  is a stronger reason to wait, not a weaker one.

**§2.3 is DONE** — see §3 above.

Both are live work and run on the sequence in [`sequence.md`](sequence.md), which
this round executed for its own half and which held.

§1's round turned out to be **self-contained** — `TSTD1R` is §2.1's instrument,
not §1's — so §1 did not wait for #100 and is live-validated, which is what
CLAUDE.md 8.4 requires before an `asm/*.asm` change merges. Checking the
remaining items the same way, per item rather than per round, is what separates
the three cases above.

**Worth naming: §1's evidence has the structure §2.2 asks for**, on the one axis
available without a redeploy — the same verb, one binary, opposite outcomes at
the two servers, with the restored state re-measured. It is *not* a before/after
revert and §1 does not claim to be one; but the "one assertion moving" discipline
§2.2 exists to enforce was applied, and it is the reason the conditional is
established rather than assumed.
