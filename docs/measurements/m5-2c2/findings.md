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
| 2 DEAD (avail bit) | ASVT entry `AVAIL` | **yes** — measured 9/9 | yes | **yes** — measured this round |
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
covered live by the entire Stage-0 and cross-AS regression set** — TSTSVC, TSTUBUF,
TSTMVCK, TSTXFW, TSTRQXC, TSTRQXF, TSTRQXM — none of which uses `ORPHAN`. Within
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

**The rate, and why a percentage would mislead.** 9 of 9 kills were classified DEAD and
acted on, the ASVT entry flipping to `AVAIL` within ~1 s of the ABEND every time. But the
window is **event-bounded, not time-bounded**: with nothing else starting, ASID 12 stayed
`AVAIL` for the full 100 s of the first watch. It returned to LIVE only when another
address space was deliberately started. So the check races **address-space starts**, not
wall time, and a rate quoted without the start rate attached is not transferable.

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
- changes **7 `NSFV_OFF_ASSERT`s** and `NSF_SIZE_ASSERT(NSFV_REQ, 64)` → 56;
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

**So the two options are not equal in kind.** Leaving the fields reserved — verb gone,
offsets unchanged — retires the forging path without introducing an unguarded layout change.
Removing them is a layout move in a structure with no version check, and would want one
(or a bump of something that covers it) as part of the same change.

**The churn is shared with the rest of the probe set.** `SLOT` / `QUERY` / `UNSTAGE` carry
the identical *"SCAFFOLDING, DUE OUT IN M5-2c — a SECURITY item, not hygiene"* header, and
their fields are **interleaved with the production `rqeimg`** (`qstate`+24, `qinfl`+28,
`qreap`+2C, **`rqeimg`+30**, `slot`+34, `sexpect`+38, `snew`+3C). No subset of the probe
fields can be removed as a contiguous block without moving a production field.

---

## 4. The bonus: retiring the verb closes half of #67

`ORPHAN` claims a slot, publishes it, POSTs the STC and **returns without parking**
(`ORPHRET`, `asm/nsfvsvc.asm:1121`). With a forged zero identity it classifies UNKNOWN →
`ACT_HOLD` → `HELD`, and `nsfreqx_reap_ok` refuses a HELD-with-UNKNOWN outright — by
design, and correctly. So **one live, unauthorised task can strand slots by repetition,
without dying and without being cancelled**; 64 calls exhaust the pool.

That it is reachable at the production STC is not an inference: M5-2b3 records `NSF050I` /
`NSF051W` firing against NSFS from *"ORPHAN requests reaching NSFS"*. Unlike `XFEROUT` —
which M5-2c0 established is dispatchable but never actually executed under NSFS — `ORPHAN`
runs there.

Retiring the verb closes that half of #67 outright, and it belongs in the argument for
doing it.

---

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
