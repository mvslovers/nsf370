# ADR-0042 — Phase-2 request slots: a 64-slot CSA pool claimed by per-slot compare-and-swap

**Status:** Proposed (2026-08-25). Settles **how a second client gets a request slot**. Through
M5-2a the CSA anchor held exactly ONE request area, and the SVC routine rejected any caller
that found it busy (`RCNOREQ`). That was correct for a single sequential probe client and is
useless for an application stack: `MAXSOC` is 64, and a stack whose transport serialises every
client at the door is not a stack. This ADR replaces the single area with a **pool of 64
independent slots**, each claimed by a compare-and-swap on **its own** state word.

It is sub-step **(b3) of M5-2b**, itself the second of five (a–e). **It builds the mechanism;
it does not prove contention.** Two clients racing on the same slot word is **b4**, the full
two-address-space stress is **e**. See "What this ADR deliberately does not decide".

**Relates to:** ADR-0038 (the private-SVC transport, the CSA anchor, the RENT/shared-scratch
caveat this ADR finally retires in full), ADR-0039 (the `MVCK` staging bounce and the
`min(ulen, 2048)` clamp — still the only thing bounding a move, see Consequence 4),
ADR-0040 (the client-death guard, whose classifier becomes per-slot here), ADR-0041 (the
`NSFRQE` crossing and the STC-private dispatch copy, unchanged), spec §10.4 (`NSFRQE`,
**frozen at M3-2** and untouched by this ADR).
**Evidence pins:** `asm/nsfxq.asm` (the CS primitive **and its ABA warning**, quoted below —
the reason this design has no free list), `include/clibos.h` (libc370 `__cas`, whose own
documentation uses a slot claim as its worked example), `test/mvs/tstmvcd.c` (b0's measured
CSA budget: 2064 KB total, largest contiguous `SP=241` `GETMAIN` ≥ 1 MB).

---

## Context

`NSFV_ANCHOR` carries one `req_state` and one set of request fields. The SVC routine takes
that area by testing `req_state` for FREE and storing PENDING; a caller that finds any other
state gets `RCNOREQ` and goes away. Nothing about it generalises: the take is not atomic
(a load, a branch and a store), the fields have no owner, and `stage[]` is shared scratch.

A pool has to answer three questions that a single slot never poses:

1. **How does a claim stay atomic** when two address spaces reach the same word?
2. **Which slot** is a given client's, at every later hop — the reply POST, the death guard,
   the STC's drain?
3. **What happens when they are all taken?**

The first is the interesting one, because the obvious answer is wrong here.

---

## Decision

### 1. A per-slot claim word, compared and swapped in place. No free list.

Each slot's own `req_state` is the claim word. The routine walks the array and, at each slot,
issues `CS` comparing FREE and swapping in CLAIMED. First success wins. A failed `CS` means
somebody else took that slot, so the walk **advances** — it never retries the same slot.

This is **ABA-free by construction, and the reason is worth stating exactly**: the location
being compared *is* the resource. There is no separate head pointer whose value could return
to a previous state while the thing it names has changed underneath it. `asm/nsfxq.asm`'s own
header records why that matters here:

> ABA safety: exactly one consumer, which removes work by swapping out the WHOLE chain at once
> (`xq_drain`); individual elements are never popped, so a node cannot be freed and re-pushed
> to reappear as a stale-but-equal head. **Do NOT add a per-element pop.**

A free list is exactly a per-element pop. The project's only other CS structure avoids one
deliberately, and it can afford to because it has a single consumer draining whole chains — a
slot pool has neither property. So: no free list, no head pointer, no `CDS` to carry a
generation counter alongside a pointer. The scan costs at most 64 `CS` attempts on an
uncontended machine and is O(1) in the common case, because the first FREE slot is usually
slot 0.

### 2. Claim with `CS`. Release with a plain `ST`. Reap with `CS`.

Three different rules, and the differences are not stylistic.

**Claim: `CS`.** Two clients may reach the same word.

**Release: plain `ST`.** The owner releasing its own slot races with nobody — no other client
can claim a slot that is not FREE, and the owner is the only party that turns it FREE. S/370
stores are ordered, so the release is simply the last store the owner makes, after everything
the client will read.

**Reap: `CS`, and this is the one real ABA in the design.** The STC's death guard reads slot 7,
classifies its client DEAD, and decides to reclaim it. Between that read and the reclaim, the
owner can complete, release the slot, and a *new* client can claim it. A blind `ST FREE` from
the reaper then steals a live client's slot — a slot handed to two owners at once, which is the
worst failure this whole layer can produce. The reap **tolerates a failed compare by moving
on**: it means the world changed under it, and the request it meant to reap no longer exists.

**Correction, found while implementing this and folded back in.** The obvious form —
`__cas(&slot->req_state, observed, FREE)` — is *still wrong*, in a smaller way, because
reclaiming a slot means **clearing a dead client's data out of CSA** and the clear cannot
happen before the compare (a failed compare would then have wiped somebody else's slot) or
after a store to FREE (a new client can claim it and begin staging into storage the reaper is
still wiping). So the reap is **two moves**:

1. `__cas(&slot->req_state, observed, CLAIMED)` — take the slot out of circulation. `CLAIMED`
   is a state no other party can claim.
2. clear, then a plain `ST FREE` — by then the reaper is the outright owner and races with
   nobody, exactly like a client releasing its own slot.

This makes `CLAIMED` mean **"owned, not available"** rather than "owned by a client": the
reaper is an owner too. `nsfreqx_slot_legal` therefore admits `PENDING/HELD/DONE → CLAIMED`,
and TSTREQX pins it.

"ABA-free by construction" covers the **claim**. It does not cover a third observer, and the
reaper is a third observer.

### 3. The claim loop does not run under a borrowed key.

`MOVEOUT` (ADR-0039 / M5-2b1) stays the **only** block in `nsfvsvc.asm` that runs under a PSW
key other than 0, and this ADR does not change that. A claim needs an *interlocked compare*;
that is a serialisation problem. A key window makes the hardware check a *destination against
its owner*; that is a protection problem. They are unrelated, and wrapping a retry loop in
`SPKA` would put an unbounded number of instructions under a key the routine does not control,
for nothing. `MOVEOUT`'s own header already carries this warning addressed to this step.

### 4. Sizing: 64 full slots, staging inside the slot.

`MAXSOC` is 64 (`nsfeza.c` clamps to the pool limit), so 64 slots is one per possible socket and
the pool can never be the thing that fails first.

| | |
|---|---|
| Slot | **2144 bytes** (`stage[2048]` + 96 bytes of per-slot fields) |
| Global header | **48 bytes** |
| Pool total | 48 + 64 × 2144 = **137,264 bytes** |
| Measured CSA on MVSCE (b0) | 2064 KB total; largest contiguous `SP=241` `GETMAIN` **≥ 1 MB** |
| Share of CSA | **6.5 %** |

**Staging stays inside the slot.** Decoupling the 2048-byte buffers into a smaller separately
claimed pool was considered and rejected: it saves roughly 96 KB — 0.05 % of CSA — and pays for
it with a second claim path, a partial-claim rollback when the second claim fails after the
first succeeded, and a second exhaustion case. All of that lands in the one layer of this
project that cannot be host-tested. The trade is bad in the direction that matters.

**The slot is NOT padded to a power of two.** That reflex costs 26 KB at 2560 and 120 KB at
4096, and buys nothing here, because **the scan walks a pointer**: it starts at slot 0 and on
failure does `LA Rslot,SLOTLEN(,Rslot)`, bounded by a count. There is no index multiply
anywhere, so no power of two is needed to turn one into a shift. 2144 fits an `LA` displacement
(max 4095) and every field offset within a slot fits base-displacement addressing directly.
The slot length remains a compile-time constant.

### 5. Layout: a fixed global header, then the slot array.

Splitting the anchor is what keeps the EQU churn manageable — without it every per-slot field
would need 64 EQUs or an index multiply.

**Global header** (keeps the `ANC*` EQUs): `eye[8]`, `version`, `flags`, `server_ecb`,
`server_ascb`, `inflight`, `served`, `reaped`, `server_ecb_ptr`, `nslots`, `exhausted`.
48 bytes — a multiple of 8, so the slot array starts doubleword-aligned and, since 2144 is
also a multiple of 8, **every** slot is. That alignment is not cosmetic: `CS` requires its
operand to be fullword-aligned, and `req_state` is at slot offset 0.

**`NSFV_SLOT`** (new `SL*` EQUs, all relative to a slot-base register): `req_state`,
`req_token`, `reply_ecb`, `req_ascb`, `req_asid`, `xfunc`, `xlen`, `rqe[64]`, `rqe_guard[4]`,
`stage[2048]`. Both structures carry their own `NSF_SIZE_ASSERT` and their own
`NSFV_OFF_ASSERT` block — a size assert cannot catch a field that moved (ADR-0040).

**`csasave[18]` comes out.** It has been dead since M5-2b2, kept only because removing it would
shift every later field and cost a Stage-0 round for no benefit. This is the layout move it was
waiting for, and its removal is free here because everything moves anyway. With it goes b2's
five-word self-check — see Decision 8.

### 6. The routine bounds its scan by the header's `nslots`, and checks the anchor version.

The SVC routine is `NSFVSVC`, a **separate load module** `__loadhi`'d into CSA. The STC is
`NSFS`. They are deployed together but they are not the same binary, and this project has a
documented failure mode where they diverge: `make deploy` fails mid-chain while an STC holds
`NSF.LINKLIB`, and the run afterwards silently uses the previously deployed module
(CLAUDE.md §5). A stale router against a new STC is not a wrong answer — it is a scan striding
by the wrong slot length, or running off the end of the allocation into adjacent CSA. That is
the IPL-class overrun ADR-0039 §3 names.

Two cheap defences, and they are cheap enough that their cost is not worth discussing:

- **The scan's bound comes from the header's `nslots` field**, which the STC wrote when it
  allocated the storage — never from the routine's own EQU. The party that knows how much
  storage exists is the party that allocated it.
- **The routine checks `version` against a compile-time constant** and returns `RCCORR` on a
  mismatch. `version` is bumped **1 → 2** in the same change that moves the layout, which is
  what makes the check load-bearing rather than decorative.

### 7. Exhaustion: `ENOBUFS`, immediately, with no retry.

All 64 taken → the routine returns a new `RCNOBUF`, which maps to `NSF_ENOBUFS` (55).

**No spinning and no blocking.** The routine runs in **supervisor state, key 0, inside a
client's address space**, on that client's TCB. A retry loop there burns a dispatchable unit at
key 0 waiting for an unrelated address space to make progress; a WAIT there parks a task
holding an SVRB in a state nothing will reliably post. Neither is acceptable, and neither is
necessary: `ENOBUFS` is the project's normal, expected exhaustion answer — the M0 invariant that
`mm_alloc` returning NULL is handled gracefully by every caller, never as an ABEND, applies
verbatim to a slot pool.

The header's `exhausted` counter records how often this happened, because a pool that is
regularly full is a sizing fact and should not have to be inferred from client-side errnos.

### 8. b2's save-area self-check is retired, not rehomed (issue #61).

The five words in `ANCSAVE` were **one-time evidence** that the SVRB's `RBEXSAVE` is a real
per-invocation save area. That evidence has been collected, it is recorded in ADR-0038, and the
prediction it tested was refuted with a measured canary surviving both the POST and the WAIT.
Carrying five stores and three `CLC`s per request forever, on the hot path, to keep re-proving
a settled fact is not a trade worth making.

**`TSTRQXF` (C) is converted rather than deleted.** It becomes the pool's own positive check —
the same *shape* of evidence (stamp something, read it back, prove it took) aimed at the thing
that is now unproven instead of the thing that is now settled. Deleting the test along with the
mechanism would leave the new mechanism resting on the absence of a complaint, which §8.5 of
CLAUDE.md is specifically about.

### 9. `nsfsx_stop()` drains before it frees (issue #55).

It currently frees the CSA unconditionally, with no reference to `inflight`. That was inert
while nothing read the count. **The pool makes it live**, and not only after a fault: with 64
clients claimed and service serialised (Decision 10), 63 are legitimately parked on their own
reply ECBs during normal operation, with `inflight` counting all of them. Freeing CSA underneath
them is a wild-store generator across every one.

`nsfsx_stop` therefore takes the shape `nsfv_drain` already uses in the probe STC: nudge parked
clients, poll `inflight` to a ceiling, and free CSA **only** on reaching zero — retaining it
with a message otherwise. Retaining CSA leaks 134 KB until IPL; freeing it under a live client
corrupts that client's address space. The asymmetry decides it, the same way it decides the
death guard (ADR-0040).

**The nudge must reach every claimed slot.** The probe's single-slot `nsfv_wake_parked` does not
survive the pool, and a drain that nudges one client and then times out on the other 63 would
look exactly like a hang.

### 10. b3 makes the CLAIM concurrent. Service stays serialised.

The STC keeps one private `NSFRQE` and one busy flag, plus a record of **which slot** it is
serving. Its drain scans for a PENDING slot instead of reading a fixed one. Sixty-four clients
can therefore have requests outstanding simultaneously; they are *serviced* one at a time, each
parked on its own reply ECB until its turn.

That is the honest minimum for this step and it is a real advance — the door is no longer the
bottleneck. Concurrent *service* is **b4**.

---

## Consequences

1. **The anchor grows from 2256 bytes to 137,264** and its layout moves substantially. Every
   `ANC*` EQU is revisited and a full Stage-0 round is the gate — an asm change under a moved
   layout is validated by re-running every stage's live gate, not just the new one (ADR-0040's
   fall-through lesson).
2. **`RCNOREQ` becomes nearly unreachable.** It survives for a slot-specific probe path; the
   ordinary busy answer is now `RCNOBUF` after a full scan.
3. **The death guard and the reap become per-slot.** `nsfsx_client_state()` and `nsfsx_reap()`
   read global `req_*` fields that no longer exist. The classifier's *arithmetic* is extracted
   into `src/nsfreqx.c` as a pure function and pinned by `TSTREQX` (the range check, the AVAIL
   bit, the address compare, all four verdicts), so the per-slot rewrite is a call rather than a
   hand-copied truth table. This also discharges an M5-2 obligation inherited from Stage-0c,
   which asked for exactly this extraction.
4. **An over-long move now lands on the NEXT slot's claim word** rather than on adjacent CSA.
   That is a different shape of hazard, not a smaller one — corrupting a live claim word hands
   one slot to two owners. **The `min(ulen, 2048)` clamp is the only thing preventing it, and it
   is unchanged from M5-2b1.** This ADR does not improve that bound; it states it. (Reordering
   the slot to put `stage[]` first, so an overrun would hit `rqe_guard`, was considered and
   rejected: the guard is checked *before dispatch* against a value stamped at allocation, so an
   overrun during the write-in would be detected only on the *next* request through that slot,
   if ever. It trades a known hazard for a differently-shaped one.)
5. **`stage[]` remains shared scratch, now per-slot** — which is what ADR-0039 said M5-2
   concurrency would need, and it is now true. ADR-0038's RENT/shared-scratch caveat is fully
   retired: the save area moved to the SVRB in b2, and the last shared writable region in the
   anchor becomes per-slot here.

---

## What this ADR deliberately does not decide

- **Contention.** Two clients racing on the same slot word are **b4**. The `CS` makes the claim
  correct by construction, but construction is not a live gate, and this project's record on
  "construction is obviously right" is not perfect — b1 and b2 each cost a round to a register
  that was obviously fine.
- **Concurrent service.** One in-service request at a time (Decision 10). **b4.**
- **Two-address-space stress.** **e.**
- **Fault recovery and address validation.** Still open, still named by ADR-0039 and ADR-0041.
  A slot whose owner faults mid-request leaves it **CLAIMED**, and `nsfreqx_reap_ok` excludes
  CLAIMED, so the death guard never reclaims it — the slot leaks until the STC stops.

  **Correcting a claim an earlier draft of this ADR made:** that exclusion is *not* redundant
  with the classifier. Identity (`req_ascb` / `req_asid`) is recorded at the **claim**,
  immediately after the `CS` and before any staging, so a CLAIMED slot has a real ASCB and
  `nsfreqx_classify` answers LIVE or DEAD for it. Safety rests on the predicate's explicit
  exclusion and on nothing else — which matters, because a rule believed redundant is a rule
  someone deletes.

  Because it *is* classifiable, the leak is closable — but not by widening the predicate. The
  two-move reap proves exclusivity with `CS(observed → CLAIMED)`; when the observed state
  already **is** CLAIMED that compare succeeds trivially and cannot distinguish "I took it"
  from "the live owner still has it". Closing it needs a distinct fourth state,
  `CS(CLAIMED → REAPING)`. Whether to spend that belongs with fault recovery.
- **The probe scaffolding.** This step *grows* it by one verb (a slot pre-setter the live
  exhaustion and skip checks need). Removing it is **M5-2c**, and it is a **security** item, not
  hygiene: `FNXFER` is reachable by an unauthorised client and still stores under key 0.

---

## Alternatives considered

**A free list with a head pointer.** The classic lock-free pool. Rejected: the pop is an ABA in
its textbook form, and `asm/nsfxq.asm`'s header forbids exactly this ("Do NOT add a per-element
pop") for the project's only other CS structure. Fixing it needs a generation counter beside the
pointer, i.e. `CDS`, i.e. Alternative B.

**`CDS` with a generation counter.** Solves the free-list ABA properly. Rejected because it
solves a problem this design does not have: with the claim word *being* the resource, there is
no pointer to go stale. Paying a double-word compare, an alignment constraint and a second
mnemonic `as370` would have to be verified on (the `MVCK` finding of ADR-0039 is one step away
from a repeat) to fix a problem introduced by a structure we are not using is backwards.

**A spin lock around a conventional allocator.** Rejected on where it would spin: supervisor
state, key 0, inside a client's address space (Decision 7).

**Padding the slot to 4096.** Rejected: 120 KB for an index multiply the design does not
perform (Decision 4).

**Decoupled staging buffers.** Rejected: ~96 KB saved against a second claim path, a
partial-claim rollback and a second exhaustion case, in the layer that cannot be host-tested
(Decision 4).

---

## Annotation — M5-2b4: contention measured, and the branch nobody had entered

**Appended 2026-08-25.** Nothing above is retracted. This records what the step that follows
b3 measured, what it could not measure, and one thing it found by accident.

### 1. A failed `CS` is invisible, so the gate needed instrumentation

The problem had to be stated before the test could be designed. A client that scans, finds slot
K taken and moves to K+1 is externally **indistinguishable** from one that found K free, lost the
compare and moved on — and from one that simply started at K+1. No arrangement of clients and
slots recovers the difference, so a gate built on "two clients ran and both were served
correctly" would have proved nothing about the claim discipline. That is the same
non-discriminating shape this milestone paid for three times already (TSTRQXF (B) twice, a
skipped gate reporting CC 0, and `nsfreqx_reap_ok` pinned but unwired).

So the anchor header gained **`collisions`**, incremented once per `CS` that failed during a
claim scan, plus **one reserved word** — because b3 left the header with no slack, which is the
entire reason one diagnostic word costs a full Stage-0 round. Header 48 → 56, slot array moves
by 8, `NSFV_ANCHOR_VER` 2 → 3, slot internals unchanged.

**What the counter means, exactly:** *a claim attempt found a slot that was not FREE at the
instant of the compare.* **What it cannot separate:** "the slot was already busy" from "I lost a
simultaneous race" — `CS` reports only the value it found, and the routine performs no load
before the compare that a second value could be compared against. Adding one would sharpen it
into a true lost-race count at the price of a second counter and a second word; that is what
`rsvd0` is there for, and it was **not** done here. Worth knowing when reading a number from it:
the MVSCE stand runs Hercules with `NUMCPU 2` and MVS dispatches on both processors (measured:
both `Processor CP00` and `CP01` threads executing), so a genuinely simultaneous compare is
physically possible here rather than only defended against on paper.

**The increment is deliberately not interlocked** — a plain `L`/`LA`/`ST`, not the `CS` loop
`exhausted` uses next door. A lost update makes it under-report, never over-report, so
"collisions ≥ 1" stays sound while "collisions == N" is not a number to build on. Aligned
fullword stores do not tear. `exhausted` can afford a `CS` loop because a full pool is rare;
this sits inside the routine's one hot loop. Do not "fix" it into a `CS` loop.

### 2. Three witnesses, and a negative control built from the same code

`TSTRQXC` is one program with roles selected by PARM, and that is deliberate: the no-PARM SOLO
run is the **negative control** for the two-client run, and a negative control built out of
different code proves less.

**The two phases do different jobs, and an earlier draft of this annotation got that wrong.** It
presented phase 1's `collisions` delta and phase 2's served/refused split together, "because the
shape of them is the evidence". They do not carry the same weight, and phase 1 carries none of it.

**Phase 1 is a MECHANISM check.** With the other client sitting on slot 0, every one of A's scans
fails exactly one compare and lands on slot 1 — so `collisions` **must** read one per request and
A's index **must** be non-zero. Those are the *same observation counted twice*, not two
independent witnesses, and neither is a lost race. What they establish is that the scan walks past
an occupied slot and that the counter tracks it, with SOLO's zero delta as the negative control.

Read the highest index the other way and phase 1 is positive evidence **against** a lost race:
had the two clients ever raced for slot 1, the loser would have walked on to slot 2. None did —
measured, A always slot 1 and B always slot 0, a stable disjoint assignment, which is consistent
with service being serialised.

**Phase 2 carries the gate, alone.** A holds slots 1..61 CLAIMED and the flag slots are taken, so
slot 0 is the only slot a request can be given, and A holds no slot between its own requests. An
`ENOBUFS` handed to A therefore has exactly one possible cause: **the other address space was
occupying slot 0 at that instant.** Both clients being served *and* refused across the same run is
the two of them genuinely alternating on one slot word. If B is not running, A is never refused
and the gate **fails** rather than passing quietly.

**So what is proven is that two address spaces share the pool and interleave correctly on one slot
under saturation.** What is not — and what no test on this stand can isolate — is that two CPUs
executed the compare on the same word at the same instant and the hardware arbitrated. That half
stays construction.

**No slot held by two clients** needs no counter. An unauthorised client cannot store into CSA,
but every request carries a 64-byte `NSFRQE` image *through* the slot and back, and the STC
writes only the six result fields into it. Each client stamps a per-request identity in fields
it owns (`reqid`, `sockdesc`) and asserts the image that comes back is its own; two clients
sharing a slot would stage into the same `rqe` and one would read the other's. No race in the
check — the copy-back happens while the client still owns the slot.

**Still not proven, and stated so it is not read as proven:** that two CPUs executed the compare
on the same word at the same instant and the hardware arbitrated. Witness 1 cannot separate that
case, by construction.

### 3. A property of the claim scan, found by being starved by it

The first run of the gate had A back off 10 ms after each refusal. It was served **0 times out
of 150** while B, asking continuously, held slot 0 throughout. That is not a defect: **the scan
is not a queue and makes no fairness promise** — both clients start at slot 0, so the client
that asks more often gets it. It is worth recording because a future step that wants fairness
will not get it from this structure, and because a test that backs off can starve itself into
looking like a broken pool. Phase 2 now spends a bounded number of *attempts* with no backoff
and asserts **both** directions: refused at least once (witness 3) and served at least once (no
outright starvation).

### 4. The retain branch: why a blocking operation cannot induce it

`nsfsx_stop` clears `ANCHOR_ACTIVE` **before** it nudges parked clients, so a nudged client wakes
inside the SVC routine, finds the anchor quiescing, takes the routine's `WQUIES` path — slot to
FREE, in-flight decremented, `RCCORR` to the caller — and **drains itself**. A blocking `accept`
or `RECV` therefore cleans up on the nudge and lands in the *drained* branch, which is what b3's
two induction attempts kept discovering. That is correct behaviour, and it is why the retain
branch is hard to reach at all.

What reaches it is a slot left **CLAIMED** with the in-flight count already taken and **nobody
waiting on its reply ECB**: no waiter for the nudge to wake, and `nsfreqx_reap_ok` excludes
CLAIMED, so the death guard never reclaims it either. A client that faults inside the routine's
write-IN move leaves exactly that, because the claim and the in-flight increment both happen
before the move. The induction is that fault with the cleanup deliberately withheld
(`TSTRQXC PARM='LEAK'`).

**The `nsfsx_router_unload` fix is proven FORWARD, not by revert, and that is weaker.** The house
habit is to prove a fix by reverting it and watching the gate go red. Reverting this one means
freeing the CSA a client is parked inside, in supervisor state, key 0 — the failure mode is the
system, not the test. The evidence is therefore: the retain decision is reached (`NSF054W`), the
unload is not called, and the anchor is still readable afterwards **with its eyecatcher intact**
— `nsfsx_anchor_free` zeroes that eyecatcher before the `freemain` precisely so a parked client
cannot accept reused storage, so an intact eye after a stop says the free path was not taken.
That is not the same as demonstrating the bad outcome absent, and it should not be written up as
if it were.

### 5. The WAIT-gate probe: what "scan all slots" would actually have done

b3 recorded that `nsfsx_pending()` returns early on `g_busy` and never looks at another slot, and
proposed to make it scan all of them. Taken literally that is **a hot spin, not a latency fix**:
`evt_mainloop` skips its WAIT whenever a probe answers non-zero, and `nsfsx_drain`'s dispatch arm
needs the single private `NSFRQE`, so a probe reporting a dispatchable second request produces a
pass that does nothing and repeats. The rule was already written down next door —
`nsfdev_work_pending` "mirrors service's consume conditions".

The split is by **what the outcome needs**. `REAP` / `HOLD` / `REAP_BAD` all finish inside the
CSA slot and never POST, so they are consumed whether or not a request is in service, one per
pass. `DISPATCH` needs the private `NSFRQE` and stays invisible until it frees. `nsfreqx_actionable`
is the one encoding of that question, asked by both the drain and the probe, so they cannot
drift apart — the drift *is* the spin. Host-pinned in TSTREQX including the anti-spin row.

What this fixes was not hypothetical: a second client that published a request and then **died**
sat un-reaped for as long as an unrelated client's blocking operation ran. What it does not fix
is a dispatchable second request served sooner — that needs **concurrent service**, still open.

Writing it exposed one real bug: **the slot in service is still PENDING**, so a selector that
scans while busy would classify it, and a client that died with its request parked would have
been reaped from under the executive. Reaping clears `stage`, which is exactly what the parked
request's `ubuf` points at. The selector skips it explicitly; step 1 remains the one place that
finishes the in-service slot.

### 6. Found while running the gate, NOT fixed here: the executive can sleep through a request

Measured on MVSCE (issue #64). Client B published a request at ~15:13 and was serviced at
**15:24:04** — eleven minutes later. A `F NSFS,STATS` issued at 15:20:52 was answered **in the
same second**, which is what identifies the sleeper: the STC, not the client, because the
operator drain runs unconditionally on every pass of `evt_mainloop`. What woke it was unrelated
inbound device traffic (a `ping` from the host); one wake then processed both. The anchor read
live through `/.dm` during the stall showed the shape exactly: slot 0 PENDING, `inflight = 1`,
`served = 0x18D`, and `served = 0x18E` afterwards.

**It is conditional, and idleness alone is not the trigger.** The second run of the two-client
gate crawled at roughly three requests per minute until a continuous `ping` was started as an
external wake floor, after which it completed normally. But a freshly started STC served eight
sequential requests in **0.39 s** with no ping running — and still in **0.44 s** after five
minutes idle and **0.24 s** after fifteen. What distinguished the slow instance is that it had
previously run a TCP workload (`TSTRQXM`) and a long two-client run. So something a prior
workload leaves behind decides whether the transport's wake reaches a sleeping executive; whoever
picks this up should start from that difference rather than from the theory.

This is **pre-existing M5-2a behaviour, not something b4 introduced** — the new probe is strictly
more permissive than b3's, returning 1 in every state where the old one did, so it cannot lose a
wake the old one caught. Two concrete asymmetries against the Phase-1 original are worth
recording as starting points, not as a diagnosis: `nsfsx_drain` never resets its wake ECB, where
`nsfreq_drain` resets `g_reqecb` **before** taking the queue and double-checks after (ADR-0022,
the #27 class); and the `nsftmr_plat_arm(1u)` heartbeat does not survive an empty timer queue
(ADR-0034: queue empty ⟺ STIMER disarmed), so there is no periodic floor under the latency.
Filed, not fixed: diagnosing a wake path is its own investigation, and a guess in one is exactly
what this project punishes.

### 7. The live round

MVSCE, deployed at the 56-byte header (`NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144)`), the
client unauthorised throughout.

| Gate | Result |
|---|---|
| Stage-0 `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH` | **444 PASS, CC 0 batch + TSO** (`TSTMVCD` excluded, #53) |
| `TSTRQXM` | **batch CC 0, 32/32**, host peer verified **9353 bytes byte-exact** (TSO leg fails by design — one-shot listener) |
| `TSTRQXF` | **53/53 CC 0 batch + TSO** |
| `TSTRQXC` SOLO (the negative control) | **8/8 CC 0 batch + TSO**, `collisions 0 → 0` on a fresh anchor |
| `TSTRQXC` A (leader) | **CC 0000, 13/13** |
| `TSTRQXC` B (follower) | **CC 0000, 8/8** |
| `TSTRQXC` LEAK (induction) | **CC 0000, 8/8** — slot CLAIMED, `inflight` 0 → 1, S0C4 caught, no dump |
| `TSTRQXC` V (after the stop) | **CC 0000, 5/5** |
| Host suite | **2925 PASS / 0 FAIL** (TSTREQX 119 → 137) |

The two-client numbers, with §2's division applied to them. These are from the **re-run on the
corrected build**, on a **fresh anchor whose counters start at zero** — which removes the last
ambiguity from the deltas:

- **Mechanism (phase 1).** 150 requests, all 150 on slot 1, `collisions` **0 → 150** — one failed
  compare per request, which is what the counter must read with the other client on slot 0. B's
  mirror image confirms it: B's index was **never** anything but 0. A stable disjoint assignment,
  so no lost race here, and the highest index of 1 says so positively.
- **The gate (phase 2).** 3000 attempts with one slot free: **served 239 / refused 2761 /
  wrong 0**. B, over the same window, was **served 194 and refused 154** — so *both* clients were
  refused by the other and *both* won the slot. That is the interleaving, measured from both ends.
- `exhausted` **0 → 2915**, and 2761 + 154 = **2915 exactly** — the counter accounts for every
  refusal the two clients saw and nothing else contributed to it.
- Pool 64/64 FREE at exit, every reply carrying its own identity.

The SOLO negative control ran on the same STC instance minutes before, reading `collisions 0 → 0`.
The probe STC's own stats line independently reported **`COLL=0`** across the 126 sequential
requests of the Stage-0 four — a second negative control, from different code.

(The first run of the gate, on the previous STC instance, gave the same verdict from a non-zero
baseline: `9888 → 10038` against a SOLO `9888 → 9888`, phase 2 served 1375 / refused 1625.)

The retain branch: `NSF043I SVC 239 RESTORED` at 15:43:52, **`NSF054W 1 CLIENT(S) STILL IN
FLIGHT -- CSA AND SVC ROUTINE RETAINED (EXHAUSTED=2)`** at 15:44:02 — the 10 s drain ceiling
elapsed. The anchor read back afterwards still carried **`NSFVANCR`**, `ACTIVE` clear and
`inflight = 1`. And the restart is its own witness: the next `S NSFS` came up on a **different
anchor (00AAD7C8, was 00A8B7C8) and a different router EP (00A8B248, was 00A820C8)** — the second
of those is the exact evidence that the module was retained, since a freed one would have been
reusable. The largest free CSA block also fell 1073152 → 933888, a drop **consistent with** the
retained pool plus module; it is not more than that, because `nsfsx_csa_largest` refines only to
4 KB, so both figures carry ±4 KB and the arithmetic cannot resolve 138672 from 139264.

**The second run of the gate needed an external wake floor.** It crawled at roughly three
requests per minute until a continuous `ping -i 0.2` was started at the host, and completed
normally with it running (see §6 — the same latency defect). That does not touch what the gate
measures, which is slot *occupancy* and not timing, but anyone reproducing this without the ping
will see the crawl and reasonably conclude the pool is broken. The first run, and every other
gate in the round, needed nothing.

### 8. What M5-2b4 still does not prove

- **Concurrent service.** One dispatch at a time (Decision 10) is unchanged.
- **Hardware arbitration of a simultaneous `CS`.** See §1 and §2 — and note that phase 1 is
  positive evidence that no lost race occurred *in that phase*, so on this stand it is not merely
  unmeasured, it is unobserved.
- **Reaping a second client's slot WHILE a request is in service.** This is the hole §5 exists to
  close, and the round did not exercise it: no `NSF050I` / `NSF051W` was issued by NSFS at any
  point, because no client died or became unclassifiable while another was being served. The
  decision is host-pinned (`nsfreqx_actionable`, TSTREQX) and the selector is wired into both the
  drain and the probe — but the *live* path is unexercised, and saying otherwise would be the
  same "absence is indistinguishable from success" the checklist is about. b3's live reap
  evidence (two `NSF050I`, one `NSF051W`) was collected with **nothing else in service**, which
  is the case that already worked.
- **That the nudge loop reaches MORE THAN ONE parked client.** b3 replaced the probe STC's
  single-slot wake with a loop over every claimed slot precisely because 63 clients can be parked
  at once. The induction leaves exactly **one** claimed slot, so the loop iterated 64 and found
  one — no better exercised than before. Same shape as the row above.
- **Two-address-space stress.** Still **(e)**.
- **Fault recovery, address validation, the CLAIMED-slot leak.** Unchanged and still open.
- **The transport's wake latency** (§6, issue #64).

### 9. Two things that must happen BEFORE (e), not after

**Issue #64 is a prerequisite, not housekeeping.** An executive that does not wake without device
traffic — one request unserved for eleven minutes while a `F NSFS,STATS` queued behind it was
answered in the same second, and a gate run that needed a host `ping` as a wake floor — makes any
*throughput* or *latency* number from a stress round meaningless while it goes unrecognised. (e)
measures exactly those. So #64 comes first.

**The induction's leak invalidates free-CSA readings on this stand.** ~137 KB plus the old router
module are retained until IPL — the intended price — and the consequence is that b3's
`LARGEST FREE BLOCK NOW 905216` no longer describes this system: this round measured 1073152
before the induction and 933888 after. **IPL before (e)**, or the stress round sizes itself against
an artificially small pool and reports a CSA ceiling that is an artefact of this round.

### 10. One acceptance item was superseded, not met

The step's acceptance list asked that "B's request must become visible to the WAIT gate **while**
A is still being served". Under serialised service that is exactly the spin §5 describes, so the
decision taken was the narrower one: **the outcomes that need no private `NSFRQE` become visible
and consumable while busy; a dispatchable one deliberately does not.** The item as written is
therefore *superseded*, and what replaced it is the reap/hold/reap-bad path — which is
host-pinned and wired but, as §8 records, went live-unexercised. Reading the acceptance list
against a green round without this paragraph would mark the item met.
