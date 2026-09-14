# #101 Stage 2 — the live round (M5-2d1 §2.3 + the wedge)

**Date:** 2026-09-02 · **Stand:** MVSCE on mvsdev, real CTCI pair 0500/0501, MTU 1500
**Predictions:** `predictions.md`, written before any deploy and **not edited since**.
**Proof kind: LIVE.** Host evidence is Stage 1's (PR #103) and is not repeated here.

All three predictions **hold**. One of them (P3) was amended before the run on a
source finding, with the original kept verbatim in `predictions.md`.

---

## Results against the predictions

| | prediction | result |
|---|---|---|
| **P0** | the deploy is seen to take effect | **held** — see below |
| **P0b** | `2.3 poll` is the single assertion that moves | **held** — B 12/13 → **13/13** |
| **P1** | arm 1, poll form | **held** — `rc=1 errno=0 foreign.ready=0 own.ready=2` |
| **P2** | arm 2, parked form | **held** — `rc=0 ready=0` |
| **P3** | arm 3, the wedge, both directions | **held** — see below |
| **C3** | the executive answers while wedged | **held, three ways** |

### P0 — how the deploy was proved to take effect

This round adds no field and no message, so the check had to be behavioural.
**`2.3 poll` was RED in all three states of the d1c round** (CLAUDE.md §7 records
it as the one assertion red throughout, and names this defect as its cause), so
it passing is **impossible on the pre-#101 module**. That is a positive proof,
not an absence.

The §5 tell — identical values across supposedly different builds — was checked
explicitly at every deploy. It did not appear; a *different* failure did, below.

### P1 / P2 — arms 1 and 2 (`TSTD1B` roles A + B, JOB03030 / JOB03031)

```
SELECT poll:   rc=1 errno=0 foreign.ready=0 own.ready=2
SELECT parked: rc=0 errno=0 ready=0
=== TSTD1B: 13/13 passed ===
```

The foreign entry is not ready; **B's own entry IS served** (`ready=2`, the
positive control, so a green arm is not a broken instrument); the call is not an
error, so SELECT does not become an existence oracle. On the parked path a
readiness change on a socket B does not own does **not** complete B's SELECT —
that is the re-scan path, which reads its identity from the SELCB rather than
the request, and it is a different code path from the poll form.

2.2 corroborates on the same run: 0 of 128 descriptors reached before B owned
one, 1 of 128 after (its own); foreign and never-existing both return
`rc=-1 errno=9`, indistinguishable.

### P3 — arm 3, the wedge (`TSTD1B` roles W + V), three states, one axis

| state | W (block-forever SELECT) | V (ordinary request) | `SERVED` |
|---|---|---|---|
| **fixed** (JOB03010/11) | `COMPLETED RC=1 MASK=00000001` | `SERVED RC=0`, same second | 276 → 283 |
| **unfixed** (JOB03021/22) | **not completed within 45 s** of a *successful* connect | **not served** in that window | **4 → 4** |
| **restored** (JOB03026/27) | `COMPLETED RC=1 ERRNO=0 MASK=00000001` | `SERVED RC=0`, same second | advanced |

**The wedge itself is NOT the defect, and the round confirms that first.** With
W parked, `F NSFS,STATS` read **`BUSY=1 BUSYSLOT=0 INFLIGHT=2`** — W holds the
single private NSFRQE and V's slot is taken but unserved — and `COLLISIONS=1`,
which independently proves V's claim scan found W's slot occupied, i.e. V really
did publish. That is serialised service (ADR-0042 §10) and it happens on **both**
modules. What the fix changes is whether the wedge can ever **lift**.

**The unfixed arm is decisive because the stimulus provably occurred:**
`Ncat: Connected to 192.168.200.1:3013` — W's listener was live and the readiness
edge really happened — and **45 s later neither W nor V had completed**, against a
fixed module that completed **in the same second**.

**That is the measurement, and it is bounded.** What was observed is *not within
45 seconds*; **"never" is the deduction** — `nsfsel_on_notify` re-scans the stored
array and residue cannot match any socket, so nothing can ever complete it. The
argument is sound and it is not an observation. Carried into §"does NOT
establish" below, where the rest of the record's limits live.

### C3 — the control, satisfied three ways

"V was not served" and "the STC died" produce the same silence.

1. `F NSFS,DISPLAY` issued **after** W parked answered `NSF800I/801I/802I`.
2. `F NSFS,STATS` answered throughout the wedge window.
3. Strongest, because it is a counter rather than a reply: across the 45-second
   unfixed window **`EVTPASSES` moved 616 → 784** while `SERVED` stayed at 4. The
   executive is alive and looping; it simply cannot serve anyone.

---

## Findings

### 1. A stale deploy, caught by an IMPOSSIBLE value rather than an identical one

The first unfixed attempt returned `RC=-1 ERRNO=22` (`NSF_EINVAL`) **immediately**
instead of parking. `EINVAL` is only producible by the **fixed** dispatcher's
non-multiple rejection; the unfixed one has no `EINVAL` path at all (the only
other `RQ_SELECT` error is `EOPNOTSUPP`, `src/nsfreq.c:920-926`). So the running
NSFS was **fixed** while the client was **reverted** — a mixed build.

Forcing the recompile (`touch` + `make modules`, which then showed
`[cc370] src/nsfsel.c`) and redeploying produced the predicted park.

**This is a new shape of CLAUDE.md §5's most expensive failure class.** §5's tell
is *identical values across supposedly different builds*. Here the tell was the
opposite: **a value neither pure build can produce.** A mixed build has its own
signature, and it is louder than the documented one — but only if you know what
each pure build is capable of returning, which is why the source enumeration of
error paths was what resolved it.

Not fixed here, and not diagnosed beyond the fact: `make deploy` after an
in-place source edit did not always recompile before packing. Recorded rather
than chased, because forcing the recompile is a one-line habit and the round's
subject is elsewhere.

### 2. `role_a`'s final assertion is structurally always-false (pre-existing)

`A: its OWN socket still works after B (got -1, want 0)` fails, and **always
has**: `tcp_listen` returns `NSF_EINVAL` unless the TCB is `TCP_CLOSED` —
*"only a fresh socket may listen"* (`src/nsftcp.c:1959`) — and `role_a` re-issues
`nsf_listen` on the socket it already listened on, with no intervening close.
`do_listen` passes that rc straight through.

Module-independent and reproduced on two separate fixed-module runs (JOB03008,
JOB03030). It is **not** a product defect: refusing a second `listen` is
deliberate, since re-initialising the acceptq would discard queued children.

**Why it surfaced only now:** A's result appears to have never been read. #100's
round submitted B after A had already ended, and the d1c record quotes B's counts
(12/13 → 9/13 → 12/13) and never A's. A job whose CC nobody looks at is §8.5 one
level out.

**Reported, not fixed** — it is unrelated to #101 and changing another gate's
assertions inside this round is the Kitchen-Sink pattern. The fix is to assert
the documented behaviour (a second `listen` is refused `EINVAL`) or to close and
re-listen; that is a decision for whoever owns d1's gates.

### 3. B's own range self-check fired, and was right

One arm-1 run reported `RANGE INADEQUATE -- SWEEP 1 IS NOT EVIDENCE` and refused
to report. Correct: that STC instance had served several earlier jobs, so slot
generations had advanced past the sweep's gen-0/1 window and B's own descriptor
fell outside the swept range — the exact condition under which a zero means
nothing. This is #100's defect-2 repair working on a real occurrence, for the
second time on record. Re-run on a fresh STC: 13/13.

**Operational consequence worth knowing:** arms 1 and 2 need a **freshly started
NSFS**, because the sweep's evidence depends on the generation range.

### 4. A falsification clause is a claim, and needs the same check as its prediction

The prediction this round amended was countersigned, and a correction written a
round earlier had already fixed its **mood** — recording that the wedge was a
deduction and not an observation — while leaving its **content** unexamined. What
was wrong was not the prediction, which was defensible; it was the clause naming
the conditions under which it would be abandoned.

Had the arm run as written, it would have produced "served in neither", and the
falsification clause would have read that as *"something other than `g_busy`
holds it"* — **a false conclusion from a true observation**, in a round whose
whole purpose is to settle the question.

> **A falsification clause is a claim and needs the same check as the prediction
> it guards.** Writing predictions before runs is the discipline; it does not help
> if the conditions under which the prediction would be abandoned are themselves
> unverified.

Recorded here and in ADR-0047 §8, **not promoted** into CLAUDE.md §8.5 — where it
belongs is a convention call, and it is Mike's. It supersedes nothing; it sharpens
the rule this project already carries, that *a chain read out of source is a
prediction until a run*. That one is about the claim; this one is about its guard.

### 5. An instrument gap of mine

`role_w` printed `RC` but not `ERRNO`, so the first anomalous result could not be
read at all and cost a redeploy to instrument. Fixed in the same file; the
`ERRNO=0` in the restored-arm line is that fix. A failure whose errno is not
printed is a result nobody can read.

---

## Round hygiene

- **Zero dumps** (`IEA995I` = 0) against **2** `IEF450I ... ABEND S222` — exactly
  the two deliberate cancels of the wedged W and V. The non-zero cancel count is
  the positive control for the zero dump count.
- **The retain branch fired, correctly and incidentally.** Cancelling the wedged
  clients left their slots claimed, so `P NSFS` reported
  **`NSF054W 2 CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE RETAINED`** rather
  than freeing storage a client might be executing in. The next start came up on
  a **different anchor (`00ACF7C8`, was `00AAD7C8`) and a different router EP
  (`00AAD248`, was `00A8B248`)** — the evidence the old ones were retained — and
  `LARGEST FREE BLOCK` fell 933888 → **794624**, exactly −139264 = pool + router,
  matching the figure on record. **~136 KB of CSA is retained until IPL** as a
  cost of the unfixed arm.
- Final state clean: `BUSY=0 BUSYSLOT=-1 INFLIGHT=0 EXHAUSTED=0 COLLISIONS=0`.
- Every deploy was `P NSFS` → deploy → `S NSFS`, and every deploy's output was
  read for the mid-chain `HTTP 500` / `Dataset delete failed` signature. None
  appeared.
- The restored source is **byte-identical to `main`** (`git diff main` on
  `src/nsfeza.c` and `src/nsfsel.c` = 0 lines) and was force-recompiled before
  the final deploy.

---

## What this round does NOT establish

- **That W would NEVER have completed on the unfixed module.** What is measured is
  that it did not complete **within 45 seconds** of a connect that provably
  succeeded, against a fixed module that completed in the same second. "Never"
  follows from the source — the re-scan reads the stored array, and residue
  cannot match — and that is a deduction, not the observation.
- **Anything host-side.** That is Stage 1's, and it is not repeated or re-claimed.
- **That the wedge is the only consequence of the defect.** Arm 3 shows one
  reachable consequence in both directions; it does not enumerate them.
- **Why `make deploy` shipped a stale object** (finding 1). The fact is measured;
  the mechanism is not.
- **That `role_a`'s assertion never passed on some earlier build.** The source
  argument is module-independent and two runs reproduce it, but no archived run
  of A's CC exists to compare against.
- **The `EINVAL` refusal across the crossing.** Stage 1 proved it on the Phase-1
  drainer path; this round reached it only via the mixed build, which is not a
  configuration anyone should rely on. It remains proved on one path, and that is
  proportionate because the decision produced one path.

---

## ANNOTATION 2026-09-03 — arms 1 and 2 rest on an UNCONFIRMED stimulus

**Appended, nothing above rewritten.** This is a **scope correction to a
record**, not a new round and not a retraction: every assertion named below is
sound, every code path exercised is the right one, and **no result is
withdrawn**. What changes is what the evidence in this directory can be read as
proving.

It arrived from the other end. The `role_a` round
(`docs/measurements/m5-d1b-rolea/`) ran on a stand with **no `tun0`**, noticed
that two of B's assertions had passed vacuously, and then — checking the poll
path against the same standard — found a third. That standard applies here too,
and this round did not apply it.

### What the evidence in this directory actually contains

**Arm 3's stimulus is confirmed on the wire.** `Ncat: Connected to
192.168.200.1:3013` — W's listener, port `D1B_W_PORT`.

**Arms 1 and 2 have no such confirmation anywhere.** `Ncat`/`Connected` does not
appear in a single arm-1/2 evidence file; the only occurrence in this directory
is the arm-3 line quoted above, in this README. What the arm-1/2 spools contain
is:

```
16.57.49 JOB 3031  +TSTD1B: PARKING SELECT ON 00010000 -- CONNECT TO A NOW
16.57.58 JOB 3031  +TSTD1B: PARKED SELECT RC=0 READY=0
```

Nine seconds and a prompt. **Nothing records that the connect was made, and
nothing records that A ever became read-ready.**

### The sentence that claims more than the evidence carries

From §"P1 / P2", verbatim:

> On the parked path a readiness change on a socket B does not own does **not**
> complete B's SELECT

That presupposes **a readiness change occurred**. On this record it is not
established. `rc=0 ready=0` is equally consistent with *"A never became
ready"* — the absent-vs-succeeded shape (CLAUDE.md §8.5), sitting inside a
countersigned round.

**And the same question reaches P1's poll assertion**, which the round listed
among its positives. `tcp_poll` (`src/nsftcp.c:2142-2147`) makes a socket
READ-ready only on a non-empty `rxq` or `acceptq`, or `TCB_F_RCVFIN`. With no
connection pending, A's listener has an **empty acceptq**, so
`foreign.ready == 0` is exactly what a *resolved, idle* listener yields.
**Neither arm separates "refused" from "resolved and idle".**

### What survives, and it is the load-bearing half

**The crossing-level ownership claim rests on 2.2 / 2.2b, not on the SELECT
arms** — and 2.2 carries its **own positive control**, independent of the wire:

- **0 of 128** descriptors reached while B owned nothing;
- **1 of 128** after B had its own — and the one reached was **its own**,
  `00010001`, while A's, derived as `00010000`, was refused;
- foreign and never-existing both `rc=-1 errno=9`, indistinguishable.

None of that needs a connect. It needs A to be **holding a live socket**, which
the console marker establishes. So the round's ownership conclusion stands; what
does not stand is attributing it to the SELECT arms.

**`own.ready=2` also survives** as a real observation — the rest of the mask is
served, so a green arm is not a broken instrument — but it is a property of
**mask handling**, not of ownership.

### Arm 3 is NOT weakened

Its stimulus is confirmed on the wire, its three-state revert varied one axis,
and its result is untouched by any of the above. The bounded-measurement caveat
it already carries (*"not within 45 seconds"*; "never" is the deduction) is
unchanged.

### No re-run

Deliberately. Re-running arms 1 and 2 with a **confirmed** stimulus needs
`tun0`, hence a Hercules restart, and it is a decision for whoever schedules the
next d1 round — not a correction to be smuggled into an annotation. The
assertions are already written and already correct; only the stand was unable to
exercise them.

### The rule this yields

> **When a stimulus is missing or unconfirmed, check EVERY assertion that could
> depend on it, not only the obvious one — and a record whose stimulus is not
> evidenced claims more than it proves, however sound its assertions.**

Recorded here with its siblings — *a chain read out of source is a PREDICTION
until a run* (Stage 1) and *a falsification clause is a claim, and needs the
same check as the prediction it guards* (Stage 2 §4). Promotion to CLAUDE.md
§8.5 is Mike's convention call.

**Its first application was retroactive, and that is the argument for it
existing.** It was not derived and then applied; it was forced by a later round
noticing the shape on its own stand, and it then found a gap in a round that had
already been reviewed and countersigned. A rule that only ever fires on the
round that invented it is a platitude. This one reached backwards.

---

### POINTER 2026-09-03 -- the re-run is tracked in one place

The "no re-run" above is not a decision to drop it. Arms 1 and 2 are listed in
**`docs/measurements/awaiting-ctci-pair.md`** as item 1, with the other property
in the tree whose only missing precondition is a working CTCI pair. Nothing is
restated there that is not already above; the point is that the two do not live
as separate footnotes in separate files.
