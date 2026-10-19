# d1c — the #67 rejection, and the encoding question settled

**Status: ALL FOUR ITEMS DONE. §1 offline-gated and LIVE-GREEN. §2.1's blocking
question SETTLED offline and case 2 ESTABLISHED live — the case #100 could not
earn. §2.2 done in three states for BOTH checks. §2.3 (Phase 1 live) GREEN. One
assertion is red in all three states of §2.2 and is out of scope: the `ulen` gap
#100 found.**

PR #100 was merged at the start of this round and `main` merged in, so the
kickoff's base is now true.

Branch `m5-2d1c`, on `main` at `6046003`. Host **3469 PASS / 0 FAIL**, unchanged
— and that is a **no-regression check only**: every file this round changes is
MVS-only (`asm/nsfvsvc.asm` never compiles on host; `src/nsfv.c` and
`src/nsfsx.c` are Phase-2 STCs; `test/mvs/tstrqxf.c` is `host = false`).

## Against the kickoff's acceptance list

| # | item | status |
|---|---|---|
| 1 | §1 refusing pre-claim, rc in the caller's block, comment naming c3 | **MET** — and the shape changed to *permit one* on evidence (§1) |
| 2 | instruction-stream diff confined; `as370 -a=` listing quoted | **MET** — 364 → 368, delta exactly the four inserted, 0 non-displacement changes |
| 3 | §2.1 with its ASCII/EBCDIC question settled, refusal attributed to the key check | **MET** — question settled offline (§2), case 2 established live (§4) |
| 4 | §2.2 in three states for both checks | **MET** — both checks, one axis each (§5) |
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

---

## 4. §2.1 — case 2 ESTABLISHED

`TSTD1R` **CC 0 batch+TSO, 10 PASS / 0 FAIL** at NSFV. **CC 0 rather than CC 20
is itself the proof case 2 ran** — the repaired gate returns 20 when its
precondition fails, so a green return can no longer be earned by skipping.

```
case 2: sent     D4 E1 C5 E4  (= NSFV_REQ_EYE - 1)
case 2: landed   D5 E2 C6 E5  (in CSA, after +1)
case 2: wanted   D5 E2 C6 E5  (= NSFV_REQ_EYE)
case 2: readback D5 E2 C6 E5  (XFEROUT -> ubuf)
case 2: readable=1 storable=0 (both as required)
case 2: target rc 5AC0F001 -> 5AC0F001, seq 5AC0F001 -> 5AC0F001
PASS: 2.4(2): a foreign R8 was REFUSED and the block NOT written
```

**The refusal is now ATTRIBUTABLE, which is exactly what #100 could not earn:**
the eyecatcher **provably landed** (`landed` == `wanted`, and `readable=1` is a
`memcmp` against the literal), so the eyecatcher check is not what refused; the
target is **provably key-0** (a key-8 read succeeds, a key-8 store faults under
`___try`), so it is not merely a bad address; and the block was **not written**.
TPROT is what is left.

### A false label, caught by the case's own first run

The first live run printed `sent D5 E2 C6 E5 (= NSFV_REQ_EYE - 1)` — the
literal, not its minus-one. **`XFER` is a round trip**: `XFEROUT` copies
`stage[]` back into `ubuf` after the STC replies, so the buffer held the
*transformed* bytes by the time the diagnostic read it.

The label was false on a real measurement — **the same defect class this round
exists to remove** — and it *looked* right, because the transformed value is the
one a reader expects to see. Fixed by snapshotting the sent bytes before the
call; the read-back is now its own row rather than being silently mistaken for
the send. Recorded rather than quietly corrected, because the run that produced
it is the reason the repair is trustworthy.

---

## 5. §2.2 — the three-state revert, both checks

Both arms vary **one axis**, and in both the restored state is confirmed
identical to the baseline rather than merely green.

### Arm 1 — the R8 check (d1b), at NSFV

Reverted by commenting out the four TPROT cards, and **verified out of the
listing** rather than assumed: **2 emitted `E501` instructions in the restored
build, 0 in the reverted one**. The restored object deck is **byte-identical**
to the pre-revert build.

| state | result | case 2 |
|---|---|---|
| restored | **CC 0**, 10 PASS / 0 FAIL | refused; `rc 5AC0F001 -> 5AC0F001` |
| **reverted** | **CC 1**, 8 PASS / **2 FAIL** | **ACCEPTED**; `rc -> 00000000, seq -> 00000001` |
| restored | **CC 0**, 10 PASS / 0 FAIL | refused again |

**Exactly one assertion moved** — `2.4(2)`, in both the batch and the TSO run.
`2.4(1)` (no eyecatcher) and `2.4(3)` (the never-referenced tail) pass in **all
three** states: the eyecatcher check is untouched, and case 3 is the negative
control that is *supposed* to pass in both arms.

The reverted arm renders the hole **positively**: `rc -> 00000000` and
`seq -> 00000001` are the router completing a `QUERY` **into key-0 storage the
caller cannot write** — the 20 key-0 stores d1b's TPROT probe exists to stop.

### Arm 2 — descriptor ownership (d1), at NSFS

Reverted by disabling the ownership comparison **only**; the resolution above it
is deliberately left intact, so the axis varied is the ownership test and not
whether a descriptor resolves at all.

| state | result | what B reached |
|---|---|---|
| restored | 12/13 | `SWEEP pre-own 0`; `foreign(00010000) rc=-1 errno=9` |
| **reverted** | **9/13** | **`SWEEP pre-own 1 REACHED 00010000`**; `foreign(00010000) rc=0 errno=0` |
| restored | 12/13 | identical to the baseline |

**Exactly three assertions moved** (`2.2`, and `2.2b` retcode and errno). **The
decision is by IDENTITY, not by count**: owning nothing, B reached
**`00010000`** — which its own `00010001` derives A's descriptor to be — and
**drove it successfully** (`rc=0`). A never-existing descriptor still returned
`rc=-1 errno=9` in the reverted arm, so the revert did not simply break
everything.

### The one assertion red in all three states

`2.3 poll: THE REST OF THE MASK IS SERVED` fails in **every** state, restored and
reverted alike. It is **#100's `ulen` product defect**, not an ownership result:
NSFSEL and the EZASOKET facade read `ulen` as an **item count** while the
transport stages `min(ulen, 2048)` **bytes**, so a cross-AS SELECT over *N*
sockets stages *N* bytes instead of *N* × 8 and B's own socket is not reported
ready. Being constant across the arms is what identifies it as unrelated to the
axis. **Out of scope by the kickoff** — d1's §2.3 goes with the `ulen` fix, and
must be re-run after it, because a different code path will execute.

### A sequencing lesson that cost a run

State 3 of arm 2 failed on the first attempt, and the cause was mine: the
readiness poll matched A's console line **by timestamp**, A announced at a time
the pattern did not cover, and B was submitted **60 s later, after A had already
ended**. The repaired instrument **caught it and refused to report** —

```
SWEEP post-own: 0 reached out of 128 attempts
FAIL: B's OWN socket is inside the swept range (the range is adequate)
THE SWEPT RANGE DOES NOT COVER LIVE DESCRIPTORS -- sweep 1's result means
nothing and is NOT reported as a refusal.
```

— which is #100's **defect 2 repair working exactly as designed**, on a real
occurrence rather than a constructed one. The fix is to **record the console
log's length before submitting A and match only new lines**; matching on
timestamps is fragile, and the two earlier runs passed only because their
timestamps happened to fit.

---

## 6. What this round did NOT do

**Nothing in the kickoff's scope remains open.** Two things are deliberately
untouched:

* **The `ulen` gap.** A design decision with ADR weight and its own issue; the
  red line said not to fix it here, and it is what keeps `2.3 poll` red in all
  three states of §5.
* **#67 is not closed.** `ECHO` and `XFER` still strand a slot each at the probe
  STC, which services them; the `HELD` arm is now unreachable from a client at
  NSFS, which is not the same as correct. c3 retires the verbs and deletes the
  gate with them. The comment is **drafted, not posted**.

**And three limits on §1's evidence, restated so a green round does not bury
them:** no before/after arm was run; the slot and in-flight assertions **cannot
fail**, so they are a record rather than a proof; and `TSTDEATH` corroborates
`BADFUNC` after its relocation but exercises **nothing** of the new `TM`/`BNE`
pair.

---

## 7. The stand

The round ran 07.30–08.30, across six deploys and nine STC starts (NSFS 1727,
1729, 1734, 1735, 1736; NSFV 1726, 1731, 1733 and the restored-build instance),
because §5's two revert arms each need three deployed states.

**Zero dumps** — `IEA995I` count **0** for the whole log. `NSF043I SVC 239
RESTORED` / `NSFV095I SVC 239 RESTORED` on every stop, and `INFLIGHT=0` checked
**before** every `P`, so nothing was retained. **The one `NSF054W` in the log is
at 03.58.12 on STC 1715 — #100's round, not this one**, checked rather than
assumed. The two `IEF450I` are likewise both older than 07.30.

`EXHAUSTED=2 COLLISIONS=138` after the first NSFS half are `TSTRQXF`'s own (D)
pool tests, which deliberately fill the pool; the final instance reads
`BUSY=0 INFLIGHT=0 EXHAUSTED=0 COLLISIONS=0 REAPED=0`.

**Both reverts are undone in the tree and on the machine**: `asm/nsfvsvc.asm`
and `src/nsfreq.c` are byte-identical to their committed state, the restored
router's object deck is byte-identical to the pre-revert build, and the stand
was left running NSFS on the restored build — as it was found.
