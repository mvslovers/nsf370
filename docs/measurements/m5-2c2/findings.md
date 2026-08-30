# M5-2c2 stage a — what retiring `ORPHAN` actually costs

**Status:** mapping round. **Nothing was deleted, no verb retired, no field removed, no
test scenario removed, no recommendation made.** `asm/nsfvsvc.asm` untouched, anchor layout
unmoved, `ANCVERNO` 3, `NSFRQE` frozen at 64 B.

Host baseline **3342 PASS / 0 FAIL**, run first (so a moved baseline could not be mistaken
for a consequence of this round) and again at the end.

Live: MVSCE on mvsdev, freshly IPLed. NSFS STC 1653, deployed module confirmed as c1's by
the positive check c1 established — **`NSF817I APPSWEEP` present**, which is impossible on
any earlier build. CTCI 0500/0501 up, MTU 1500, host ping 3/3 0 % loss.

---

## 0. The one-line answer

Retiring `ORPHAN` does **not** cost an induction technique — a real dying STC client
reaches the guard on both paths, and this round demonstrates it end to end for the first
time. It costs **live wiring coverage of the two rows a real client cannot reach**: row 3
and one of row 4's four branches. Row 3 is unproducible on this stand *regardless of*
`ORPHAN`, so what is actually lost there is a forged rehearsal of a row that has never been
reachable.

---

## 1. Per-row map

`nsfreqx_classify` (`src/nsfreqx.c:128-165`) is the whole truth table. "Producible" below
means: can a **real** client — one whose identity the SVC routine captured at `CLAIMOK`
from the FLIH, not one it was handed — be arranged into this row?

| row | condition | producible by a real client | logic host-side | wiring live **after** removal |
|---|---|---|---|---|
| 1 LIVE | ASID assigned, ASCB matches | **yes**, constantly | yes | **yes** — every cross-AS test |
| 2 DEAD (avail bit) | ASVT entry `AVAIL` | **yes** — 9 of 9 kills | yes | **yes** — app sweep n=8, transport **n=1** |
| 3 DEAD (ASCB mismatch) | ASID assigned, ASCB differs | **no** — 0 of 9 reuses | yes | **no**, and not coverable |
| 4a UNKNOWN | `req_ascb == 0` | **no** | yes | **no** (only `ORPHAN` drives it) |
| 4b UNKNOWN | `asvt_enty == NULL \|\| maxu == 0` | **no** — STC-side, not a client property | yes | no (never was) |
| 4c UNKNOWN | `req_asid == 0` | **no** | yes | no (never was) |
| 4d UNKNOWN | `req_asid > asvt_maxu` | **no** | yes | no (never was) |

**Row 4 is four branches, and `ORPHAN` exercises exactly one of them.** `tstd_orphan((void *)0,
own_asid)` (`test/mvs/tstdeath.c:290`) drives only 4a. 4b/4c/4d are host-pinned only and
always have been — so the row's live coverage is already 1-in-4, not 1.

### Row 1 — LIVE

Producible and continuously exercised. The guard gates **every** reply POST:
`nsfv_service` calls `nsfv_client_state` → `nsfreqx_classify` before the `__xmpost`
(`src/nsfv.c:428-458`), with the comment *"Nothing else in the STC may POST a client
without passing here."* The production STC does the same at `src/nsfsx.c:374`.

So a false DEAD on row 1 would hang the client and fail the round. **Row 1's wiring is
covered live by every test that actually crosses the boundary** — TSTSVC, TSTUBUF,
TSTXFW, TSTRQXC, TSTRQXF, TSTRQXM — none of which uses `ORPHAN`. (Pruned deliberately:
`TSTMVCK` and `TSTMVCD` are *instruction-level* probes that self-auth and never rendezvous
through the anchor — `project.toml` says of TSTMVCK "No NSF sources" — so they never pass
the guard and are not evidence here. The listed six were checked for `nsfv_svc_issue` /
the `src/nsfreqc.c` client seam, not assumed.) Within
`TSTDEATH` itself, scenario 5 (the blocking ECHO round trip) is also a real-identity LIVE
control and survives the retirement of scenarios 1–4.

Measured fresh this round: `NSF815I SLOT 2 TOKEN=00010002 ASCB=00FF8E68 ASID=000C LIVE`,
cross-checked against the client's own WTO `TSTAPPD: HANG ARM -- ASCB=00FF8E68 ASID=000C`.
Identity proven, not inferred.

### Row 2 — DEAD, available-ASID

**Producible by a real client, and the guard acts on it. Measured on both paths.**

*App-sweep path* (`nsfreq_app_classify`): 8 kills of a real STC client
(`S TSTAPPDS,P=HANG` → `C TSTAPPDS`), **8 reclaims**, every `NSF057I` carrying the
identity the client had WTO'd:

```
IEF450I TSTAPPDS TSTAPPDS - ABEND S222 U0000 - TIME=09.25.45
NSF057I APP SLOT 2 (TOKEN=00010002 ASCB=00FF8E68 ASID=000C) RECLAIMED -- CLIENT ADDRESS SPACE GONE
```

*Transport path* (`nsfsx` slot guard) — **the datum `ORPHAN` uniquely provides today, now
obtained without it, and the first time in this tree that the transport guard's DEAD path
has been driven by a real address-space death:**

`S TSTAPPDS,P=PARK` parks a real published request. Outstanding was proven by the
**conjunction** 40-CHK established, not by `PENDING` alone:

```
NSF813I BUSY=1 BUSYSLOT=0 INFLIGHT=1 ... REAPED=0        <- before the kill
IEF450I TSTAPPDS TSTAPPDS - ABEND S222 U0000 - TIME=09.36.35
   (ASVT entry for ASID 12 reads 80FDB048 -- AVAIL: row 2)
   (host sends the datagram that completes the parked RECVFROM)
NSF050I CLIENT DEAD (ASCB=00FF8E68 ASID=000C) -- REQUEST REAPED
NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 ... REAPED=1       <- after
```

The in-flight count came back, the slot was released, `REAPED` +1 — the full reap, on a
real client, with no forged identity anywhere.

The completing datagram is required: M5-2b4 established that the drain scan **skips the
in-service slot**, so a parked request is never reaped by the periodic scan; only the
completion path's guard looks at it.

**The ledger, so the counts reconcile.** 9 real STC clients were killed. All 9 were
reclaimed by the **app sweep** (`NSF057I`), which is why the closing
`NSF817I APPSWEEP … RECLAIMED=9` reads 9. Exactly one of those 9 — the `PARK` run — also
had a published **transport** request outstanding, and that one produced the `NSF050I`
transport reap and the closing `REAPED=1`. So: app-sweep path **n = 8** plus the PARK run;
**transport path n = 1**.

**And the transport datum is n = 1, which matters because it is the path `ORPHAN`
rehearses.** The app-sweep evidence is plentiful but it is not the path `TSTDEATH`'s rows
live on. The claim "a real client can drive the transport guard to row 2" rests on a single
observation, cleanly instrumented but single.

**The rate, and why a percentage would mislead.** Every kill was classified DEAD and acted
on, the ASVT entry flipping to `AVAIL` within ~1 s of the ABEND every time. But the window
is **event-bounded, not time-bounded**: with nothing else starting, ASID 12 stayed `AVAIL`
for the full 100 s of the first watch, and returned to LIVE only when another address space
was deliberately started. So the check races **address-space starts**, not wall time, and a
rate quoted without the start rate attached is not transferable.

**For the transport path it is worse than that, and this is the sharper form of the same
insight: the transport guard has no period at all.** The sweep looks every 10 s; the
transport guard looks *when the request completes*, which is the client's or the peer's
business and is unbounded. In this round's own datum there were **~16 s** between the
`ABEND S222` (9.36.35) and the `NSF050I` (9.36.51), and the guard looked only because a
datagram was sent. Had another address space started in that gap, the identity would have
resurrected to LIVE and the STC would have POSTed into a dead address space — 40-CHK's
permanently leaked slot. So the transport exposure is **completion latency vs. start rate**,
with completion latency unbounded, not "sweep period vs. start rate".

Direction, carried from `m5-79` rather than re-derived: fast reuse means the check **misses
more**, and this stand is the **pessimistic** case (three initiators, near-empty STC ASID
range, LIFO free chains). A hit rate measured here is a **floor**.

### Row 3 — DEAD, ASID reused with a different ASCB: **not produced, not once**

Every reuse observed restored the **identical** pair. Across 9 separate STC starts the
client's identity was `ASCB=00FF8E68 ASID=000C` every single time. The continuous trace
(`resurrect.log`) is the clean statement:

```
  0.1s  entry=00FF8E68 ascb=FF8E68 -> LIVE                (client running)
  3.4s  entry=80FDB048 ascb=FDB048 -> DEAD-row2-avail     (killed)
 24.5s  entry=00FF8E68 ascb=FF8E68 -> LIVE                (competitor started -- SAME ASCB)
 60.6s  final -> LIVE
```

So the reuse case this stand produces is a **false LIVE — a resurrection — not row 3**.
The ASID and the ASCB come back from their free chains in lockstep, so the address the
guard compares is the one it recorded, and the guard cannot tell the new occupant from the
old one. That is exactly the case row 3 exists to catch, and exactly the case it fails to
catch. (Consistent with 40-IDENT, here re-measured with a continuous per-second trace
rather than two point samples.)

**This is a finding about the row, not about `ORPHAN`.** Row 3 is untestable live on this
stand either way, so `ORPHAN` buys nothing for it beyond the host-pinned arithmetic — it
rehearses a row that a real client has never been able to reach.

Not established: whether some other MVS configuration breaks the ASID/ASCB lockstep and
makes row 3 reachable. Nothing here says it cannot; only that this stand does not.

### Row 4 — UNKNOWN: unproducible by a real client, all four branches

4a needs a **zero recorded ASCB**. The transport records identity at the claim, from the
FLIH — a real caller always has an ASCB, which is why `ORPHAN` has to **overwrite** what
the claim recorded (`asm/nsfvsvc.asm:604-607`, *"OVERWRITE the identity the claim
recorded"*). 4c and 4d are equally out of reach: a real address space's `ASCBASID` is
never 0 and never exceeds `ASVTMAXU`. 4b is a property of the STC's own ASVT read and not
something a client can arrange at all.

Note the asymmetry with c1's app-registry path, which maps a zero caller ASCB to `NO-ID`
and **never calls the classifier** (`src/nsfreq.c:301-314`, `NSFREQ_APPCL_NONE`) — there, 4a is unreachable by
construction rather than by circumstance.

---

## 2. What is already covered elsewhere (§1.2)

**Logic — all rows, host-pinned, independent of `ORPHAN`.** `test/tstreqx.c:252-295` drives
every row of `nsfreqx_classify` including all four UNKNOWN branches, against a fake ASVT
with a neighbour on both sides; the `asid-1` index is verified to discriminate (the suite
goes red 4-fail with the index deliberately wrong). `slot_action` / `reap_ok` /
`actionable` are swept over all 60 input rows.

**Wiring — this is what `ORPHAN` buys, and it is two rows.** Retiring it loses the live
demonstration that the assembler hands the right values to the classifier and acts on the
verdict, for:

- **row 3** — no replacement possible (unproducible);
- **row 4a** — no replacement possible (unproducible), and it is the only source of a live
  `NSF051W` / `HELD` observation.

**A caution on neighbouring evidence.** M5-2b3's live `NSF050I`/`NSF051W` rows against the
production STC were **`ORPHAN`-driven** and are therefore not independent coverage — they
are part of what is at stake. This round's `NSF050I` is the first that is not.

---

## 3. Retiring the verb vs removing the fields (§1.3)

These are two different changes with very different blast radii. Both costs below; no
recommendation.

### 3a. The verb

Small and self-contained:

- `asm/nsfvsvc.asm` — `FNORPH` EQU (236), the dispatch test + branch (498-499), the
  `ORPHIN` block (592-611), the no-WAIT return test (759-760) and `ORPHRET` (1121).
- `include/nsfvsvc.h` — `NSFV_REQ_ORPHAN` (163) and its doc block.
- `test/mvs/tstdeath.c` — `tstd_orphan()` and scenarios 1–4. **Scenario 5 survives** (a
  real-identity LIVE control).

Consequence to name: **`TSTDEATH` is part of the standing Stage-0 regression figure** (the
444 / 484 PASS quoted every round). Retiring scenarios 1–4 changes that baseline, so the
round protocol has to be restated at the same time or the next round reads as a regression.

### 3b. The fields — this is where the cost is

`pascb` `+1C` and `pasid` `+20` sit **mid-struct**, with **seven fields after them**
(`qstate` `qinfl` `qreap` `rqeimg` `slot` `sexpect` `snew`). Removing them:

- shifts all 7 by 8 bytes;
- changes the value of **7 `NSFV_OFF_ASSERT`s**, deletes 2, and changes
  `NSF_SIZE_ASSERT(NSFV_REQ, 64)` → 56;
- changes the 7 matching asm `REQ*` EQUs (`asm/nsfvsvc.asm:202-209`);
- and **two of the seven shifted fields are used by real, non-probe requests** —
  `rqeimg` (the NSFRQE image, M5-2a) and `slot`, which the claim path writes for **every**
  request (`ST R9,REQSLOT(,R8)`, line 463). `slot` is documented dual-use: *"IN for the
  probe verbs that name a slot … and OUT for every real request"*. The removable set is a
  subset of the probe fields, and it is not contiguous.

**The decisive point, and it is a source claim needing no stand: nothing version-checks the
`NSFV_REQ` layout.** The router validates the request block with the 4-byte eyecatcher only
— `CLC REQEYE(4,R8),=CL4'NSFV'` (line 344) — which is **layout-invariant**. `ANCVERNO`
(line 362) guards the **anchor**, i.e. the router↔STC contract, not the client↔router
contract. And the two parties to `NSFV_REQ` are separately-linked load modules: the client
lives in TESTLIB or an application, the router is `NSFVSVC` `__loadhi`'d from
`NSF.LINKLIB`. They can skew exactly the way CLAUDE.md §5 documents — a mid-chain
`make deploy` failure silently keeps the previous module.

A new client meeting an old router would therefore have `rqeimg` and `slot` read at the old
offsets, silently, with the eyecatcher check passing. That is the same hazard class
`ANCVERNO` was introduced for, in a structure that has no equivalent guard.

**The two costs, stated flatly.** Verb gone / offsets unchanged: no field moves, no assert
changes, no skew hazard, and the two fields remain as dead reserved words. Verb gone /
fields removed: 7 fields move, 8 offset asserts and 7 asm EQUs change, two production
fields (`rqeimg`, `slot`) shift, and the move lands in a structure that has no version
check — so it is a layout change of the kind `ANCVERNO` exists to catch elsewhere, and
would want an equivalent guard introduced in the same change. Which of those is worth
paying is the decision this round does not make.

**The churn is shared with the rest of the probe set.** `SLOT` / `QUERY` / `UNSTAGE` carry
the identical *"SCAFFOLDING, DUE OUT IN M5-2c — a SECURITY item, not hygiene"* header, and
their fields are **interleaved with the production `rqeimg`** (`qstate`+24, `qinfl`+28,
`qreap`+2C, **`rqeimg`+30**, `slot`+34, `sexpect`+38, `snew`+3C). No subset of the probe
fields can be removed as a contiguous block without moving a production field.

---

## 4. The bonus: retiring the verb closes the sharp half of #67

Read from the issue rather than inherited: **#67's stranding mechanism does not depend on a
forged *dead* identity at all**, and that makes the case stronger than the kickoff's summary.
`nsfsx_next_actionable` skips any slot that is not `PENDING`, and the `ACT_DISPATCH` arm
rejects any staged `xfunc` other than `NSFV_REQ_RQE` by setting the slot `HELD`. Nothing
re-examines a `HELD` slot.

`ORPHIN` stages `xfunc = FNECHO` (`asm/nsfvsvc.asm:596-597`), so at the production STC an
`ORPHAN` carrying a perfectly ordinary **LIVE** identity is stranded by the `xfunc`
rejection — no forgery required. (A forged UNKNOWN identity strands it one step earlier, via
`ACT_HOLD`; a forged DEAD one is the only case that cleans itself up, by being reaped.)

What makes `ORPHAN` the sharp case is `ORPHRET` (`asm/nsfvsvc.asm:1121`): it returns
**without parking** and without decrementing `inflight`. `ECHO`/`XFER` also strand a slot,
but the caller parks, so one hostile task costs exactly one slot and hangs itself — 64
parked tasks to exhaust the pool. With `ORPHAN`, **one live, unauthorised task can repeat
the call 64 times**, after which every real client gets `ENOBUFS` and `P NSFS` retains the
anchor and the SVC routine (~137 KB of CSA) until IPL. `QUERY`/`UNSTAGE`/`SLOT` are not
affected — they branch out ahead of the claim and take no slot.

Reachability at NSFS is recorded, not inferred: M5-2b3 saw `NSF050I`/`NSF051W` fire against
the production STC from *"ORPHAN requests reaching NSFS"*. Unlike `XFEROUT`, which M5-2c0
established is dispatchable but never actually executed under NSFS, `ORPHAN` runs there.

So retiring the verb closes the **unparked, repeatable** half of #67 outright, and leaves
the `ECHO`/`XFER` half (one slot per parked task) untouched. That belongs in the argument
for doing it.

**A note that makes §3a's enumeration complete rather than merely unrefuted:** because
`ORPHIN` stages `xfunc = FNECHO`, no C-side code anywhere ever sees `ORPHAN` as a distinct
transform. The verb exists only in the assembler's dispatch and return paths and in
`tstdeath.c`. That is why the §3a list is short, and why it can be claimed to be exhaustive.

## 5. What this round does not establish

- Whether row 3 is reachable on **any** stand — only that it was not, 0 of 9, on this one.
- The reuse window in general: it is governed by address-space **start events**, and this
  round measured one configuration (idle, plus deliberate competitors).
- Anything about the TSO client class, still unmeasured (batch and STC only).
- Whether the `ORPHAN` retirement should happen. That is §4 of the kickoff and Mike's call.

## 6. Housekeeping

Each `HANG`/`PARK` run costs one app slot until reclaimed; every one was reclaimed except
the two **batch** `LEAVE` runs from the `--only TSTAPPD` deploy, which by construction never
will be (initiator identity — the c1 finding, reproduced here as `SLOT 0` and `SLOT 1`
sharing `ASCB=00FD0F18 ASID=0008`). Transport pool left clean: `BUSY=0 INFLIGHT=0`.

`TSTAPPD` had to be redeployed to TESTLIB — a previous round's `--only` had replaced it, so
the first `S TSTAPPDS` drew `IEA703I 806-4 … MODULE ACCESSED TSTAPPD` / `ABEND S806`. That
is the §8.5 shape again: the rig was absent, and absence looks like a result.

`NSF.LINKLIB` was **not** redeployed — no module source changed in this round.

---

## 7. Two deviations from §2, named with their compensating controls

Both are defensible; leaving them implicit is what would cost.

**Offsets were inherited, not re-gated.** §2 says no control-block offset from memory —
`SYS1.AMODGEN`, live, through the DSECT gate. This round did **not** re-run the gate; it
reused `40-ident/arm1.py`'s already-proved set (`CVTASVT`, `ASVTMAXU`, `ASVTENTY`,
`ASVTAVAI`, `ASCBASID`), which 64-3-0 proved with IFOX00 (`IRAOUCB` 17/17, `IHAASCB` 13/13).
Two independent compensating controls were taken instead, and both held:

1. The **CSA size reproduces** — `GDA+8 CSAPQEP → PQE → PQESIZE` reads 2 113 536 B
   (2064 KB), identical to 64-3-0's measurement, on every invocation. A wrong `CVTGDA` or
   a wrong chase would not land on that number.
2. **The raw read agreed with the guard's own verdict, every time.** `rowwatch.py` reads the
   ASVT directly through `/.dm` from outside; the STC reads it through
   `nsfreqx_classify` from inside. They concurred on all 9 kills — `rowwatch` said
   `DEAD-row2-avail` exactly when `NSF057I`/`NSF050I` fired, and said `LIVE` whenever
   `NSF815I` said `LIVE`. Two independent paths to the same field, agreeing.

**Two new files were written.** §2 says build no new induction machinery. Neither of these
is induction: the induction is unchanged — `SYS2.PROCLIB(TSTAPPDS)` and 40-CHK's
cancel-while-parked shape, both pre-existing and both used exactly as documented.
`rowwatch.py` is `40-ident/asvtentry.py` in a loop (**observation**, read-only, `/.dm` only,
no MODIFY and no console command), and `iter.sh` is **orchestration** — it issues the same
`S`/`C` commands a human would. No new way of killing a client was invented.

## 8. Positive evidence for the red lines

Not assertions — `git status` on the round's branch shows exactly one added path:

```
?? docs/measurements/m5-2c2/
```

`asm/nsfvsvc.asm`, `include/nsfvsvc.h`, `src/`, `test/` and `project.toml` are all
untouched, so *"anchor layout unmoved, `ANCVERNO` 3, `NSFRQE` frozen at 64 B, no verb
retired, no field removed, no test scenario removed"* is a property of the diff rather than
a claim about it. Host **3342 PASS / 0 FAIL** before and after — a no-regression check only,
and evidence of nothing else, since nothing outside `docs/` changed.

Stand left clean: `NSF043I SVC 239 RESTORED`, `NSF044I`, `NSF011I`, `IEF404I` — **no
`NSF054W`**, so the drain reached zero and the CSA was freed, no debt and no IPL owed.
**Zero dumps** (`IEA995I` count 0) against 9 deliberate `IEF450I … ABEND S222` cancels,
which is the positive control on that count being real.
