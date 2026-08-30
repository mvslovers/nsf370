# M5-2c2 stage a — map what retiring `ORPHAN` costs

Mapping round for the first half of M5-2c. **Nothing is deleted and nothing is
recommended**: no verb retired, no field removed, no test scenario removed. `asm/`, `src/`,
`test/`, `include/` and `project.toml` are untouched — `git status` shows exactly one added
path, `docs/measurements/m5-2c2/`. Anchor layout unmoved, `ANCVERNO` 3, `NSFRQE` frozen at
64 B. Host **3342 PASS / 0 FAIL** before and after (a no-regression check only — nothing
outside `docs/` changed).

Full detail in `docs/measurements/m5-2c2/findings.md`.

## The answer in one line

Retiring `ORPHAN` does **not** cost an induction technique — a real dying STC client reaches
the guard on both paths, shown end to end here for the first time. It costs **live wiring
coverage of the two rows a real client cannot reach**, and one of those rows has never been
reachable with or without `ORPHAN`.

---

## Read from source — survives without a stand

- **The truth table.** `nsfreqx_classify` (`src/nsfreqx.c:128-165`) reaches `UNKNOWN` from
  **four** distinct conditions, not one. `tstd_orphan((void *)0, own_asid)`
  (`test/mvs/tstdeath.c:290`) drives only `req_ascb == 0`. Row 4's live coverage is
  therefore already 1-in-4.
- **Row 4 is unproducible by a real client, all four branches.** Identity is captured at
  the claim from the FLIH; a real caller always has an ASCB, its `ASCBASID` is never 0 and
  never exceeds `ASVTMAXU`, and the "no ASVT" branch is an STC-side property no client can
  arrange. This is why `ORPHIN` must **overwrite** what the claim recorded
  (`asm/nsfvsvc.asm:604-607`).
- **Row 1 is gated on every reply POST.** `src/nsfv.c:427` — *"Nothing else in the STC may
  POST a client without passing here."* A false DEAD hangs the client, so every
  boundary-crossing test is a row-1 witness.
- **The layout has no version check.** The router validates the request block with a
  **layout-invariant** eyecatcher only (`CLC REQEYE(4,R8),=CL4'NSFV'`,
  `asm/nsfvsvc.asm:344`). `ANCVERNO` (line 362) guards the **anchor** — the router↔STC
  contract — not the client↔router one, and those two are separately-linked load modules
  that can skew the way CLAUDE.md §5 documents.
- **The fields are mid-struct and interleaved with production fields.** `pascb`/`pasid` at
  `+1C`/`+20` have **seven** fields after them; two of the seven carry real requests —
  `rqeimg`, and `slot`, which the claim path writes for *every* request
  (`ST R9,REQSLOT(,R8)`, line 463). The probe fields are not contiguous: `rqeimg` sits
  between them.
- **`ORPHAN` needs no forged dead identity to strand a slot (#67).** `ORPHIN` stages
  `xfunc = FNECHO` (`asm/nsfvsvc.asm:596-597`), and the `ACT_DISPATCH` arm strands anything
  that is not `NSFV_REQ_RQE` as `HELD`. `ORPHRET` returns **without parking**, so one live
  unauthorised task can repeat it 64 times — where `ECHO`/`XFER` cost one slot per parked
  task. The same fact makes the §3a change-list exhaustive: no C-side code ever sees
  `ORPHAN` as a distinct transform.

## Measured live — MVSCE, freshly IPLed, NSFS STC 1653

Deployed build confirmed as c1's by the positive check c1 established (`NSF817I APPSWEEP`
present — impossible on any earlier build). CTCI 0500/0501 up, ping 3/3 0 % loss.

- **Row 2 is producible by a real client and the guard acts on it.** 14 real STC clients
  killed across two NSFS instances; the ASVT entry flipped to `AVAIL` within ~1 s of the
  ABEND every time. All 14 reclaimed via the **app sweep** (`NSF057I`).
- **The transport path — the one `ORPHAN` rehearses — 6 of 6, and it is new.** Each `PARK`
  run proved the request genuinely outstanding first (`BUSY=1 BUSYSLOT=0 INFLIGHT=1`, the
  conjunction 40-CHK established), was cancelled, then completed over the wire:
  `NSF050I CLIENT DEAD (ASCB=00FF8E68 ASID=000C) -- REQUEST REAPED` every time. **First time
  in this tree the transport guard's DEAD path has been driven by a real address-space
  death**, with no forged identity anywhere.
- **The death→verdict interval decomposes, and only half of it is a system property.** The
  first session's ~16 s was mostly the arm's own choice of when to send the completing
  datagram, so the addendum *varied* that gap — 12 / 28 / 52 / 97 / **158** s — and reports
  the two parts separately. **(b) datagram → `NSF050I` is ≤ 1 s in all five**: once a
  completing event exists the STC classifies and reaps immediately. **(a) ABEND → datagram
  is not a system property at all** — it is when the peer happens to send, and the guard did
  not look until then, at every gap, **up to 158 s after the client had died**.
- **Row 3 was not produced once, in 9 reuses.** Every reuse restored the **identical**
  `(ASCB=00FF8E68, ASID=000C)` pair. The continuous trace is
  `LIVE → DEAD-row2-avail → LIVE` with the same ASCB — a **false LIVE**, which is exactly
  the case row 3 exists to catch and exactly the case it fails to catch. That is a finding
  about the row, not about `ORPHAN`: it is untestable live either way.
- **Every identity was cross-checked** against the client's own WTO before use; no reading
  is about some other address space.
- **The transport guard has no period.** The exposure is therefore an **unbounded interval
  (a)** in which the identity is stale and unexamined, followed by a **bounded (≤1 s)
  verdict (b)**. All the risk lives in (a), and what it races is address-space *starts*, not
  wall time — the verdict held `DEAD` through all 158 s here only because the stand was
  idle. Had an address space started in any of those windows the identity would have
  resurrected to LIVE and the STC would have POSTed into a dead one (40-CHK's leaked slot).

## What removal costs, per row

| row | producible by a real client | logic host-side | wiring live after removal |
|---|---|---|---|
| 1 LIVE | yes, constantly | yes | **yes** — every crossing test |
| 2 DEAD (avail) | yes — 14 of 14 | yes | **yes** — app sweep n=13, transport **n=6** |
| 3 DEAD (mismatch) | **no** — 0 of 9 reuses | yes | no, and not coverable |
| 4a/b/c/d UNKNOWN | **no** (4a only via `ORPHAN`) | yes | no |

Lost: live wiring for **row 3** and **row 4a** — and the only live source of an
`NSF051W` / `HELD` observation. M5-2b3's `NSF050I`/`NSF051W` against NSFS were themselves
`ORPHAN`-driven and are **not** independent coverage.

**Two framings that make the decision small.** **Row 3 is not a cost of removal at all**:
0 of 9 reuses produced it, every reuse restored the identical `(00FF8E68, 000C)` pair, and
what this stand produces is a **false LIVE** — exactly the case row 3 exists to catch.
Untestable live with or without `ORPHAN`. **And row 4 was never one row**: four branches,
`ORPHAN` drives one, so its live coverage was 1 in 4 rather than 1 in 1.

So what retiring `ORPHAN` actually costs is **row 4a's live wiring proof, plus row 2 moving
from a deterministic forge to a racy real induction** — against a classifier whose logic is
host-pinned throughout.

## Verb vs fields (§1.3), both costs, no recommendation

- **Verb only:** `FNORPH` EQU, the dispatch test and branch, the `ORPHIN` block, the
  `ORPHRET` test and label; `NSFV_REQ_ORPHAN`; `tstd_orphan()` and `TSTDEATH` scenarios
  1–4 (scenario 5, a real-identity LIVE control, survives). Consequence to name: **`TSTDEATH`
  is part of the standing 444/484 Stage-0 figure**, so that baseline changes and the round
  protocol must be restated with it.
- **Fields as well:** 7 fields shift by 8 bytes; 7 offset asserts change value, 2 are
  deleted and the size assert goes 64 → 56; 7 asm `REQ*` EQUs change; two production fields move, and the move lands in a structure with **no version
  check** — so it would want an equivalent guard in the same change. The churn is shared
  with `SLOT`/`QUERY`/`UNSTAGE`, whose fields are interleaved with `rqeimg`.

## Not established

Whether row 3 is reachable on any *other* stand; the reuse window in general (it is governed
by address-space start events, and one configuration was measured); the TSO client class;
and whether the retirement should happen — **that is Mike's call**.
