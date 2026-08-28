# ADR-0040 — Phase-2 client-death guard: an ASVT liveness check before the reply POST

**Status:** Proposed (2026-08-21). Settles what happens to the stack's per-request state —
the in-flight count, the request slot and the CSA staging buffer — when the **client
address space dies while a request is in flight**, and what stops the STC from posting a
reply into an address space that is gone or has been recycled. Stage-0a′ proved the
transport (ADR-0038), Stage-0b the `ubuf` move (ADR-0039); **Stage-0c (this ADR) is the
last M5 Stage-0 gate before M5-2.** Still an isolated probe: **no NSFRQE, no socket, no
`owner_ascb` over real sockets.**

**ADR number.** `0037` remains the deliberate, noted gap recorded in ADR-0039; this ADR
takes the next chronological number.

**Relates to:** ADR-0038 (the private-SVC transport and its CSA anchor — the `req_ascb`
this ADR validates, and the **CSA reply ECB** that turns out to carry half the safety
argument), ADR-0039 (the CSA staging buffer this ADR reclaims), ADR-0036 (no cross-memory
services on 3.8j), spec §17.3 (Phase-2 security posture: "socket ownership by ASID"),
spec §19 M5-1 ("client-death cleanup via `owner_ascb`").
**Evidence pins:** `ufsd/docs/recovery.md` §"The Liveness Guard (#53)" and §"Normal
Shutdown"; `ufsd/src/ufsd#rcl.c` (`ufsd_server_state` — the truth table in code, and the
measured SQA-reuse case: UFSD in ASCB `00FD40D0`, killed, restarted into `00FD40D0`
again); `ufsd/src/ufsd#ses.c` (`ufsd_sess_cleanup` — the ASVT walk `CVT@16 → cvtasvt`,
`asvtenty[asid-1]`, high bit = ASID free); `ufsd/src/ufsd#asv.c` + `include/ufsdasv.h`
(the membership scan, and why nothing is read *out of* the ASCB);
`ufsd/src/ufsd#ssi.c` (`ASCBASID` at ASCB offset `X'24'`); `ihaasvt.h` (`ASVTMAXU`,
`ASVTENTY`, `ASVTAVAI` = `X'80'`).

---

## Context

In Phase 2 the client runs in one address space and the stack in the `NSFS` STC in
another. A request crosses the boundary through the CSA anchor: the SVC routine (running
in the *caller's* address space, key 0) marks the request in flight, stages it, POSTs the
STC and WAITs; the STC services it and POSTs the client back; the client copies its result
out, releases the slot and decrements the in-flight count **last**.

Every step of that is written on the assumption that the client comes back. It does not
have to. A `CANCEL`, a `FORCE`, an abend the client's own recovery does not survive, a
memterm for any reason — any of these can end the client address space between the `POST`
and the `WAIT`, or while it is parked in the WAIT. Two things then go wrong, and they are
not equally bad:

1. **State leaks.** The decrement never runs, so `inflight` stands forever; the request
   slot stays busy; the CSA staging buffer stays claimed. At shutdown the drain cannot
   reach zero and the STC retains the CSA (ADR-0038 §5, the 0a′ "retain on timeout" rule).
   Cost: a leaked slot and retained common storage until the next IPL. Bad, recoverable.

2. **The STC posts into a dead address space.** `__xmpost` takes the client's ASCB
   address out of the anchor and branch-POSTs through it. An ASCB block returns to SQA at
   memterm and is handed out again — `ufsd/docs/recovery.md` records this **measured**, not
   theorised: UFSD ran in ASCB `00FD40D0`, was killed, and the restart came up in
   `00FD40D0` again. Posting through a stale ASCB means dereferencing whatever now
   occupies that SQA block. Cost: an `S0C4` or a silent clobber in a **third, innocent**
   address space. Unbounded, unrecoverable, and exactly the failure class M5 exists to
   avoid.

So the question this ADR answers is not "how do we tidy up" but **"how does the STC know,
at the moment it is about to POST, that the client is still there?"** — with the tidy-up
falling out of the same answer.

### Why not a resource manager (RESMGR / an EOM routine)

The textbook answer on a modern system is an end-of-memory resource manager: MVS calls
you when an address space terminates, and you clean up its business. MVS 3.8j has no
dynamic resource-manager interface — no `RESMGR`-style service a program can call at run
time. End-of-memory / end-of-task resource managers are **table-driven and nucleus
resident**: adding one means a nucleus module and a table entry, i.e. a USERMOD and an
IPL. That is out of proportion to what NSF is, and it breaks the install model the whole
project is built on (copy the load modules into a LINKLIB, start the STC — TK4-, TK5 and
MVSCE unmodified). UFSD, facing the same question one layer up, did not build one either;
it polls the ASVT.

### What UFSD proved, and in which direction

Issue #53 in UFSD is the same arithmetic pointed the other way. There, the **cleanup
utility asks whether the server is still alive** before it frees the server's CSA. Here,
the **server asks whether a client is still alive** before it posts into the client. The
mechanism transfers; the truth table does not transfer unexamined (§2 below), and the
*direction of the residual error* changes with it.

---

## Decision

### 1. Client liveness is an ASVT lookup, run immediately before the reply POST

The anchor records the client's identity at SVC entry, where it is authoritative: the SVC
FLIH hands the routine `R7 = A(caller ASCB)`, and the routine stores that (already the
case in 0a′, `ANCRASCB`) **plus the caller's ASID** — `ASCBASID`, the halfword at ASCB
offset `X'24'` (new: `ANCXASID`). A client cannot forge either; both are taken from the
control blocks, not from the request.

Before it does anything with a pending request, the STC classifies that pair:

```
CVT       = *(CVT **)16
ASVT      = cvt->cvtasvt
entry     = asvt->asvtenty[client_asid - 1]      /* ASID n is at index n-1 */

  entry & X'80000000'         -> ASID is AVAILABLE, no address space   -> DEAD
  entry != client_ascb        -> ASID assigned, but to a DIFFERENT ASCB -> DEAD
  entry == client_ascb        -> assigned, and it is our client's ASCB  -> LIVE
```

**LIVE** → service and `__xmpost`, exactly as in 0a′/0b. **DEAD** → **reap**: do *not*
post, release the slot (`req_state` → FREE), decrement `inflight`, clear the staging
buffer and its length/function words, count it (`reaped`), and log it. The reply POST is
gated on this check; there is no other path to it (the drain's "nudge a parked client"
POST is gated on the same classifier).

The check runs **at the moment of use**, not on a timer. A periodic reaper — the shape
`ufsd_sess_cleanup` uses for sessions — always leaves a window: between two sweeps the STC
can still pick up a pending request and post into an address space that died since the
last sweep. Checking immediately before the POST has no such window, and the reclamation
is a by-product of a check that has to happen anyway.

### 2. Three states, not two: `UNKNOWN` is treated as LIVE, never as DEAD

The classifier answers **LIVE / DEAD / UNKNOWN**, and `UNKNOWN` covers every case where
the lookup cannot be completed: no ASCB recorded, no CVT, no ASVT, `asvtmaxu == 0`, or an
ASID outside `1 .. asvtmaxu`. An `UNKNOWN` request is **neither posted into nor reaped** —
it is held, and the state it occupies leaks until the STC stops (which then retains CSA,
per the unchanged 0a′ drain policy).

This is the **safe-side asymmetry**, and it is the rule the whole design is bent around:

| Error | Consequence |
|---|---|
| A **dead** client is classified LIVE or UNKNOWN | The request is not reclaimed. A slot and a staging buffer leak; the STC retains CSA at stop. **Nothing is corrupted.** |
| A **live** client is classified DEAD | The STC frees a request slot and staging buffer that the client is still executing against, and the client's own decrement then runs against a slot that has been handed to someone else. **This is the catastrophe.** |

The two are not comparable, so the classifier is deliberately reluctant: it reports DEAD
only from a *positive* reading of the ASVT (the entry says available, or the entry names a
different ASCB). "I could not check" is never answered with "go ahead and free it" — the
rule `ufsd#rcl.c` states in the same words for its own direction.

An out-of-range ASID deserves a note, because the obvious reading is the wrong one. It
*looks* impossible-therefore-dead — no such address space can exist. But impossible means
the recorded identity is not trustworthy, and an untrustworthy identity is exactly when a
free-the-storage decision must not be taken. It is `UNKNOWN`. (`ufsd_sess_cleanup` treats
an out-of-range ASID as stale and reaps the session; it can afford to, because what it
frees is that session's own file handles, not storage another address space is executing
inside.)

### 3. The comparison is on the ASCB **address** only

Nothing is read out of the ASCB — not the jobname, not a flag, nothing. The address in the
anchor may be stale, and the SQA block it points at may now be some other address space's
ASCB or not an ASCB at all; a field read out of it answers with a stranger's data, or
faults. Identity is established by comparing the ASVT entry to the recorded address, which
cannot fault and needs no field offsets. This is `ufsd#asv.c`'s rule adopted verbatim, and
it is the reason the classifier is arithmetic over two words rather than a control-block
walk.

### 4. Both halves of the identity are needed: ASID indexes, ASCB address decides

The ASID alone is not enough. An ASID is reassigned when an address space ends: a lookup
that only asks "is ASID 42 assigned?" reads *assigned* and answers LIVE, when what is
assigned is a completely different job that inherited the number. That is a post into a
stranger.

The ASCB address alone is not enough either — with no ASID you must scan the whole ASVT to
see whether the address appears anywhere (what `ufsd_ascb_in_asvt` does, because the
reclaim path has only the address).

Recording both turns the scan into an **indexed lookup with an identity check**: the ASID
selects the entry, the address decides whether that entry is *our* client. `asvtenty[asid
- 1]` is the one line where an off-by-one is silent and fatal in the LIVE direction (it
would compare against a neighbour's ASCB, classify a live client DEAD, and reap under it);
ADR-0038's live-gate discipline applies — the live gate's LIVE control is what proves the
index, since an off-by-one makes it fail.

### 5. UFSD's row 4 (own ASCB → DEAD) does **not** transfer

`ufsd_server_state` excludes the caller's own ASCB first and calls that case DEAD —
"inherited, not inhabited". It has to: the reclaimer is usually the *restart* of the very
server it is checking, and it can be handed its dead predecessor's ASCB block, so without
the exclusion a FORCEd predecessor finds its own recycled ASCB in the ASVT, calls itself
live, and refuses both the start and the cleanup with no way out but an IPL.

Here the checker is the **server** and the subject is a **client**, and the same row would
be a bug. `client_ascb == our own ASCB` has a perfectly ordinary reading in this
direction: a client task running **inside the STC's own address space**. Calling that DEAD
would reap a live client's request — the catastrophe direction, produced by a single
compare copied out of context. It is therefore **not implemented**, and rows 4/5 of §1
cover what it was there for anyway: if the client is genuinely dead, its ASID is either
available (DEAD) or holds a different ASCB (DEAD), and the "different ASCB" case includes
"the ASCB is now ours".

This is recorded at length because the #53 truth table is the natural thing for the next
reader to copy wholesale, and one of its five rows inverts when the direction inverts.

### 6. A held request needs its own state (`NSFV_REQ_HELD`)

`UNKNOWN` means the STC declines to service a request without releasing it. The slot
therefore cannot stay `PENDING`: the STC's service loop drains *while* the state is
`PENDING` (ADR-0022 reset-before-WAIT + double-check-drain), so a request it refuses to
touch would spin the executive at 100 % CPU. The classifier's refusal moves the slot to a
fourth state, `HELD` — still busy to the SVC routine's slot-take (which rejects any
non-FREE state), no longer a work item to the STC. Written by the server side only, like
`PENDING → DONE`; the one-writer-per-transition rule of ADR-0038 is unchanged.

### 7. The residual error, and why it is survivable in this direction

One error survives the guard, the same one #53 documents: if the dead client's ASID has
been reassigned to a new address space **that was also handed the same ASCB block**, the
lookup finds `entry == client_ascb`, and the classifier says LIVE.

In UFSD's direction that residual costs convenience (a dead server looks alive, cleanup is
refused). In this direction it means the STC services the request and posts — into a
**valid, live** address space that is not the one that asked. Two properties of ADR-0038's
design contain the damage:

- **The reply ECB is in CSA**, not in the client's private storage (ADR-0038 §4 — forced
  by the SVC routine running key 0 throughout). The POST therefore writes a word of common
  storage the stack owns, never an address in a private address space that has been
  reused. This is the difference between a wrong wake-up and a cross-address-space clobber,
  and it was decided for an unrelated reason two stages ago.
- **The ASCB is a currently valid ASCB.** That is precisely what the guard establishes, so
  the branch POST dereferences a live control block, not freed SQA.

The observable cost is that the request is serviced, posted, and never completed by anyone
— so the slot stays busy and `inflight` stays up: a **leak**, the acceptable side of §2's
asymmetry. Recorded, accepted for the probe, and inherited by M5-2 (where per-client slots
bound it to one request rather than the single shared slot).

### 8. What the probe adds to reach the gate deterministically

The gate must be a batch job with a deterministic return code — no operator timing (Mike's
call: simulation (b)). A real mid-WAIT kill is not that, so the probe models the **effect**
through three **probe-only** SVC functions, all of them documented as such and none of them
part of the M5-2 transport:

- **`NSFV_REQ_ORPHAN`** — stage a request exactly as `ECHO` does, but with the identity
  **the client supplies** (`pascb`/`pasid`) instead of the one the FLIH provides, POST the
  STC, and **return without WAITing**. The in-flight decrement is skipped *by
  construction*, which is what a dead client produces, and the caller survives to check the
  outcome.
- **`NSFV_REQ_QUERY`** — read `req_state`, `inflight`, `reaped` and `served` back into the
  request block. Touches no state, works while the slot is busy: the gate's only way to
  observe the STC's decision without reading CSA from an unauthorized program.
- **`NSFV_REQ_UNSTAGE`** — probe cleanup: release a slot the STC deliberately did not
  release (the `HELD` case, and the `LIVE`-orphan case), so the probe leaves no in-flight
  count behind and the STC still stops clean.

**This is synthetic, and the PR says so plainly.** The client is alive and hands over a
dead *identity*; what is genuine is (i) the in-flight increment and the staging, (ii) the
skipped decrement, and (iii) the identity naming an address space that really is not there
— an ASVT entry that is really available, or an ASID that really is assigned to a different
ASCB. What is *not* modelled is the race between a kill and the STC's POST. That race is
covered by the manual, operator-timed check documented in the PR: hold a reply, `CANCEL`
the client, watch the STC classify and reap.

One detail of the gate is load-bearing and easy to "clean up" into a bug: the free-ASID
scenario stages the client's **own real ASCB** with a *free* ASID. If the ASID is assigned
between the client's pick and the STC's check (a real possibility on a busy system), the
entry then holds the new occupant's ASCB, which cannot be the still-living client's — so
the scenario still reads DEAD, through §1's mismatch row instead of its available-bit row.
Replace that ASCB with a dummy value and the race turns into a false LIVE.

---

## Consequences

- **The reply POST has exactly one gate, and it is not optional.** Both POST sites in the
  STC (service and the drain's nudge) go through the classifier. A future POST site that
  forgets it re-opens the whole failure class — the same shape as the project's
  one-destroy-function rule.
- **Reclamation is bounded to what the guard can prove.** A client that dies with **no
  request in flight** is invisible to this mechanism: there is nothing pending to
  classify. In the probe that is a non-issue (no state exists outside a request), but it is
  the direct reason M5-2 still needs the `owner_ascb` sweep for sockets — a socket outlives
  the request that created it. That sweep should reuse **this** classifier rather than
  `ufsd_sess_cleanup`'s ASID-only test, which cannot see ASID reuse.
- **The drain policy is unchanged.** Stage-0c keeps 0a′'s "retain CSA on timeout"; folding
  the guard into the drain (so a shutdown could reclaim what it can prove dead) is M5-2.
- **The anchor and request block grow**, and their assembler mirror grows with them:
  `NSFV_ANCHOR` 2176 → 2184 (`client_asid`, `reaped`; `stage` stays the last field so
  ADR-0039's overrun argument still holds), `NSFV_REQ` 28 → 48 (the probe verbs' in/out
  words). Size asserts are updated, and per-field offset asserts are added — a total-size
  assert cannot catch a field that moved, and a wrong `ANCSTAGE` is an IPL-class CSA
  overrun by ADR-0039's own reasoning.
- **No host coverage for the arithmetic.** The classifier reads real control blocks, so it
  is MVS-only, and the host suite is unchanged. UFSD split the scan out into portable C
  (`ufsd#asv.c`) precisely so the off-by-one class is host-testable; NSF does not, and the
  live gate's LIVE control is what stands in for it. Whether M5-2 should extract it is
  Mike's call.
- **`inflight` becomes a counter two address spaces decrement** — the client's SVC routine
  on the normal path, the STC on a reap. Both do it with a compare-and-swap loop, and the
  STC's never goes below zero.

## Status / history

- **2026-08-21 — Stage-0c implemented and VALIDATED LIVE on MVSCE** (probe STC `NSFV`
  `STC01187`, anchor `00A86288`, ASCB `00FD4F18`, `SVC 239` stolen from EP `0000CCC8`).
  `TSTDEATH` **CC 0 batch + TSO, 72 PASS / 0 FAIL** (36 assertions each; the TSO re-run
  passes too — unlike the CTCI probes, nothing here is a single-shot physical resource).
  The client ran with `__isauth() == 0` throughout (the ADR-0038 red line), located its own
  identity (`ASCB 00FE7380`, `ASID 0008`) and found `ASID 0020` reported AVAILABLE by the
  ASVT — the control-block reads an unauthorized problem-state program needs for the probe
  worked without a fault. All three classifications came out right, and the STC logged each
  one:
  - `NSFV050I CLIENT DEAD (ASCB=00FE7380 ASID=0020) -- REQUEST REAPED, INFLIGHT=0
    REAPED=1` — the **available-ASID row**: the client's own real ASCB carried on a free
    ASID, reaped, in-flight count back to zero, **no POST issued**.
  - `NSFV050I CLIENT DEAD (ASCB=00FE7388 ASID=0008) -- REQUEST REAPED, INFLIGHT=0
    REAPED=2` — the **ASID-reuse row**, forced deterministically: `ASID 0008` *is* assigned
    (to the client itself), but to ASCB `00FE7380`, not the recorded `00FE7388`. This is
    the row an ASID-only check cannot see.
  - `NSFV051W CLIENT LIVENESS UNKNOWN (ASCB=00000000 ASID=0008) -- REQUEST HELD, NOT
    REAPED` — the **safe side**: no post, no reap, `inflight` deliberately left standing,
    released only by the client's own `UNSTAGE`.
  - **No `NSFV050I` for either LIVE scenario** — the orphan carrying the live identity was
    serviced (`DONE`) and the blocking `ECHO` round trip returned `token+1`. The guard did
    not false-positive a living client, which is also what proves the `asvtenty[asid-1]`
    index: an off-by-one would have compared against a neighbour's ASCB and reaped here.
  Shutdown clean on the first `P NSFV`: `NSFV002I NSFV SERVED=4 INFLIGHT=0 REAPED=4
  STATE=0`, then `NSFV095I SVC 239 RESTORED` → `NSFV036I SVC ROUTINE UNLOADED` →
  `NSFV011I NSFV SHUTDOWN COMPLETE` → `IEF404I`, **`COND CODE 0000`, no `NSFV098W`** (the
  drain reached zero, no CSA retained) and **no dump**.
- **2026-08-21 — What is proven, and what is not.** Proven live: the classifier's three
  answers on real control blocks, the reap (in-flight count, staging, slot), the refusal to
  post into anything not confirmed LIVE, and that the guard costs the LIVE path nothing.
  **Not proven live, by construction:** the race between a kill and the STC's POST — the
  gate stages a dead *identity* from a living client (ADR-0040 8). The operator-timed check
  that closes that gap is a manual step: hold a reply, `CANCEL` the client while it is
  parked, watch the STC classify and reap. **Deferred to M5-2:** the `owner_ascb` sweep for
  sockets that outlive their request (this classifier, not `ufsd_sess_cleanup`'s ASID-only
  test), folding the guard into the drain's timeout policy, and per-client staging/slots.
- **2026-08-21 — Offline gates.** Host suite **2788/0 unchanged** (the guard is MVS-only);
  full cc370/as370/ld370 cross-build (5 modules + 45 test modules); **no new external
  symbols** (every new C helper is `static`, the assembler added no CSECT/ENTRY). The
  layout change was audited two ways: the new `NSFV_OFF_ASSERT` set pins every C field
  offset at cross-compile (verified to have teeth — moving `stage` from 136 to 128 in the
  assert fails the build), and the `as370 -a=` listing was read against it (`ANCSTAGE` now
  `X'088'` in both `MVCK` setups, the ASID capture assembles as `4830 7024` = `LH R3,
  X'024'(0,R7)`, every `CS` still RS-format `D(B)`, nothing dropped at column 72).
- **2026-08-21 — A regression the first live run could not see, found by re-running 0b.**
  The gate above (`STC01187`) exercised `ECHO` and the probe verbs; it never issued an
  `XFER`. Re-running Stage-0b's own deliverables at the new layout — which the moved
  `stage[]` (+128 → +136) demanded — **hung `TSTUBUF` on its first call**, and the console
  said why: `NSFV051W CLIENT LIVENESS UNKNOWN (ASCB=00000000 ASID=0000)`.
  **Cause: a fall-through the insertion broke.** The `XFER` staging block used to end by
  *falling through* into the shared POST block. Stage-0c inserted the `ORPHAN` staging
  block between the two, so every `XFER` fell into `ORPHAN` instead and re-staged the
  identity from the request's probe words — zeros for an `XFER` caller. The guard then
  correctly answered UNKNOWN, correctly refused to post, and the client waited forever for
  a reply that must not be sent. **The guard behaved exactly as designed; the routine lied
  to it.**
  **Fix:** every staging block now ends with an **explicit `B DOPOST`**, including the one
  physically ahead of it, with a comment block saying why they must not be "cleaned up".
  **The lesson is CLAUDE.md §3's, in a new shape.** Not a column-72 merge this time, but the
  same evidence rule: the assembler is happy, the link is clean, the offset asserts pass,
  the `as370` listing looks right instruction by instruction — and none of them can see
  that control now reaches the wrong block. Only running the *other* stage's live gate
  found it. **An asm change under a moved layout is not validated by re-running the new
  test; it is validated by re-running the tests of every stage that layout carries.**
- **2026-08-21 — Final validated state** (probe STC `NSFV` `STC01189`, anchor `00A736C8`).
  Full Stage-0 regression at the Stage-0c layout, **all CC 0 batch + TSO, 444 PASS / 0
  FAIL**: `TSTSVC` (0a′ round trip, 50 ECHOs per run), `TSTMVCK` (0b step 1), **`TSTUBUF`
  (0b step 2 — sizes 0/1/100/2048/5000/10 byte-exact with the guard byte after `ulen`
  untouched, the discriminating test for the moved staging)** and `TSTDEATH` (0c). The
  three guard verdicts logged again unchanged, no `NSFV050I` on either LIVE path. Final
  `NSFV002I NSFV SERVED=126 INFLIGHT=0 REAPED=4 STATE=0`; `P NSFV` → `NSFV095I SVC 239
  RESTORED` → routine unloaded → `NSFV011I`, **no `NSFV098W`** (drain reached zero, no CSA
  retained), no dump.
- **2026-08-21 — One consequence of gating the drain's nudge, recorded honestly.** Because
  `nsfv_wake_parked` posts only a LIVE client, a client parked on a request the guard
  cannot classify (UNKNOWN) is **never nudged at quiesce** — it waits until it is cancelled,
  and the STC retains CSA. That is the safe-side trade in its least pleasant form, and the
  aborted run above demonstrated it (`NSFV098W 1 CLIENT(S) STILL IN FLIGHT -- CSA
  RETAINED`, the anchor left allocated). For a real request it cannot arise — the routine
  always records `R7` — but M5-2 should decide whether an UNKNOWN request at *shutdown*
  deserves a different answer from an UNKNOWN request at *service* time.
- **2026-08-22 — Stage-0c COUNTERSIGNED by the maintainer** (review on PR #50; verdict:
  countersign / merge). **Stage-0c is proven, and it was the last M5 Stage-0 gate — M5-2 is
  unblocked.** The review states its own proof division, which is worth keeping because it
  is what a countersign means here: the maintainer independently verified the **control-flow
  fix** (traced `XFERIN → WRINEND → B DOPOST` jumping over the physically-following
  `ORPHIN`, and that no staging block falls through), the **classifier and the row-4
  adaptation**, the truth table, the safe-side and address-only rules, ADR-0040 itself, and
  the static gates; the **live evidence** (the four runs, the counters, the console
  verdicts, the cross-build, the 0b regression) is taken from this session's runs, because
  cross-address-space behaviour is not host-reproducible. Two findings were confirmed as
  the review's own: that the fall-through was caught by the **right** cross-check
  ("does the layout shift touch older deliverables?"), and that **§5's refusal to carry
  ufsd's row 4 corrected the kickoff**, which had carried it over unexamined.
- **2026-08-22 — Two forward notes from the countersign review, both for M5-2, neither
  blocking Stage-0c.**
  1. **The probe scaffolding must NOT survive into M5-2.** `NSFV_REQ_ORPHAN` and the
     `pascb`/`pasid` request words exist so a batch job can stage a dead identity without
     killing an address space — necessary for the deterministic gate, and **exactly what
     the guard must never trust for a real client**. The unforgeable path is the one the
     ECHO/XFER staging uses: `ASCBASID` off `R7`, from the control block. When M5-2 wires
     this guard to real sockets, `ORPHAN` and those two fields **come out**; they are
     scaffolding, not a transport path, and must not be mistaken for one.
  2. **Extract the classifier's arithmetic and host-test it (M5-2).** The open question
     this ADR left to the maintainer — whether to split the ASVT arithmetic out the way
     UFSD did into `ufsd#asv.c` — is answered **yes, in M5-2**: the ASID range check, the
     AVAIL bit, the address compare and all four verdicts (including the subtle
     own-ASCB → **LIVE** row and the UNKNOWN cases) are pure deterministic arithmetic, and
     a host test pins the whole truth table instead of leaning on a live run to exercise
     every row. **Not** retrofitted into Stage-0c: right-sized for M5-2, which owns this
     guard on `owner_ascb`.

- **2026-08-28 — THE GUARD IS INERT FOR A CLIENT THAT HAS NO ADDRESS SPACE OF ITS OWN.**
  Measured live (40-CHK, `docs/measurements/40-chk/findings.md`; M5-2c1 stage a,
  `docs/measurements/m5-2c1/stage-a.md`). **No change to the guard, the classifier or the
  POST path** — this entry records what the check does and does not cover, so that what to
  do about it is decided on a reading rather than on an expectation.

  1. **The class: clients that do not own their address space.** A batch job runs in an
     **initiator**, and an initiator does not terminate when a job ends — it waits for the
     next one. So the ASCB recorded at the claim is the *initiator's*, its ASVT entry never
     goes AVAILABLE, and `nsfreqx_classify` answers **LIVE** at exactly the moment this
     guard exists to answer DEAD. **The classifier is not wrong**: it answers "did that
     address space end?", and for a batch client the honest answer is no, even though the
     application is gone. Measured twice, on two different initiators: stage a saw **three
     jobs** report `ASCB=00FE7B58 ASID=0006` (one submitted after the first had reached
     OUTPUT), and 40-CHK saw **two jobs** report `ASCB=00FE7330 ASID=0008`. **Every client
     in this tree today is a batch job**, so today the check runs, costs a lookup per
     reply, and cannot fire.

  2. **NO CORRUPTION, and this is milder than the concern that motivated the round.** The
     reply ECB is the word at **slot+8 inside the CSA anchor** — `asm/nsfvsvc.asm:584-585`,
     `LA R3,SLRECB(,R7)` / `WAIT 1,ECB=(R3)`, issued in supervisor state key 0, which is
     what makes a key-0 ECB legal there. Common storage; **job termination does not free
     it.** This ADR's own header already said the CSA reply ECB "turns out to carry half
     the safety argument" — 40-CHK is where that half was collected. The private region
     genuinely *is* handed on: two different jobs reported byte-identical
     `STACK=000D1348 HEAP=00094C68`. **But the ECB is not in it**, so a POST into storage a
     later job has been given cannot arise on this path. The fear was reasonable and it is
     refuted.

  3. **What it costs instead — the expensive consequence first.** `inflight` is **never
     given back**, because the party that returns it is the client and the client is dead.
     `nsfsx_stop` then cannot drain: its nudge POSTs parked clients, and there is nobody to
     wake. So `P NSFS` runs the full drain ceiling and takes the **retain** branch, keeping
     the anchor *and* the SVC routine — and the storage is unreclaimable **until an IPL**.
     Measured, not reasoned: `NSF054W 1 CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE
     RETAINED (EXHAUSTED=0)` ten seconds after `NSF830I`, then the next start reporting
     `NSF055I ... LARGEST FREE BLOCK NOW 933888` against `1073152` before it — **139 264
     bytes**, the pool plus the router, **and again on every recycle**. So **one parked
     batch client that dies costs an IPL, without NSFS ever crashing.** (Distinct from
     issue #79, where an *abend* leaves SVC 239 stolen and NSFS cannot restart at all. Here
     the SVC **is** restored — `NSF043I` precedes the drain — so the STC comes back; it
     simply cannot get its CSA back.) *Then*, and only then, the smaller costs: the request
     slot is stuck at `DONE` for ever, and every later claim scan walks over it and pays —
     `collisions` **0 → 4** across the remainder of the round, a permanent tax for the life
     of the STC. Sixty-four such deaths exhaust the pool.

     The slot is unrecoverable **by design, given the verdict**: `nsfreqx_slot_action`
     returns `ACT_NONE` for anything that is not `PENDING`, and
     `nsfreqx_reap_ok(DONE, LIVE, storage_ok=1)` returns 0 — not DEAD, storage trusted, so
     there is nothing to reclaim on. Both rows are right. **The verdict is the defect.**

  4. **The two ECB states, because they carry the refutation.** The guard answered LIVE and
     the LIVE branch ran to completion — `served` 9 → 10, `req_state` 1 → 2 (`DONE`),
     `reaped` 0, no `NSF050I`, no `NSF051W`, no abend — and `served++` → `DONE` →
     `__xmpost` is one straight-line sequence, so **the POST was attempted into an address
     space whose client task was dead.**

     - dead client, slot 0: `reply_ecb` at `00A8B808` still **`809DE5F0`** minutes later —
       the dead task's WAIT bit plus its RB address, **unchanged**; slot stuck at `DONE`.
     - live client, slot 1, same anchor and same code path minutes later:
       `reply_ecb` **`40000000`** — properly posted — and `req_state` back to `FREE`.

     The contrast is what makes the first reading mean something: it is not that the POST
     was skipped, it is that it left no mark. A control obtained for free, because the next
     client had to skip the leaked slot.

  5. **An OPEN DIRECTION, offered as a question and explicitly not as a mechanism.** The
     guard asks *"is the client alive?"*. The reading suggests asking *"did the POST
     arrive?"* instead: POST, read the ECB back, and a missing POSTED bit means the waiter
     is gone, so the slot and the in-flight count can be returned. It is attractive because
     it addresses **exactly the class the ASCB check fails on**, and because it invents no
     per-task identity — for which this ecosystem has no pattern: `ufsd#asv.c` is
     address-space-wide and expressly designed that way, which is why §5 of this ADR
     already declined to carry one of its rows.

     **It is n=1 against n=1**, and it rests on undocumented POST behaviour when the
     recorded RB has expired. This round observed the ECB unchanged in one case and posted
     in one other; that is an observation, not a contract, and nothing here establishes
     what POST does in general with a stale RB. **It becomes a mechanism only after a
     measurement with several cases**, and until then it is a direction, not a design.

  6. **The gap that made all of this possible, named plainly: no test in this tree has ever
     watched a real address space die.** `TSTDEATH`'s DEAD rows stage a **synthetic**
     identity — `tstd_free_asid` scans the ASVT for an ASID that is *already* AVAILABLE —
     so the guard's DEAD path has always been exercised against a manufactured pair, never
     against a client whose address space actually ended. An **STC** or TSO client *is* its
     own address space and does terminate, which is the case this guard was built for and
     the one M6 needs (HTTPD and mvsMF are both STCs). A dying STC client is the test that
     never existed. It is independently fixable and **not part of this annotation**.

  **What happens to the batch class is the maintainer's decision, on this record.** The
  failure direction remains the safe one throughout: a false LIVE leaks, and never tears
  down a healthy client's sockets — the asymmetry §5 is built on, holding under a condition
  nobody had anticipated.

