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
