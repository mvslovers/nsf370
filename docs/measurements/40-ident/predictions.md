# 40-IDENT — predictions, recorded BEFORE the live arms

Written after the offline reading (AMODGEN macros, the DSECT gate, and the
re-reading of 64-3-0's own survey output) and **before** any arm ran. The
offline facts that inform them are listed first so the predictions are not
mistaken for guesses about things already known.

## Established offline, before any prediction

1. `ASCBJBNI` (+`X'AC'`) and `ASCBJBNS` (+`X'B0'`) are **`DS A` — pointers**,
   not inline character fields. Both offsets are in the **IFOX00-proved**
   control set, not derived.
2. The macro's own comments name the client classes:
   `ASCBJBNI` = "POINTER TO JOBNAME FIELD FOR **INITIATED PROGRAMS** OR ZERO";
   `ASCBJBNS` = "POINTER TO JOBNAME FIELD FOR **START/MOUNT/LOGON** OR ZERO".
3. **No `ASSB` and no `STOKEN`** anywhere in `IHAASCB` (count 0, with a
   positive control). There is no architected per-instance address-space
   identity on this system.
4. `GDA` `PASTRT` (+`X'10'`) / `PASIZE` (+`X'14'`) bound the private area —
   the test for whether a pointer target is readable from NSFS at all.
5. 64-3-0's survey already resolved ASID 6 / ASCB `FE7B58` — **stage a's exact
   initiator** — to the jobname **`INIT`**, with `ASCBFMCT = 0` (idle, no job
   running). `aslist.py` takes the **first non-zero** of JBNS then JBNI, so
   that reading does not say which pointer supplied it.

## Predictions

**P1 — while a job is running, the two pointers differ, and JBNI carries the
job.** `ASCBJBNS` resolves to `INIT` (the initiator is itself a started task)
and `ASCBJBNI` resolves to the **job's** name. Basis: the macro's own split in
(2). If this fires, `aslist.py`'s first-non-zero order has been **masking**
JBNI behind JBNS for every initiator it ever printed.

**P2 — when the initiator is idle, `ASCBJBNI` is zero.** The macro says "OR
ZERO", and 64-3-0 saw `INIT` at `FMCT = 0`. So the field is expected to track
job residency rather than to hold a stale name.

**P3 — the same JCL submitted twice is NOT distinguishable at this level.**
Two runs of one job name give the same eight characters, and nothing else at
ASCB level carries a per-submission identity (3). If P3 fires, a jobname-based
identity is **unsound** and must not be built — this is the discriminating
case and it is run explicitly, not inferred.

**P4 — readability is open, and it is the question that can kill the
direction.** Whether `ASCBJBNI`'s target is common storage or the initiator's
private region is **not** established by anything read so far. If it is
private it is unreadable from NSFS (ADR-0039: no cross-memory move on this
target), and the direction ends there regardless of P1–P3. Prior rounds'
successful dereferences are **not** evidence for JBNI: every name 64-3-0
resolved was a started task, which is the JBNS path.

## Against the kickoff's three

- **I(i)** needs P1 **and** not-P3 **and** not-P4-private.
- **I(ii)** is what P3 alone produces: something distinguishes job from
  initiator, but nothing distinguishes submission from submission.
- **I(iii)** is what P4-private produces: a signal exists but cannot be
  reached, or can be reached only through a chase that may fault.

P3 and P4 are independent, so **I(ii) and I(iii) can both hold at once**.
