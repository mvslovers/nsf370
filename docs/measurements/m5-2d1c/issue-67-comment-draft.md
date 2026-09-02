# Proposed comment on issue #67 — NOT POSTED

CLAUDE.md §8 puts `gh issue edit` under the confirm-first rule: the audit trail
cannot tell us apart, so a state change nobody intended is unattributable
afterwards. Drafted here for Mike to post or amend.

---

## The rejection came forward from c3 — the issue stays open

M5-2d1c. Pulled out of c3 for a reason that is **not** the security one
this issue was filed for: **our own rounds keep walking into it.** M5-2d1's
second live round (#100) issued `XFER` at NSFS, parked forever, and cost a
cancelled job, a leaked slot and an anchor plus router retained to IPL — in the
round that had been told not to fix this. Between then and c3 sit the `ulen` fix
and (e), and each can walk in again.

## What changed

The SVC routine now refuses **ahead of the claim**, so a refused request costs
**no slot and no in-flight count** — true by position rather than by argument,
the form M5-2c2 established for the retired `FNORPH`. The rc reaches the
**caller's block** (`BADFUNC`, not `BADREQ`'s R15-only), so a client cannot read
back its own initialised value and take it for "the SVC never ran".

**It permits one verb rather than refusing two by name, and that is a widening of
this issue's own analysis.** The "Which verbs" section above lists `ECHO`,
`XFER` and `ORPHAN`. Tracing the staging dispatch at `CLAIMOK` shows the list is
incomplete: it is a **fall-through chain ending in ECHO**, so

```
L R3,REQFUNC(,R8)
C R3,=A(FNXFER)  -> XFERIN
C R3,=A(FNRQE)   -> RQEIN
[fall through]   -> ECHO staging
```

**any `REQFUNC` the routine does not recognise** stages as `ECHO` and hangs
identically. That is the *cheapest* instance of this issue — one wrong word in a
client, no knowledge of the verb set needed — and refusing `ECHO` and `XFER` by
name would have left it open. The production STC therefore permits exactly
`FNRQE`.

`QUERY` / `UNSTAGE` / `SLOT` remain unaffected, as this issue already records:
they branch out of the chain above the insertion point and take no slot.

The gate is a new anchor capability bit, `NSFV_ANCHOR_PROBE`, in the existing
`flags` word — set by the probe STC and by nothing else, so a zeroed anchor
refuses. Fail-closed by polarity. No field moved, so `NSFV_ANCHOR_VER` stays 3.

## What remains — the issue stays OPEN

**`ECHO` and `XFER` still strand a slot each at the probe STC, which services
them.** A parked task still costs exactly one slot and hangs itself, and the CSA
retain path still follows if a parked client's address space is gone while it is
parked. Nothing about the `HELD` mechanism in `src/nsfsx.c` is fixed — it is now
unreachable from a client at NSFS, which is not the same as correct: a slot set
`HELD` there is still never re-examined.

Removing the remaining probe verbs is still **c3, after (e)**, and c3 deletes this
rejection together with the verbs it protects against. Obligation #4 is still
discharged in substance for the **identity half only**.

## Evidence

Offline: `as370` listing checked (`TM ANCFLAG(R2),ANCPROBE` = `9140 200C`, base
R2 not dropped to 0; `C R3,=A(FNRQE)` = `5930 6540` with the literal reading
`00000006`; every branch target matching its label), all 1368 source cards
present in source order, and the instruction stream differing from `main` by
**exactly the four inserted statements** — 274 identical, 90 by uniform
displacement shift, 0 by anything else. All three assembler gates verified to
discriminate against a deliberate column-72 overrun.

Live: **run and green.** `TSTRQXF` section (E), on MVSCE, from an unauthorised
client — `ECHO RETURNED rc=4 (it did not park)` and **`REQFUNC 99 RETURNED
rc=4`**, the fall-through case, with `inflight 0->0` and no slot changed state,
in both the batch and the TSO run; `TSTRQXF` 122 → **130 PASS / 0 FAIL**, the
delta being exactly the four new assertions across the two runs. The conditional
is shown on one binary with one axis varied: the same `ECHO` is **serviced** at
NSFV (`TSTSVC` green, 438 PASS across that set) and refused at NSFS. No
before/after arm was run — the prior behaviour is on record from #100's round and
re-inducing it costs a cancelled job, a leaked slot and an anchor retained to IPL.
Zero dumps; `SVC 239` restored by both STCs; stand left as found.
