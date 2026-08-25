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
  A slot whose owner faults mid-request leaves the slot CLAIMED; the death guard reclaims it
  only if the owner's whole address space ended.
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
