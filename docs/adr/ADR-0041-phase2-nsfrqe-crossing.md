# ADR-0041 — Phase-2 NSFRQE crossing: a copied CSA request slot and an STC-private dispatch copy

**Status:** Proposed (2026-08-22). Settles **how the frozen 64-byte `NSFRQE` crosses the
address-space boundary** between an application and the `NSFS` STC, how the result comes
back, and which fields the STC may write. Stage-0 proved the three mechanisms this ADR
composes — the private-SVC transport (ADR-0038), the keyed `ubuf` CSA bounce (ADR-0039)
and the client-death guard (ADR-0040) — each in isolation, with no `NSFRQE` and no socket.
**M5-2a is the first M5 step that touches production code:** a real request crosses and a
real socket op round-trips. It is sub-step **(a) of five**; **single client, single slot,
nothing concurrent, nothing dies.**

**ADR number.** `0037` remains the deliberate, noted gap recorded in ADR-0039; this ADR
takes the next chronological number.

**Relates to:** ADR-0038 (the SVC transport, the CSA anchor and the reply path this ADR
carries the `NSFRQE` over), ADR-0039 (the `MVCK` staging buffer and the `min(ulen, 2048)`
clamp whose silent-truncation obligation lands here), ADR-0040 (the ASVT liveness check
that still gates every reply POST), ADR-0022 (the same-AS completion POST seam and the
reset-before-WAIT / double-check-drain rule), ADR-0025 (the WAIT-gate pending probe —
a reachable state the loop must not WAIT on top of), spec §10.4 (`NSFRQE`, **frozen at
M3-2**), spec §5.3 (the executive loop).
**Evidence pins:** `ufsd/src/ufsd#que.c` (`ufsd_dequeue` / `ufsd_dispatch` — the
server-side copy-and-dispatch pattern, and its two warnings: cross-AS POST must be
`__xmpost` not SVC 2, and the request block stays owned by the client-side router until
the client's WAIT returns); `include/nsfreq.h` (the frozen 64-byte layout);
`src/nsfsoc.c` (`soc_complete` — the single completion POST call site, written at M3-1
with "one call site so M3-2 can swap the seam"); `src/nsfv.c` (`nsfv_classify_client`,
the `__super`/`__prob` discipline and the `__xmpost` reply); `include/nsfvsvc.h`
(`NSFV_ANCHOR` at 2184 B, `stage[]` at +136).

---

## Context

Through M4 the application and the stack shared one address space. `nsfreq_submit` pushed
the caller's `NSFRQE` onto a CS-safe queue and POSTed the executive; the executive
dispatched **that same block**, and `soc_complete` POSTed the ECB embedded in it. One
pointer, one copy, no keys, no boundary.

In Phase 2 the application runs in its own address space and the stack in the `NSFS` STC.
The caller's `NSFRQE` lives on its stack in **its** address space, in **its** storage key.
The STC cannot dereference that pointer: there is no cross-memory on this target
(ADR-0036 — `MVCP`/`MVCS` are DAS and absent on 3.8j), which is why ADR-0039 settled on a
CSA bounce moved with `MVCK`.

So the block must be *copied* across, not referenced in place. That is the same conclusion
`ufsd#que.c` reached, and this ADR follows it. What remains to decide is the shape: where
the copy lives, what the executive actually dispatches, how a **parked** request (one that
completes long after `nsfreq_dispatch` returns) signals its reply, and which of the 64
frozen bytes the STC is permitted to write back.

The freeze is the hard constraint. `NSFRQE` is 64 bytes and **frozen at the M3 exit gate**;
`ubuf`@20, `ulen`@24 and `apptok`@56 were defined at M3-2 precisely so Phase 2 would need
no layout change. This ADR does not change the layout, and records below why it does not
have to.

---

## Decision

### 1. Three hops, not two — the executive dispatches an STC-private copy

The naive design copies the caller's `NSFRQE` into the CSA slot and lets the executive
dispatch **the CSA block itself**. We do not do that. The request makes three hops:

```
  app NSFRQE            CSA request slot           STC-private NSFRQE
  (caller stack,   ──►  (anchor, key 0,      ──►   (STC storage, key 8)
   caller key)           the transport)                    │
                                                           │ nsfreq_dispatch
                                                           ▼
                                                    soc_complete POSTs
                                                    the PRIVATE ecb
       ◄── reply POST ──  result fields  ◄── copy-out ──────┘
        (ADR-0040 guard,   written back
         __xmpost)         to the slot
```

**Why the private copy earns its hop.** If the executive dispatched the CSA block,
`soc_complete` would issue its `nsfthr_post` — a real SVC 2 POST — against an ECB in
**key-0 CSA storage**, from the STC's problem-state, key-8 executive. Whether that
succeeds on 3.8j or takes an S0C4/S102 is an open empirical question, and it is exactly
the shape this project has paid for before: host-clean, link-clean, and wrong only on the
live machine. **The private copy means we never have to know.** `soc_complete`'s POST
target is ordinary STC storage in the STC's own key — byte-for-byte the same semantics it
has had since M3-1.

Three further properties fall out, and they are the reason this is the design rather than
a workaround:

- **`nsfsoc.c` is not touched.** `nsfreq_dispatch` takes an `NSFRQE *` and is indifferent
  to where the block lives. No socket, protocol, TCP or UDP file learns that a boundary
  exists. That is a red line of M5-2a, and this design satisfies it structurally rather
  than by care.
- **CSA is touched at exactly two points** — copy-in and copy-out — each a short
  `__super`/`__prob` window, the `ufsd#que.c` discipline.
- **The `q` linkage stays private.** The STC-private copy is queued on the STC's own
  request queue; the caller's queue element never crosses and never means anything on the
  far side.

### 2. `ubuf` and `ulen` are **rewritten** in the private copy — the moved-length contract

This is the correctness crux of M5-2a, and it is where **obligation #2 from ADR-0039
lands**. Before `nsfreq_dispatch` sees the private copy, two fields are replaced:

| Field | In the CSA slot (as the caller wrote it) | In the STC-private copy |
|---|---|---|
| `ubuf`@20 | the **caller-AS** buffer address | `&anchor->stage[0]` — the CSA staging buffer |
| `ulen`@24 | the length the caller **requested** | the count **actually staged** |

**`ubuf`** must be rewritten because a caller-AS pointer dereferenced from the STC's
address space reads the wrong space entirely. The staging buffer is the only address that
means the same thing on both sides.

**`ulen`** must be rewritten because ADR-0039's move clamps at `min(ulen, NSFV_XFER_CHUNK)`
with `NSFV_XFER_CHUNK` = 2048. Hand the protocol op the caller's *requested* `ulen` and a
5000-byte send reads 2952 bytes past the end of a 2048-byte staging buffer **and** reports
5000 bytes moved. Both halves are defects: a buffer overread and a silent over-report.

Setting `ulen` to the staged count fixes both **without any new mechanism**: the existing
protocol code already returns the number of bytes it moved, and `retcode`@40 is already
documented as `EZASOKET RETCODE (0/count or -1)`. So the moved count reaches the caller
through the field that has always carried it.

> **The frozen layout needs no new field, and there is no re-freeze discussion.** The
> obligation ADR-0039 left open — "the `min(ulen, 2048)` clamp is silent and both sides
> infer the moved count from a shared constant" — is discharged by making the clamp
> *visible to the dispatcher* instead of by adding a field to carry it. BSD `send`/`recv`
> return the bytes transferred; so does NSF, through `retcode`.

The truncating case (`ulen > NSFV_XFER_CHUNK`) is therefore a **required live test**, not
an edge case: it is the case that distinguishes this decision from the bug.

### 3. The request-slot layout — appended at +2184, nothing moves

The CSA request slot is a **64-byte `NSFRQE`-shaped area embedded in the existing
`NSFV_ANCHOR`**, appended **after** `stage[]`:

```
  NSFV_ANCHOR
    +0000  eye[8] .. reaped           (unchanged, ADR-0038/0039/0040)
    +0088  stage[2048]                (unchanged — the ADR-0039 staging buffer)
    +0888  rqe[64]                    NEW: the M5-2a request slot
           = 2248 bytes total         (was 2184)
```

**Appending rather than inserting is deliberate.** Stage-0c's live-only regression was a
control-flow fall-through broken by inserting a block into a **moved layout**, and its
lesson was recorded as a rule: an asm change under a moved layout is validated by
re-running *every* stage's live gate, not just the new one. Appending after `stage[]`
keeps `stage` at +136 and leaves every `ANC*` `EQU` in `asm/nsfvsvc.asm` untouched, so
that surface is near-zero here. **All four Stage-0 gates are still re-run** — but we
expect them green rather than hope so.

One new `NSFV_OFF_ASSERT(NSFV_ANCHOR, rqe, 2184)` joins the existing set. Per ADR-0040's
own lesson, a size assert cannot catch a field that *moved*; the offset asserts can.

**Single slot, by construction.** One request area, one in-flight request, one client.
The 64-slot (= MAXSOC) CSA pool, per-invocation SVRB storage and everything concurrency
implies are **sub-step (b)** and are deliberately absent here. Where the code would
naturally generalise to a pool, it is left as a seam and not filled.

### 4. Which fields the STC may write back

Copy-out writes **only** these into the CSA slot:

| Field | Why |
|---|---|
| `retcode`@40 | the EZASOKET RETCODE — **and the moved count** (§2) |
| `errno_`@44 | the EZASOKET ERRNO |
| `apptok`@56 | `RQ_INITAPI` output; `RQ_SOCKET`/`RQ_TERMAPI` carry it as input |
| `p1`/`p2`/`p3`@28/32/36 | fn-specific outputs (`accept`'s new descriptor, `getsockname`'s address and port) |

**Caller-owned, never written back:**

| Field | Why not |
|---|---|
| `q`@0 | queue linkage — the private copy's is the STC's own and is meaningless to the caller |
| `eye`@8, `fn`@12, `flags`@14, `sockdesc`@16 | caller inputs; echoing them back can only mask a mismatch |
| `ubuf`@20 | **a live hazard** — this holds the STC's staging address in the private copy; writing it into the client's block hands the caller a pointer into another address space |
| `ecb`@48 | the caller's own ECB; vestigial in Phase 2 (§5) but still the caller's storage |
| `reqid`@52 | trace correlation, caller-assigned |

For a read-direction op the staged data is moved back out to the caller's buffer by the
ADR-0039 read-out path, clamped to the same staged count.

### 5. Completion detection — an end-of-pass check on the private ECB

`nsfreq_dispatch` returning does **not** mean the request is complete. The executive parks
blocking ops (`recv` on an empty rxq, `accept` on an empty acceptq, `connect` in
`SYN_SENT`) and completes them later, from a device or timer event. The reply must go when
`soc_complete` fires, not when dispatch returns.

The STC therefore checks, **at the end of each executive loop pass**, whether the in-flight
private `NSFRQE`'s ECB is posted. If it is, the STC copies the result fields out to the CSA
slot, moves any read-direction data, and runs the reply path (§6).

This works because **a completion is always caused by an event the loop just processed** —
an immediate dispatch, a device wake, or a timer expiry — so a pass always follows a
completion. Testing a private, key-8 ECB once per pass costs nothing.

Two rules this must respect, both already paid for elsewhere in the project:

- **Test the POSTED bit, never non-zero** (§4 of `CLAUDE.md`): a satisfied multi-ECB WAIT
  leaves an RB-address remnant in un-posted ECBs.
- **"Completed but not yet replied" joins the WAIT-gate pending probe**, alongside
  `nsfdev_work_pending` and `nsfreq_pending`. Without it the loop can reach that state and
  then commit to a WAIT on top of it — the shape of ADR-0025's defect (2) and the #27
  lost-request class.

### 6. The reply path is ADR-0040's, unchanged

The STC still classifies the client with the ASVT check **immediately before** the reply
POST — `asvtenty[asid-1]` available bit → DEAD, ASCB-address mismatch → DEAD (the
ASID-reuse row), equal → LIVE — reaps a DEAD request instead of posting into it, and holds
an UNKNOWN one. The safe-side asymmetry is unchanged: a dead client called live has its
storage freed underneath it; a live client called dead merely leaks a slot.

The guard is **used as proven and must not regress**; the Stage-0c gate (`TSTDEATH`) is
part of M5-2a's regression set for exactly that reason.

### 7. `nsfreq_submit` / `nsfreq_wait` — the swap, and the surfaces above it

`nsfreq_submit` copies the caller's `NSFRQE` into the CSA slot and issues the SVC;
`nsfreq_wait` blocks on the anchor's **`reply_ecb`** (the ADR-0038 reply path), **not** on
`&r->ecb` — the caller's own ECB is vestigial in Phase 2. On wake, the result fields (§4)
are copied back into the caller's `NSFRQE`.

**Everything above that seam is unchanged.** NSFEZA still builds the `NSFRQE` on the
caller's stack and still calls `nsfreq_call`; it uses `nsfreq_call` at *every* call site,
so there is no async submit/wait split to preserve. **The C, EZASOKET and EZASMI surfaces
do not change at all — applications relink only.** That is the whole point of the
`NSFRQE` having been made the phase boundary at M3-2.

---

## Consequences

**Good.**
- The freeze holds: 64 bytes, no new field, no re-freeze discussion.
- No protocol, socket, TCP or UDP file learns that an address-space boundary exists.
- `soc_complete` is untouched, and we never need to know whether a key-0 CSA POST from a
  problem-state key-8 task works — a design property, not an untested assumption.
- Obligation #2 is discharged on the real path, and its failure mode has a required live
  test rather than a comment.

**Costs, accepted.**
- **A third copy per request.** 64 bytes, twice per round trip. Against an SVC, two key
  switches, a cross-AS POST and a WAIT, it is not measurable.
- **One in-flight request.** A second client gets `NSFV_RC_NOREQ`. Correct for (a) and the
  reason (b) exists.
- **A CSA anchor 64 bytes larger** (2184 → 2248), which is 64 more bytes of a resource
  MVS 3.8j has little of. The pool in (b) will make this the dominant term and will need
  its own sizing argument.

---

## What this ADR deliberately does not decide

Each of these is a named later sub-step. Seams are laid; none are filled.

| Deferred | Sub-step |
|---|---|
| Concurrency: SVRB per-invocation storage, the 64-slot (= MAXSOC) CSA pool, per-client staging | **(b)** |
| The **write-out key window** — the read-out stores into caller-supplied addresses under PSW key 0, so the keyed protection is half-closed. **After M5-2a there are TWO such destinations: `ubuf` and `rqeimg` (the NSFRQE image)** — see the second correction below; the probe must cover both. Opens with an empirical probe | **(b)** |
| `owner_ascb` sweep for sockets that outlive their request, reusing the ADR-0040 classifier (**not** `ufsd_sess_cleanup`'s ASID-only test); removal of the probe scaffolding (`NSFV_REQ_ORPHAN`, `pascb`/`pasid`) | **(c)** |
| Security validation of caller-supplied fields; whether **UNKNOWN at shutdown** differs from UNKNOWN at service time; extracting the guard arithmetic for a host test | **(d)** |
| Two-address-space stress | **(e)** |

M5-3 (TCP hardening), M5-4 (NETSTAT) and RACF integration are separate tracks and are not
folded in.

---

## Alternatives considered

**Dispatch the CSA block directly (two hops).** Rejected: it puts `soc_complete`'s SVC 2
POST against key-0 CSA storage from a problem-state key-8 task — an open empirical
question in the exact host-clean/link-clean/live-wrong shape this project has repeatedly
paid for. The private copy costs 64 bytes and removes the question.

**Have the STC read the caller's `NSFRQE` in place.** Not available: no cross-memory
services on this target (ADR-0036). This is the constraint that produced ADR-0039's bounce
in the first place.

**Add a `moved` field to `NSFRQE` for the staged count.** Rejected: the layout is frozen,
and it is unnecessary — rewriting `ulen` in the private copy makes the existing `retcode`
carry the true count, which is also what BSD does.

**Hook `soc_complete` with a transport-aware completion seam.** Rejected: it changes a
socket-layer file for a transport concern, against an M5-2a red line. The end-of-pass check
on the private ECB gets the same signal without the coupling — and the seam comment
M3-1 left there stays available for a future phase that genuinely needs it.

**Poll the CSA slot's `req_state` from the executive instead of an ECB.** Rejected: it
puts a key-0 read in the loop's hot path every pass, where the private ECB check is free.

---

## Addendum (2026-08-23) — the STC's wake ECB lives in STC-private key-8 storage

Countersigned decision, recorded after the design review. §5 says the executive checks the
private ECB at the end of each pass; it did not say **how the executive is woken in the
first place**, and that turned out to have exactly one safe answer.

**The problem.** The Stage-0 probe STC WAITs on `anchor->server_ecb` — which is CSA, key 0
— and does so **in supervisor state**, with `__super(PSWKEY0)` bracketing the WAIT, for
that reason. The NSF executive does not have that option: `evt_mainloop` WAITs through
libc370's `ecb_waitlist` from **problem state, key 8**. Putting a key-0 CSA ECB into that
ECBLIST is not merely untested — it is a **documented abend**: `ufsd/docs/cross-as-reference.md`
records `S047` and `X'201'` for exactly "WAIT (SVC 1) issued from problem state for an ECB
in key-0 storage".

**The decision.** The STC publishes, in the anchor, the address of an ECB in **its own
key-8 private storage** (`server_ecb_ptr`), and the SVC routine POSTs *that* address
instead of `&anchor->server_ecb`. The executive then WAITs on ordinary problem-state key-8
storage and nothing about the WAIT is novel.

The mechanism is not a guess. `ufsd/docs/cross-as-reference.md` documents the mirror image
as its final working design: *"STC wakes client — `__xmpost(client_ascb, client_ecb_ptr, 0)`
… client_ecb location: local stack var in ufsdssir, key-8, **NOT** in CSA."* Cross-AS POST
takes an ASCB and interprets the ECB address in *that* address space; private storage is
fine. Here the poster sits in the client's address space and the target is the STC, which
changes nothing about the mechanism.

`server_ecb_ptr` is **appended** (at +2248, after `rqe`), same rule as §3, and **zero means
"not published"**: the SVC routine then falls back to `&anchor->server_ecb`, so the
Stage-0 probe STC — which genuinely wants the key-0 CSA ECB it supervisor-WAITs on —
keeps working byte-for-byte. That fallback is what lets the four Stage-0 gates stay a
regression rather than becoming a rewrite.

### Publication order

**The ECB address is published BEFORE the SVC slot is stolen, and invalidated AFTER the
slot is restored.** The slot steal is the "we are open for business" signal; a client must
never find a stolen slot together with an unpublished or stale ECB address. Teardown runs
the reverse order for the same reason.

### The mirrored STC-death race — residual risk, recorded

ADR-0040 settled what happens when the **client** dies with a request in flight. This
addendum introduces the mirror: the STC's wake ECB now lives in STC-**private** storage, so
if the STC address space dies between a client's anchor validation and its POST, the client
can POST into storage that has been freed and possibly reused.

This is the same shape as the hazard ADR-0040 §7 recorded honestly in the other direction,
and it gets the same treatment rather than being inherited silently. Mitigations already in
the transport: the SVC routine re-validates the anchor eyecatcher and the `ACTIVE` flag
before it posts, shutdown clears `ACTIVE` before it drains, and the in-flight counter keeps
CSA alive while any client is inside the routine. The window that remains is narrow and
open only across an STC memterm.

It is **not closed in M5-2a**, and no client-side check can close it alone — the same
asymmetry as ADR-0040: the STC checking a client can consult the ASVT, but a client
checking the STC would need the equivalent lookup on the server's ASCB. That is the natural
companion to the `owner_ascb` sweep and belongs with it in **(c)/(d)**, not here.

### Operational note

`P NSFV` takes **~27 s** to reach the syslog (MVSCE syslog lag plus the in-flight drain),
then reports `NSFV095I SVC 239 RESTORED` and ends cleanly. **This is not a hang — do not
re-issue the stop.** Recorded because the natural reaction to a silent console is to send
it again, and a second stop against a draining STC is exactly the wrong move.

### Correction (2026-08-23) — what the 444 Stage-0 asserts actually prove

An earlier note claimed the four Stage-0 gates exercised the key-8 POST path. **They do
not, and the distinction is the whole point of recording it.**

The probe STC publishes no `server_ecb_ptr`, so `ANCSEPTR` is zero and every one of those
444 assertions takes the **CSA fallback** branch — `LTR R11,R11 / BNZ PSTECBX` in
`asm/nsfvsvc.asm` falls through to `LA R11,ANCSECB(,R2)`. So:

- **The 444 Stage-0 asserts prove the FALLBACK branch is intact** — which is exactly what
  they are there for: they are the regression that says the probe still works unchanged.
- **The key-8 branch is proven by `TSTRQXM`'s passing rows, and by nothing else.**

Written as one sentence — "the Stage-0 gates cover it" — this reads in six months as
coverage for a path none of those runs enters. That is this project's most expensive
failure class (a green result that measures something adjacent to the claim), relocated
into the documentation, where no test can catch it.

### The parked-request path, first proven 2026-08-23

§5's end-of-pass completion check and the un-posted-private-ECB rule had no live evidence
until `TSTRQXM` grew its TCP case. Every synchronous verb — INITAPI, SOCKET, BIND,
GETSOCKNAME, CLOSE, TERMAPI — completes inside the dispatcher and never parks, so the
twelve rows that passed before it proved the crossing but said nothing about §5. A TCP
`connect` and `send` do park. Both now complete across the boundary live, which is the
first evidence that the design in §5 works.

### The moved-length contract is a STREAM contract

Obligation #2's live case is a **TCP `send`**, not a UDP `sendto`, and the reason is
semantic rather than incidental. `SENDTO` on a datagram socket is **atomic** in BSD and in
EZASOKET: the whole datagram goes or the call fails. A partial move there is not a short
write but a **truncated datagram**, and an application looping on the returned count would
send the remainder as a second datagram and corrupt its own framing. Pinning
"`SENDTO` 5000 → 2048 moved" would have pinned semantics NSF's own surface contradicts.

A short return is correct, expected and loop-safe exactly where BSD says it is: `send` on
a connected stream socket. UDP's honest case is the opposite one — a `sendto` above
`MTU − 28` returns `EMSGSIZE` (spec §11.3, v1 does not fragment), which also proves a
specific errno crosses from a **protocol** op rather than from the dispatcher.

### Correction (2026-08-23, second) — the write-out key window has TWO destinations

The deferred-obligations table above carries ADR-0039's wording forward verbatim: *"the
read-out still stores into `ubuf` under PSW key 0."* **After M5-2a that is no longer the
whole picture.**

`RQEOUT` now performs **two** key-0 stores into caller-supplied addresses:

| Destination | What it is | Since |
|---|---|---|
| `ubuf` | the caller's data buffer, address supplied in the request block | ADR-0039 |
| **`rqeimg`** | the caller's **64-byte NSFRQE image**, address supplied in the request block | **M5-2a** |

Both are written while the routine runs under PSW key 0, so the hardware does **not** check
either against the caller's own key. The `rqeimg` one is newer and no less dangerous: it is
a pointer a client puts in its own `NSFV_REQ`.

**M5-2b's empirical probe must cover both.** This obligation text is the input to that
step's scope; naming only `ubuf` would scope the probe to one destination and let the
second go quiet — which is precisely how a known hazard becomes an unknown one.

Note also the distinction the `RQEOUT` header comment previously blurred (now corrected in
`asm/nsfvsvc.asm`): the **source** key being 0 is correct and harmless — the staging buffer
and the slot *are* key-0 CSA. The hazard is entirely on the **destination** side.

### Update (2026-08-23) — the write-out key window is CLOSED for BOTH destinations

M5-2b1 discharges the **mechanism** half of the deferred-obligations row above. Both
key-0 stores this ADR named — `ubuf` and the M5-2a `rqeimg` — now run inside a narrow
`SPKA` window set to the **caller's own key**, so the hardware checks each
caller-supplied destination against the key that owns it. The move is a plain `MVC`
reached by `EX`; the key comes from `TCBPKF` (TCB+`X'1C'`, reached via `PSATOLD`), never
a hardcoded 8. The full decision, the rejected `MVCK`-inside-the-window alternative, the
`as370` listing bytes and the live gates are recorded **append-only in ADR-0039**, which
framed the obligation — this entry only re-reads the row.

**The obligation row above should now read: mechanism CLOSED for both destinations;
recovery still OPEN.** Concretely:

| Was | Now |
|---|---|
| `ubuf` written under PSW key 0 | written under the caller's key — hardware-checked |
| `rqeimg` written under PSW key 0 | written under the caller's key — hardware-checked |
| a wrong/hostile pointer is a **silent clobber** | a wrong/hostile pointer is a **caught `S0C4`** |
| — | **recovery from that fault: still open**, an M5-2 item ADR-0039 already names |

**What closing it exposed, so the next reader does not have to rediscover it.** A fault in
the write-out leaves the anchor dirty: `req_state` stuck at **DONE** (the slot busy
forever, every later request `RCNOREQ`) and `inflight` leaked. That shape is *worse* than
the in-direction's (which leaves the slot FREE, because the fault precedes publication),
and it is the input to deciding where recovery lands. ADR-0039's b1 entry carries the
measured and reasoned assessment in full, including two adjacent findings that are **not**
b1's to fix: `UNSTAGE` cannot recover a pre-publication leak, and `nsfsx_stop()` does not
drain `inflight` at all — unlike the Stage-0 probe STC's `nsfv_drain`.

**`XFEROUT` is deliberately untouched** and keeps its key-0 write-out: it is Stage-0b probe
scaffolding, carries no NSFRQE and no application data, and is already listed for removal
with the other probe verbs in (c). Do not read its key-0 store as this transport's.

Nothing in this ADR's own subject changed: the anchor layout did not move, `NSFV_REQ` and
`NSFV_ANCHOR` are unchanged, **NSFRQE stays frozen at 64 bytes**, and the C / EZASOKET /
EZASMI surfaces are untouched — applications relink only.