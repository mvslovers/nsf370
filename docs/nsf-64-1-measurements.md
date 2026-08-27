# Issue #64, step 64-1 — the reset, and the experiment it makes possible

**This is a measurement record and a fix record. It is NOT a fix for issue #64,
and nothing in it should be read as one.** The reset below is an ADR-0022
violation corrected on its own merits — a permanent quarter of a host core — and
it is separately the one single-variable experiment the investigation had left,
because every stall on record occurred on a spinning instance. Whether removing
the spin removes the stall is what §5 reports, with the honesty its result
demands.

Round: MVSCE on `mvsdev`, 2026-08-27. Console times are the MVS clock (UTC-5);
host times CEST. Modules built from `m5-64-1-reset-before-drain`.

**Not read:** `nsf-64-diagnosis-memo.md` is still not in the repo and was not
supplied to this session — the fifth round in a row (64-0, 64-0b, 64-0c, 64-0d,
64-1). Every reference below is to the four measurement records that *are* in
the tree.

---

## 1. The change

`nsfsx_drain` never cleared `g_wake_ecb`. The word was assigned zero once, in
`nsfsx_start`, and never again, so the first cross-address-space POST latched
the POSTED bit for the life of the STC — and a posted ECB in the ECBLIST makes
`WAIT` return immediately, so `evt_mainloop` could not block afterwards.

Phase 1's `nsfreq_drain` has always reset `g_reqecb` before taking its queue,
and so does the probe STC (`nsfv.c`). Phase 2 was the one that diverged from the
discipline `src/nsfevt.c` step 2b states in so many words. ADR-0022 carries the
annotation; it is not repeated here.

**The diff is one statement.** A comment-stripped diff of `src/nsfsx.c` against
`main` is exactly `+ g_wake_ecb = 0u;` and nothing else — checked mechanically,
because "nothing rode along" is a claim that should not rest on reading a diff.

Phase 1's `do/while` recheck loop is deliberately **not** replicated; the three
reasons are in ADR-0022's annotation and at the call site.

---

## 2. Instruments, and one repair that was not planned

### The console was dead, and it was 64-0d's own residue

The stand's master console was stuck in `*IEA420A NO FULL CAPABILITY CONSOLES,
REASON=EXT` — the state 64-0d §3 documented as needing a **second** `ext` press
to clear, and never got one. The symptom is worth recording because it is not
the obvious one: commands entered at console `0:0009` were **accepted and
queued** (`HHC00013I '/' input entered`) and **never executed**, and mvsMF's
`restconsoles` returned a response key for each while nothing ran.
`mvslog.txt` — the 1403 hardcopy at CUU 0015 — simply stopped at 12:34.

A second `ext` at 13:31 gave `IEE143I CONSOLE SWITCH, REASON=EXT  OLD=009
NEW=009` and **flushed every queued command at once**: three `D T`s, a `D U` and
an `S NSFS`, all issued over the preceding ten minutes, executed in one burst.
So nothing had been lost, only suspended — which is its own small echo of the
condition under investigation.

An IPL was available and was **not** taken: it was not needed after the repair,
and it would have cost the HTTPD/UFSD/FTPD restarts every storage-reading
instrument in this round depends on.

### The deploy-took-effect check had to be a different shape

Every prior round proved its deploy by a **field** — `BUSY=`, `WPREG=`, the
counter count. 64-1 adds no field, so it has no check of that shape. The
substitute is the observable itself:

> **`POSTED=N` on an instance whose `SERVED` is non-zero is the proof.** Before
> the reset nothing cleared the word, so that combination was impossible.

**Its complement is ambiguous and is not treated as a check.** `POSTED=Y` means
either the reset is absent from the source *or* the deploy silently did not
take — CLAUDE.md §5's most expensive failure class. So the revert arm in §4 is
corroborated independently: the deploy output read for the mid-chain
`HTTP 500` signature, and the local artifact's SHA-256 recorded per state.

### The stall detector had to be rebuilt, and this is the round's second trap

64-0c and 64-0d detected a stall by `ASCBEJST` going **bit-identical**. That
worked because every stall on record was on a **spinning** instance, so flat
EJST meant "not running" against a ~1 CPU-second-per-second background.

**The reset destroys that detector.** With the spin gone and the STIMER
heartbeat drained by a TCP workload (ADR-0034), a *correctly idle* executive
also reads EJST bit-identical — §3 measures exactly that. Healthy and stalled
become the same reading on that instrument.

The discriminator that survives is **a published request that stays PENDING**:
healthy → the POST lands, the WAIT returns, `served` moves in milliseconds;
stalled → the POST lands, the task is non-dispatchable, nothing moves. So this
round's detector is a **conjunction** — EJST bit-identical **and** at least one
slot `req_state == PENDING` **and** `served` frozen. All of it is CSA and SQA,
read through `/.dm`, so it needs no MODIFY and pokes nothing.

### The instrument itself perturbed a measurement, and that is logged

A first state-A CPU sample read **17–20 % user**, which would have been a
serious result. The cause was mine: a detector left running, reading 64 slot
words through HTTPD **inside the guest** every 2 seconds. Stopped, the same
instance reads 0.7–1.6 %. The detector now pre-filters over the first 8 slots
before reading all 64. Recorded rather than quietly corrected, because a
measurement tool that costs 20 % of the machine is a finding about the tool.

---

## 3. Gate 1 — the spin, in three states, and the revert discriminates

The pair is controlled: **one STC instance per state, one workload
(`TSTRQXC` with no PARM — 8 sequential requests, SOLO), and the reset as the
only variable.** Idle windows have `SERVED` confirmed unchanged across them.

**The "before" arm cost zero deploys.** 64-0d §9 recorded that
`NSF.LINKLIB` still held the binary 64-0c measured, and this round confirmed it
by *field* — `NSF812I … WPREG=Y` present, `NSF813I BUSY=0 BUSYSLOT=-1` leading —
so state B below was taken on the module as it stood, with no deploy in the
path at all.

| # | state | STC | POSTED | `WAKEECB` | idle window | rate | host CPU (us) |
|---|---|---|---|---|---|---|---|
| B0 | reset absent, **no request yet** (control) | 1480 | N | `009DCD10` | 958 → 3 544 / 259 s | **9.98 /s** | **0.5–0.9 %** |
| **B** | reset absent, after 8 requests | 1480 | **Y** | `40000000` | 250 992 → 2 188 809 / 259 s | **7 482 /s** | **25.9 / 30.5 %** |
| **A** | **reset present** | 1481 | **N** | `00000000` | 718 → 3 303 / 259 s | **9.98 /s** | **0.7–1.6 %** |
| **C** | **reset commented OUT**, rebuilt, redeployed | 1482 | **Y** | `40000000` | 206 743 → 1 819 265 / 189 s | **8 532 /s** | **26.9 / 26.0 %** |
| **A′** | **restored**, rebuilt, redeployed | 1483 | **N** | `00000000` | 698 → 2 286 / 159 s | **9.99 /s** | **0.5 / 0.7 %** |

Every SOLO run in every state: **CC 0000, 8/8 PASS, all eight requests inside a
single console second** (step CPU 0.08–0.18 s). The reset costs no service.

**Exactly one assertion moves.** State C's source was checked to be
**identical to `main` at instruction level** (comment-stripped diff empty)
before it was built, so the reset was provably out rather than believed out;
and every deploy's output was read for the mid-chain `HTTP 500` `DELETE`
signature, which never appeared.

**A hash is NOT usable as deploy corroboration here, and the check that would
have relied on it was dropped.** Two builds of byte-identical source produce
load modules differing in **exactly two bytes** (a build timestamp at offset
10560) — measured, by building twice. So the SHA-256 of `build/NSFS` is a
build fingerprint, not a source fingerprint, and reporting it as evidence would
have been the round's own §8.5 instance.

### `WAKEPOSTS` now counts wake events, and the reading proves it

| state | requests served | WAKEPOSTS |
|---|---|---|
| B (latching) | 8 | **87 316**, thereafter tracking `EVTPASSES` at a constant offset of 3 755 |
| A (counting) | 8 | **8** |
| A, after `TSTRQXM` too | 28 | **28** |
| A, after another SOLO | 36 | **36** |

`WAKEPOSTS == SERVED` exactly, at every reading. **Every figure in
`docs/nsf-64-0*.md` was taken under the latching semantics**, so the ~8 500 /s
`WAKEPOSTS` rates recorded there are the latch tracking `EVTPASSES`, not wakes,
and nothing after this change is comparable to them. Documented at the counter's
declaration, where a reader making that comparison will look.

---

## 4. The floor — measured on purpose, and NOT built

This is the round's most valuable number for the decision that is still Mike's.

`TSTRQXM` was run first (TCP: sockets, connect, 9 353 bytes of short writes,
close), so the TCP timers were armed and then cancelled, and ADR-0034's
invariant — *queue empty ⟺ STIMER disarmed* — took the heartbeat away. Then
259 s of idle, `SERVED` confirmed unchanged across it:

```
T0 13:57:13  EVTPASSES=5854  WAKEPOSTS=28 SERVED=28 TMRQ=0 POSTED=N
T1 14:01:32  EVTPASSES=5855  WAKEPOSTS=28 SERVED=28 TMRQ=0 POSTED=N
             host CPU 0.2 % / 0.7 % us, 99.1-99.8 % idle
```

**One pass in 259 seconds.** 0.0039 /s, against 9.98 /s on the same instance
before the TCP workload. So after a TCP workload there is no floor at all —
which ADR-0034 predicts and 64-0 §4.1 anticipated, now measured rather than
inferred.

**Paired with service in the same section deliberately**, because
`EVTPASSES ≈ 0` read alone looks like a dead executive:

> On that same floorless instance, 8 cross-address-space requests were
> submitted at 14:02:54 and were **served and returned inside one console
> second** (`7.02.54` start *and* end), CC 0000, 8/8 PASS, `EVTPASSES`
> 5 855 → 5 872 — 17 passes for 8 requests.

**The wake works with no floor.** "No floor is needed" is now a measurement
rather than an assumption — **for the healthy case**, which is the only case
this measures. It says nothing about issue #64, whose mechanism 64-0c and 64-0d
placed outside the wake path entirely. **No floor was added, and where one
would live remains not the fix author's to choose.**

---

## 5. Functional regression

`make test-mvs --only TSTRQXM --only TSTRQXF --only TSTRQXC`, job MBTTEST
JOB02334, on the final restored module:

| test | batch | TSO | assertions |
|---|---|---|---|
| TSTRQXC | **CC 0** | **CC 0** | 8/8 |
| TSTRQXF | **CC 0** | **CC 0** | 53/53 |
| TSTRQXM | **CC 0** | FAIL CC 1 *(by design)* | **32/32** batch |

TSTRQXM's host peer verified **9 353 bytes byte-exact**, log mtime 14:16:06
fresh (64-0b's stale-log catch applied, and the listener was confirmed in
`LISTEN` before the run). The TSO re-run's two failures are `CONNECT` and its
dependent `CLOSE`: the one-shot listener was consumed by the batch run, which is
the documented TSTTCPW/TSTRQXM precedent — **batch is the gate**.

This is the regression that matters for a reset: a lost wake shows up as a
request that is never serviced, and TSTRQXM's `CONNECT` is the first **parked**
request to complete, so the parked path is exercised on the reset build.

---

## 6. Gate 2 — the reproduction attempt, and what it can and cannot support

64-0b's standing instruction was that 64-1 must **attempt** a reproduction after
the reset, because it becomes untestable the moment the reset lands. 64-0c
sharpened the arm and 64-0d named the state to look for. This section reports
what the attempt found, at the strength the evidence carries and no more.

### The arm, and the correction to the plan it needed

64-0d's arm: one instance, `TSTRQXM` first, then the **two-client contention
gate re-run back to back**, no `PARM='LEAK'` (four reproductions without it), a
client parked with a published request, and a passive detector. Every stall on
record began within ~40–90 s of a gate round starting.

**64-0e was going to be "NSFS with no `DEVICE`", and that arm is confounded** —
recorded here rather than discovered on the machine. The recipe needs
`TSTRQXM`, a TCP workload, which needs the device; and it is the TCP timers
whose cancellation destroys the ~10 Hz floor in the first place. Removing the
device removes the workload *and* the floor destruction, so a quiet run would
prove nothing. The spin is the one variable isolable with the recipe intact,
which is why this step exists.

### The detector had to be rebuilt, and that is a consequence of the fix

Stated in §2 and repeated here because it governs how this section reads: the
reset destroys 64-0c/64-0d's `ASCBEJST`-flat detector, since a correctly idle
executive with no floor (§4) reads exactly as flat as a stalled one. This round
used the conjunction — **EJST bit-identical AND a slot `req_state == PENDING`
AND `served` frozen**, all read from CSA/SQA through `/.dm`, never a MODIFY
(which would POST the cib ECB that `nsfsmain.c:316` puts in the executive's own
ECBLIST). Armed at 14:17:06 on `ASCB FF8F18` / anchor `00A8D7C8`, verified
against the anchor's eyecatcher before watching.

### What ran
**The arm ran, at scale.** One STC instance (1483, the final restored module),
`TSTRQXM` first, then the two-client contention gate **45 times back to back**
(`jcl/TSTRQXCA.jcl` + `TSTRQXCB.jcl`, A submitted first), no `PARM='LEAK'`:

| | result |
|---|---|
| rounds | **45**, every one completing in ~10 s, no timeouts |
| leader A | **42 × CC 0000**, 3 × CC 0001 |
| follower B | **45 × CC 0000** |
| requests served | `SERVED` 69 → **164 570** |
| passes | `EVTPASSES` 4 204 → 320 733 — **≈ 1.95 passes per request** |
| pool | `COLLISIONS` 670 052, `EXHAUSTED` 10 364, `REAPED` 0 |
| **stalls** | **none** |

The three CC 0001s are a **gate-internal timing assertion** — *"A was given a
slot other than 0"*, which the gate's own text calls the weaker of two witnesses
and explicitly *"NOT a second independent witness"*. In each, `collisions` still
moved by ~149 and the decisive assertion (*A was REFUSED while slot 0 was the
only free slot*) passed. Not an NSF defect.

### The detector was validated BEFORE its silence was quoted as a result

This is the section's own §8.5 exposure and it was closed rather than argued.
The whole conjunction rests on `req_state` read at `ANCHOR + 0x38 + i*2144`; a
wrong stride or offset would read FREE forever and report "quiet" straight
through a stall. Validated live, during the campaign:

```
14:29:16  census {'FREE': 49, 'CLAIMED': 15}
14:29:23  census {'FREE': 64}
14:29:28  census {'FREE': 63, 'HELD': 1}
14:29:35  census {'FREE': 46, 'CLAIMED': 18}
```

and the `PENDING` conjunct specifically, by fast-sampling slots 0–3:
**360 reads → `{FREE: 340, CLAIMED: 15, PENDING: 3, DONE: 2}`.** All four states
the detector distinguishes are readable through this exact path, so its silence
is a real null.

### The honest statement of the null, in the unit that matters

**Not "45 rounds and no stall".** Every stall on record — #64's own, 64-0b's
two, 64-0c's two, 64-0d's four — shared exactly one condition: **a client parked
with a published request.** The sampling above measures how much of this arm had
it: `PENDING` in **3 of 360 reads ≈ 0.8 %** — sampled over roughly 90 s of the
campaign, on slots 0–3 (where a small client set lands), *during the heaviest
workload the stand can produce*. That is a duty cycle for that burst rather than
a campaign-wide average, and it is the right order of magnitude either way. In
64-0d a **single** gate round held a client parked for **minutes**, because
those rounds were themselves slow.

So the correct statement is: **the condition every stall shares was present for a
few seconds in total across this whole arm.** That is a nameable weakness, not a
generic one, and it is why §7 does not treat the null as evidence.

### An uncontrolled observation, offered as exactly that

M5-2b4 recorded that its two-client gate *"crawled at ~3 requests/min until a
continuous host `ping -i 0.2` was started (the #64 latency defect)"*. This
round's 45 rounds needed no external floor and each ran 3 000 attempts in ~10 s.
That is a large difference in the same workload on the same stand — but the
rounds are months and many changes apart, nothing was held constant, and there
is no control. **Suggestive, not evidence**, and it is not counted anywhere
below.

### The idle arm, counted and labelled

| window | length | instance state | result |
|---|---|---|---|
| B0 | 259 s | fresh, no request | quiet |
| B | 259 s | post-request, spinning | quiet |
| A | 259 s | post-request, reset in | quiet |
| floor | 259 s | post-TCP-workload, no floor at all | quiet |
| C | 189 s | reset out, spinning | quiet |
| A′ | 159 s | restored | quiet |
| idle 1 | **972 s** | post-campaign, 164 570 requests behind it | quiet — `EVTPASSES` 320 733 → **320 736**, three passes in 972 s, `SERVED` frozen |

**The idle arm is known low-yield and is not offered as a fair test.** 64-0c
recorded four non-reproductions in the idle configuration across three rounds
and concluded it *"looks like a property of the idle configuration rather than a
sampling accident"*; 64-0d added more. And structurally: with no client parked,
`req_state` is never `PENDING`, so this round's detector **cannot** fire in an
idle window by construction. These windows are reported for completeness and for
their `EVTPASSES` and CPU content, which is where their value actually is.

---

## 7. What the round supports, stated at the strength the evidence carries

### The reset — CONFIRMED, and the revert proves it discriminates

The spin is an ADR-0022 violation, it cost a permanent ~26 % of a host core on
every instance that had ever served a request, and one statement removes it.
Three deployed states, one assertion moving, with the reverted source verified
identical to `main` at instruction level. Service is unaffected (8/8 inside one
console second in every state), and the full NSFS regression is green.

### The floor — MEASURED, and deliberately not built

After a TCP workload the executive makes **one pass in 259 seconds** and still
services eight cross-address-space requests inside a single console second.
**No floor is needed** for the healthy case, which is the only case measured.
No floor was added. Where one would live remains not the fix author's to choose.

### Issue #64 — NOT reproduced, and that is WEAK evidence

**64-1 does not fix issue #64, does not close it, and this round does not claim
it has been fixed.** #64 remains open.

The stall did not reappear across 45 gate rounds, 164 570 requests and seven
idle windows totalling **2 356 s** (259 + 259 + 259 + 259 + 189 + 159 + 972,
the seven tabulated in §6). That is a null result and it is weak, for a
reason that can be named precisely: **the condition every stall on record shares
— a client parked with a published request — was present for well under 1 % of
this arm's wall-clock**, where 64-0d's single slow gate round held it for
minutes. A quiet run is the expected outcome of a short experiment here even on
unchanged code.

**What is NOT weakened is the prior work's finding**, and it is what keeps this
in proportion: 64-0c measured the executive **not waiting** during a stall
(ordinary PRB, `WCF=0`, `RBXWAIT`/`RBECBWT` clear) and 64-0d measured its
address space **stuck part-way through an MVS swap-out** (`OUCBQFL = X'80'`,
`OUCBGOO`), in fields NSF does not write and cannot see. **The suspension is
outside nsf370.** The reset was never a candidate fix for it; it is a candidate
*provocation* removal, and this round neither confirms nor refutes that.

### What the round does NOT establish

- That removing the spin removes the stall. The arm did not put the stand in the
  stalling condition for long enough to say either way.
- That an idle instance cannot stall — the idle arm cannot fire this detector.
- Anything about the second candidate provocation 64-0d names: the
  indefinitely-outstanding CTCI read. The instrument for that is **NSFV**, the
  probe STC, which runs the same transport with **no device at all** — a cheap
  next round needing no code change, and untouched here.
- Whether NSF should mitigate an MVS condition at all, and in what form
  (`SYSEVENT DONTSWAP` being the obvious candidate, and a privileged,
  contract-level change). That is the maintainer's decision.

---

## 8. The NSFV round, and round hygiene

### NSFV (the Stage-0 regression)

`P NSFS` first — both STCs steal SVC 239 — then `S NSFV` and
`make test-mvs --only TSTSVC --only TSTMVCK --only TSTUBUF --only TSTDEATH
--only TSTXFW`, job MBTTEST JOB02426:

| test | batch | TSO |
|---|---|---|
| TSTSVC | **CC 0** | **CC 0** |
| TSTMVCK | **CC 0** | **CC 0** |
| TSTUBUF | **CC 0** | **CC 0** |
| TSTDEATH | **CC 0** | **CC 0** |
| TSTXFW | **CC 0** | **CC 0** |

**484 PASS / 0 FAIL** — the same figure M5-2c0 recorded. `TSTMVCD` deliberately
excluded (issue #53: it is b0's probe, not a Stage-0 gate, and fails for
environmental reasons, which in a full-green round costs a re-run and erodes
confidence — the b3/c0 precedent).

`NSFV000I`→`NSFV001I` clean start, `NSFV095I SVC 239 RESTORED` +
`NSFV036I SVC ROUTINE UNLOADED` clean stop.

### Hygiene

- **The console repair is in §2** and is the round's largest piece of friction.
  An IPL was offered by the maintainer and turned out not to be needed.
- **`NSF.LINKLIB` deploys: three** (reset in, reset out, restored). Every
  deploy's output was read for the mid-chain `HTTP 500` `DELETE` signature and
  none occurred. Every `make deploy` was preceded by `P NSFS`.
- **CSA gave nothing away.** `NSF055I` read **1 044 480** at every one of the
  four `S NSFS` starts (STC 1480/1481/1482/1483) — constant across the round.
  Every `P NSFS` and the `P NSFV` was clean: `NSF043I SVC 239 RESTORED`,
  `NSF044I`, `NSF011I`, **no `NSF054W` retain branch**. `PARM='LEAK'` was **not**
  run — it was not part of the arm (four 64-0d reproductions without it) and its
  ~137 KB would be retained to IPL for no measurement.
- **Zero dumps.** `IEA995I` / `SYS1.DUMP` count is 0 for the whole log.
- **`IGF991I` / `IGF995I` on device 500** at 6.49.09 — the familiar CTCI-pair
  degradation after sustained use, not a new symptom.
- **FTPD (STC 1476) logged recovered `S0C1`/`S0C6`/`S0C4` worker abends** at
  7.26–7.27 on `CMD=QUIT`. Unrelated to NSF, in another address space, recovered
  by FTPD itself; recorded as stand background rather than passed over.
- **Host suite `make test-host`: 2925 PASS / 0 FAIL, 27 tests**, before and
  after — a **no-regression check only**, and evidence of nothing else, since
  `src/nsfsx.c` is MVS-only and never compiles on host.
- **Stand left with NSFS and NSFV both stopped**, and **TESTLIB holding
  `TSTDEATH` / `TSTMVCK` / `TSTSVC` / `TSTUBUF` / `TSTXFW`** — the last `--only`
  run replaced it, so the next round must re-deploy `TSTRQXM` / `TSTRQXC` /
  `TSTRQXF` rather than assume they are there (the b4 `S806`).
- **The detector was armed 14:17:06 → 14:48:22 (31 m 16 s)** across the campaign
  and idle window 1, and emitted **nothing** — no stall, and no false positive
  either, which matters because a naive EJST-flat detector would have fired
  almost continuously against the floorless executive.

---

## 9. What this round did not do

- **No floor**, of any kind, and no `nsftmr` change.
- **No ADR-0043.** The wake contract — who posts, what resets, what the floor is,
  and what happens when the executive's liveness depends on something outside
  nsf370 — is Decision 2 and remains Mike's, to be taken once #64's mechanism is
  established. ADR-0022 carries an annotation, append-only, because the *reset*
  is unambiguously that ADR's subject.
- **It did not close, or claim to have fixed, issue #64.** The issue was found
  `CLOSED (COMPLETED)` at round start — closed again five hours after its own
  reopen — and was **reopened** with a comment saying plainly that nothing has
  fixed it.
- It did not test the second candidate provocation (the indefinitely-outstanding
  CTCI read). **NSFV** — the probe STC, same transport, no device at all — is the
  cheap instrument for it and needs no code change.
- It did not read the OUCB or any SRM state: no stall occurred to read one from.
