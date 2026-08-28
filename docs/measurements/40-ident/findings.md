# 40-IDENT — what a recorded identity can actually distinguish

**Stand:** MVSCE on Hercules (`mvsdev`), NSFS STC 1534 (module as deployed by
64-3-1; nothing was rebuilt or deployed this round). Read-only apart from the
jobs and started tasks the arms submit, and one new `SYS2.PROCLIB` member.
Times are the console's unless marked Z.

**Answer, in one line:** something changes, and it is not an identity. Both
predictions **I(ii) and I(iii)** are supported; **I(i) is not**.

---

## 1. What was read offline, before any arm ran

All from `SYS1.AMODGEN`, downloaded live this round, through 64-3-0's DSECT
gate (`cblayout.py`) unmodified. **Fetch control:** the live `IHAASCB` is
byte-identical to 64-3-0's capture apart from one trailing blank line.

| gate | result |
|---|---|
| `IRAOUCB` positive control | **17/17** reproduce |
| `IHAASCB` positive control | **13/13** reproduce |
| `IHAGDA` positive control (`CSAPQEP`=`X'08'`, proved by 64-3-0) | **1/1** reproduces |

Derived and used: `GDA PASTRT` `X'10'`, `PASIZE` `X'14'`. `ASCBJBNI` `X'AC'`
and `ASCBJBNS` `X'B0'` are **in the IFOX00-proved control set** — not derived
at all.

**Three facts that shaped every arm:**

1. **`ASCBJBNI` / `ASCBJBNS` are `DS A` — POINTERS**, not inline character
   fields. Whatever they address has to be classified before it is read.
2. **The macro names the two client classes itself:** `ASCBJBNI` = "POINTER TO
   JOBNAME FIELD FOR **INITIATED PROGRAMS** OR ZERO"; `ASCBJBNS` = "POINTER TO
   JOBNAME FIELD FOR **START/MOUNT/LOGON** OR ZERO". The batch/STC split is
   architected into the control block.
3. **No `ASSB` and no `STOKEN` anywhere in `IHAASCB`** — count 0 with a
   positive control (`ASCBJBNI` found twice in the same grep). The architected
   per-instance address-space identity of later MVS **does not exist here**.
   That is most of prediction I(ii), established without touching the stand.

An error caught by the gate rather than by a test: `ASCBFMCT` is **`DS H`, a
halfword**. Read as a fullword it gives 589824 frames on a 4096-frame machine.
64-3-0 had it right; the first version of this round's tool did not.

---

## 2. §3.1 — the layering rule, recorded because any later design inherits it

`ufsd#asv.c:8-13` declines to read anything out of the ASCB, and its reason is
specific: the pointer comes from CSA, may be stale, and reading a jobname out
of a reused block "would answer with another address space's data". **That
hazard is about an ASCB that is gone. Ours is alive, hosting a different job.**

So: **any candidate field is read only after the existing ASVT-membership
check has established the ASCB is real.** Under that ordering ufsd's hazard
cannot arise, and a job identity becomes an extension of the proven pattern
rather than a replacement for it. This probe followed the ordering itself, not
merely recorded it for later.

---

## 3. Arm 1 — three jobs in one initiator

### 3.1 The boundary, and the control on it

```
CVT 01D048   GDA FFFFC0 (CVT+X'230')
PASTRT 090000  PASIZE 00950000 (9536 KB)   private area = [090000 .. 9E0000)
CONTROL: GDA+8 CSAPQEP -> PQE FFFF50  PQESIZE 2113536 = 2064 KB  -> REPRODUCES
         64-3-0's independently measured CSA total.
```

A GDA read at a wrong address would not land on a PQE describing exactly the
size a previous round measured by a different route.

### 3.2 P1 fires — and `aslist.py` has been masking it

With `TSTAPPDH` (JOB02824) running, initiator ASID 8:

```
ASCB FD0F18  ASID 0008  FMCT 109
  JBNS ptr=FE8488  COMMON   'INIT    '
  JBNI ptr=FF8F58  COMMON   'TSTAPPDH'
```

`JBNS` is the initiator's own name and never changes. **`JBNI` carries the
job.** Both targets are in **COMMON** storage, so both are readable from NSFS.

`aslist.py` takes the **first non-zero** of (`JBNS`, `JBNI`), so it will report
`INIT` for an initiator **whenever a job is running** — the field this round is
about, hidden by ordering alone. **That is a property of the tool, and 64-3-0's
survey was not affected by it:** its initiator rows all show `FMCT 0`, i.e.
idle, and on §3.3 an idle initiator has `JBNI == 0`, so first-non-zero would
have returned `INIT` whatever the order. The masking is real and the specific
prior reading it would have spoiled has not been found.

**Positive control on the dereference**, as required: the name read back
matches the job known to be running (`TSTAPPDH` = JOB02824's jobname) and
differs from NSFS's own (`NSFS`).

### 3.3 P2 fires — the field tracks residency, not history

Across the cancel of JOB02824, sampled every ~4 s:

```
09:47:29Z  JBNI ptr=FF8F58  'TSTAPPDH'
09:47:48Z  JBNI ptr=000000  ZERO          <- and stays zero
```

Idle initiator ⇒ `JBNI == 0`. Confirmed again across the end of JOB02825.

### 3.4 P3 fires — **the discriminating case, run explicitly**

The same JCL submitted twice, into the same initiator:

| run | jobid | ASID | ASCB | JBNI ptr | name |
|---|---|---|---|---|---|
| 1 | JOB02824 | 0008 | FD0F18 | **FF8F58** | `TSTAPPDH` |
| 2 | JOB02825 | 0008 | FD0F18 | **FF8F58** | `TSTAPPDH` |

**Byte-identical in every observable field.** A jobname-based identity — or a
(jobname, pointer) pair — cannot tell two submissions of one JCL apart. **It
is unsound and must not be built.**

### 3.5 The pointer is an allocation artifact, not an identity

Two further runs of the *same* PROC:

| run | jobid | JBNI ptr |
|---|---|---|
| 3 | JOB02834 | **FF9390** |
| 4 | JOB02835 | **FF8F58** |

So the pointer **repeats across different submissions** (1, 2) and **differs
for the same job name** (3). It fails as a discriminator in both directions,
and the second direction is the dangerous one: a design keyed on the pointer
would read "changed" for a job that had not changed — a false DEAD, the
**unsafe** direction. Worth naming because "the pointer moved" is exactly the
sort of thing that looks like an identity.

### 3.6 I(iii): the per-submission identity exists and cannot be reached

The JES job number distinguishes JOB02824 from JOB02825. Reaching it means
`ASCBASXB → ASXBFTCB → TCBJSCB → SSIB`. Measured:

```
ASCBASXB = 9DF300   ->  PRIVATE  (inside [090000 .. 9E0000))
```

and it is not merely private, it is **aliased**:

```
ASXB addresses shared by MORE THAN ONE address space:
  9DF300  <- JES2, TSO, UFSD, NET, INIT, INIT, INIT, FTPD, HTTPD, NSFS
```

**Ten address spaces report the identical virtual address.** From NSFS the
chain is unreachable (ADR-0039: no cross-memory move on this target —
`MVCP`/`MVCS` take `S0C1`). From the instrument it is worse than unreachable:
`/.dm` runs inside HTTPD, so dereferencing it returns **HTTPD's** ASXB every
time, and the answer would look perfectly plausible. The classification step
is what prevented that, and it is the reason it exists.

`*MASTER*` is the one address space whose ASXB is COMMON (`01C258`, in the
nucleus) — a useful reminder that "all private" is itself an assumption.

### 3.7 A single unreproduced observation, reported as such

One early sample read `JBNI ptr=FF8F58` with a **non-printable** 8 bytes. If
real, that is an *unsafe*-direction hazard: a reader would see "the name
changed" and conclude DEAD for a live client. It was chased and **not
reproduced**:

| transition | samples | result |
|---|---|---|
| IDLE → name (job start) | 362 @ ~0.25 s | 12 IDLE then 350 × `TSTAPPDH`, clean |
| name → IDLE (job end) | 345 @ ~0.25 s | 16 × `TSTAPPDH` then 329 IDLE, clean |

The likeliest explanation is the **instrument**, not MVS: the tool reads the
ASCB and then the pointer target in **two separate `/.dm` round-trips**, so a
sample can straddle a job transition. It is left on the record as an
observation, not a finding — but any in-STC reader also loads the pointer and
the bytes non-atomically, so a design must tolerate a torn pair regardless.

### 3.8 A null that was checked rather than believed

Two short jobs (`TSTAPPDC`, `TSTAPPDL`) produced **576 samples, all IDLE**
across all three initiators, and a full-ASVT sweep produced **38 passes, zero
hits** — while the jobs demonstrably ran (CC 0000). That contradicted §3.2, so
the instrument was put under a **positive control**: the long job was
re-submitted and the same sweep saw it in **32 of 33 passes**. The instrument
works; short jobs simply have an initiator residency below the ~2 s sweep
period. Without that control the honest-looking conclusion would have been
"the field is not populated for short jobs", which is false.

(Two earlier submits also failed outright — a relative JCL path against a
changed working directory. The poller reported all-IDLE, which was a **true**
null about a job that never ran. Both are the §8.5 shape.)

---

## 4. Arm 2 — a dying STC, which nothing in this tree had ever watched

`jcl/TSTAPPDS.jcl`, installed as `SYS2.PROCLIB(TSTAPPDS)` — the **same
program** the batch arms run, started with `S` instead of submitted, so the
arms differ only in how they are started.

**An STC has its own address space**, as required: `ASCB=00FF8D00 ASID=000C`,
a new ASID, not one of the initiators. Every reading below is cross-checked
against the ASCB/ASID the program WTOs before it ends.

### 4.1 The alive control, without which "DEAD" means nothing

```
NSF815I   SLOT  7 TOKEN=00010007 ASCB=00FF8D00 ASID=000C LIVE
ASCB FF8D00  ASID 000C  FMCT 102
  JBNS ptr=FF8F60  COMMON   'TSTAPPDS'
  JBNI ptr=000000  ZERO
```

This also confirms §1(2) **from the other side**: an STC populates `JBNS` with
its own name and leaves `JBNI` zero — the exact mirror of an initiator running
a job. The architected split is measured on both classes.

### 4.2 DEAD, within one second — both death modes

`C TSTAPPDS`, ASVT entry for ASID 12 sampled every ~4 s:

```
12:20:53  asvtenty[11]=00FF8D00  avail=False  -> LIVE     (control, before)
12:20:54  asvtenty[11]=80FDB048  avail=True   -> DEAD     (and stays DEAD)
```

The availability bit is set and the ASCB word is replaced by a free-chain
pointer, so **both** of ADR-0040's DEAD rows fire. Read through the guard's
own arithmetic:

```
NSF815I   SLOT  0..6  ASCB=00FD0F18 ASID=0008 LIVE     <- batch, in an initiator
NSF815I   SLOT  7     ASCB=00FF8D00 ASID=000C DEAD     <- the STC
NSF816I APP REGISTRY: 8 OF 16 SLOTS IN USE, 1 DEAD
```

**Both classes side by side in one report**: the guard cannot fire for the
batch class and fires correctly for the STC class — the class M6 needs, since
HTTPD and mvsMF are both STCs. A **normally ended** STC (`P=LEAVE`,
`IEF404I ... ENDED`, no TERMAPI) read DEAD too.

So the guard **fires correctly for an STC — inside a window §4.3 measures and
does not bound.** Stated that way deliberately: "the sweep's premise holds for
its real target class" is true of this reading and is **not** a green light,
and §4.3 is the half that says why.

### 4.3 But the DEAD verdict is NOT STABLE — and this is the round's sharpest result

Both STC runs reported the **same** `ASCB=00FF8D00 ASID=000C`: MVS reused the
ASID *and* the ASCB block at the identical address. Starting a third STC:

```
before: SLOT 7 ... DEAD   SLOT 8 ... DEAD    2 DEAD
after:  SLOT 7 ... LIVE   SLOT 8 ... LIVE    0 DEAD
```

The slots were **not reaped** — no sweep exists yet — they were **reclassified
LIVE**. Their clients are provably dead (one cancelled, one ended normally,
both witnessed on the console).

**ADR-0040's ASID-reuse row does not catch this**, because that row compares
the ASCB *address*, and the address was reused unchanged. So:

> A DEAD verdict is a statement about **what occupies that ASID right now**,
> not about what happened to the recorded client. It is correct only inside
> the window between the client's death and the next occupant of its ASID —
> and nothing bounds that window. Here it was **under a minute**, on the very
> next start of the same PROC.

The failure direction stays the safe one (false LIVE leaks; it never tears
down a healthy client). But a sweep is **racing a reuse window it does not
control**, which is a far stronger constraint on c1 stage b than the rate
limit stage a was worrying about.

### 4.4 The unifying result

The batch and STC cases are the same defect at two scales:

| class | what happens to the recorded pair |
|---|---|
| batch | the identity **never dies** — the initiator outlives every job |
| STC | the identity dies, then is **resurrected** by ASID+ASCB reuse |

In both, a recorded `(ASCB, ASID)` **stops denoting what it was recorded
for**. Stage a found the first; this round found the second, and the second is
the one that was assumed safe.

---

## 5. The predictions, quoted as written

- **I(i)** — "a field or combination distinguishes job N from job N+1,
  **including the same-name-twice case**. Both halves get a candidate
  mechanism, and the next artifact is a design memo, not code."
  **NOT SUPPORTED.** §3.4: runs 1 and 2 are byte-identical.
- **I(ii)** — "nothing at this level distinguishes them. Batch clients are
  undetectable; TERMAPI is their contract; the operational answer is
  `P NSFS` / `S NSFS`."
  **SUPPORTED, with one correction.** Nothing distinguishes *submission from
  submission* (§3.4) and there is no STOKEN to fall back on (§1.3). But it is
  not true that *nothing changes*: `JBNI` tracks residency exactly (§3.2,
  §3.3). What is absent is an **identity**, not a signal.
- **I(iii)** — "something distinguishes, but only through a chase that can
  fault or that ufsd's reasoning forbids. Report the hazard and stop."
  **SUPPORTED.** §3.6: the job number exists, and the chain to it is private
  and aliased ten ways.

This round's own `predictions.md` anticipated the pair ("P3 and P4 are
independent, so I(ii) and I(iii) can both hold at once"); P1, P2 and P3 fired
as written, P4 resolved to *common and readable* for `JBNI` and *private and
aliased* for the job number.

**Not anticipated by anyone:** §4.3. The kickoff expected arm 2 to return
"DEAD, promptly → the sweep's premise holds" or "anything else → stop and
report". It returned **both**: DEAD promptly, and then not DEAD.

---

## 6. What this round does NOT establish

- **Any design.** Nothing was built beyond arm 2's PROC. Whether a `JBNI`
  comparison is worth having, and under what safety argument, is not settled
  here.
- **How long the ASID-reuse window is in general.** One measurement, under a
  minute. **And this stand is the fast end of the range**: three initiators and
  an almost-empty STC ASID range mean a freed ASID is reused about as quickly
  as it can be. A busier system would give a *longer* window — which is the
  dangerous direction, because a sweep would then look reliable under test and
  fail in production. Nothing here bounds it, and a bound is what a sweep needs.
- **That the non-printable transient is real** (§3.7) — one observation, not
  reproduced in 707 tightly-sampled transition samples.
- **Whether `JBNI` is stable under paging or swap-out.** All samples were
  taken with the field's owner resident or swapped in; `FMCT` was seen going
  to 0 while a job ran, but `JBNI` was not tracked across a full swap cycle.
- **TSO.** An STC was measured; a TSO user is a third class and was not.
- **Anything about #64, #79 or #80.** Untouched.

## 7. Housekeeping

The round leaked 10 app slots of 16 across both arms (every LEAVE and every
CANCEL costs one — that is what the arms are). `P NSFS` / `S NSFS` reset the
registry, **verified**: STC 1538 came up `0 OF 16 SLOTS IN USE, 0 DEAD`.

The shutdown **drained** — `NSF043I SVC 239 RESTORED`, `NSF011I`, `IEF404I`,
**no `NSF054W`** — and the restart reported `NSF055I ... LARGEST FREE BLOCK
NOW 1073152` — **identical to the reading STC 1534 took at its own startup
03.52.18, before this round began** (no pre-round sample was taken this
session; the comparison is against that STC's own line). So **no CSA debt and
no IPL owed**. This is why the arms deliberately used `HANG`/`LEAVE`/`CLEAN` and
never `PARK`/`PARKA`: a client parked on a request would have leaked
`inflight`, forced the retain branch, and cost an IPL.

**Zero dumps.** Stand left as found, NSFS running, registry empty.

**`SYS2.PROCLIB(TSTAPPDS)` is left INSTALLED on purpose** — it is not round
scaffolding. It is the only rig in this tree that produces a real dying address
space, which c1 stage b needs for its gate and c2 needs to retire `ORPHAN`.
`test/mvs/tstxfw.c`-style tooling aside, nothing else here can do that.
