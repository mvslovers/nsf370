# M5-2c2 stage b — retire the `ORPHAN` verb, leave the fields

Implements the decisions locked on the stage-a map (PR #90). Detail in
`docs/measurements/m5-2c2/stage-b.md`.

**Nothing is closed.** Obligation #4 is discharged **in substance for the identity half
only** — the rest of the probe scaffolding is c3, after (e). #67 is **narrowed, not closed**.
The `TSTDEATH` restructuring is **priced and left open**.

Anchor layout unmoved, `ANCVERNO` 3, **`NSFRQE` frozen**, **no field removed**.

---

## The change

`ORPHAN` stored a **request-supplied identity into the slot verbatim** — a forged identity
taken from an unauthorised caller, which is what the guard must never trust for a real
client. It is retired.

**It is rejected, not just deleted, and the placement carries the argument.** The staging
dispatch is a fall-through chain ending in ECHO, so deleting the test alone would have made
`FNORPH` be **serviced as an ordinary ECHO**. It is now rejected **by name, first in the
pre-claim chain**, so a retired verb costs **no slot and no in-flight count — true by
position, not by argument**. That is also what narrows #67.

The rejection writes the rc into the **caller's block** (`BADFUNC`, mirroring `BADANC`)
rather than reusing `BADREQ`, which leaves it in R15 only. `BADREQ` would have left the
client reading its own initialised `-1` — indistinguishable from "the SVC never ran".

`pascb`/`pasid` stay as `rsvd_pascb`/`rsvd_pasid` at **identical offsets**: no layout change,
so the client/router skew stage a identified cannot arise.

---

## Verified host-side (survives without a stand)

- `make test-host` **3342 PASS / 0 FAIL**, unchanged.
- **The classifier's logic is still fully pinned**: `src/nsfreqx.c` and `test/tstreqx.c` are
  untouched by this branch, so all four rows and all four `UNKNOWN` branches keep their host
  coverage. The retirement removes a live *driver*, not host coverage.
- **Layout proven identical to `main`** by comparing every `NSFV_OFF_ASSERT` value and
  `NSF_SIZE_ASSERT(NSFV_REQ, 64)` — no field removed, no offset moved.
- `tools/check-card-columns.sh` OK — after it **caught one of my own cards at 72 bytes**
  mid-edit, which is the documented failure mode found before the toolchain saw it.
- `as370 -a=` listing: `C R3,=A(FNORPH)` → `5930 64F8`, `BE BADFUNC` → `4780 64A4`, **base R6
  not dropped to 0**, target matching `BADFUNC` at `0004A4`.
- **All 1268 source cards present, byte-identical, in source order** (matched as a
  subsequence — macro expansion makes listing statement numbers non-1:1 with source lines).
- **The check discriminates**: a deliberate 89-byte card gave `check-card-columns` FAILED,
  **as370 rc=8**, and the statement check named both the overlong card and the `BE BADFUNC`
  it ate.
- `XFERIN` still ends `B DOPOST`; only comment text sits where `ORPHIN` was — no orphaned
  code, no new fall-through.

## Verified live (MVSCE)

| round | result |
|---|---|
| NSFV — `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTXFW` | **412 PASS / 0 FAIL**, CC 0 batch+TSO |
| NSFS — `TSTRQXC`/`TSTRQXF` | **122 PASS / 0 FAIL**, CC 0 batch+TSO |
| NSFS — `TSTRQXM` | **batch CC 0**, host peer **9353 bytes byte-exact** |

**`TSTDEATH`'s absence is stated, not quiet**: the NSFV figure moves **484 → 412**, exactly
its 72 assertions. `TSTMVCD` stays excluded (#53). `TSTRQXM`'s TSO arm fails **by design**
(one-shot listener consumed by the batch run — verified to be exactly `CONNECT` + its
dependent `CLOSE`).

**The rejection, from an unauthorised client** — `TSTDEATH` is that client, now **FAIL CC 1**
("ran and failed", *not* the CC 20 "did not run" idiom):

```
FAIL: LIVE orphan: SVC accepted and returned without waiting  (got 4, want 0)
FAIL: LIVE client: request SERVICED (state DONE), not reaped  (got 0, want 2)
FAIL: LIVE client: in-flight count NOT given back             (got 0, want 1)
```

`rc = 4` in the caller's block; slot **FREE**; `inflight 0`. No slot, no count — observed.

**Revert test, three states, one assertion moving:**

| state | `TSTDEATH` | rc | slot claimed | forged reaps |
|---|---|---|---|---|
| retired | FAIL CC 1 (52/20) | **4** | no | **0** |
| reverted | ok CC 0 (72/0) | 0 | yes | **6** |
| restored | FAIL CC 1 (52/20) | **4** | no | **0** |

State 2 is rendered **positively** — the forged identities shown *taking*: `ASID=0020` (a
free ASID nothing owns), `ASCB=00FD0F20` (the real ASCB **+8**), `ASCB=00000000` (row 4a).

**Zero dumps**; both STCs start/stop clean; `SVC 239` stolen and restored every cycle; no
`NSF054W`; stand left with nothing running.

---

## Reported, not done

- **`TSTDEATH`: four options priced** against the thing that actually matters — a Stage-0
  probe going red names a *mechanism*, a socket op going red names nothing. Option B turns
  out not to be possible as stated, for this round's own reason (a batch client reads LIVE
  forever). Decision left open.
- **`NSFV_REQ_ORPHAN`**: kept with a retired-marker comment. Cost stated both ways — deleting
  or renaming it breaks `tstdeath.c`'s compile and would **force the `TSTDEATH` decision as a
  side effect**, which this round is told not to pre-empt. The honest cost of keeping it is a
  header that names a verb which no longer works.
- **#67 update drafted, not applied.**
- **The STATS truncation issue drafted, not filed.**
