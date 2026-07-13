# ADR-0027 — Locally-originated writes: actively park the READ via IOHALT (SVC 33)

**Status:** Accepted (2026-07-13). Resolves the known limitation recorded in
**ADR-0025** ("a WRITE that originates while the READ is already armed still
queues behind the READ until the next inbound frame"). Grounded in the M3-0a
Stage-0 probe `test/mvs/tsthio.c` / `test/asm/tsthalt.asm` (three reproducible
live runs on the real 0500/0501 pair), which is the permanent pin for this seam.
**Relates to:** ADR-0025 (pair sequencing, the passive form of this decision),
ADR-0022/0023 (subtask model), §9.3 (CTCI driver), §9.2 (NETDEV counters).

## Context

ADR-0025's pair sequencing parks the READ *passively*: when an inbound frame
completes the READ, the driver holds it (`rhold`) and lets a queued WRITE issue
before re-arming. That covers the M2 receive→reply pattern completely (1000/1000,
unimodal RTT). It does nothing for **locally-originated** traffic — a SENDTO from
an MVS application, a future TCP SYN or retransmit — where a WRITE must go out
while the READ sits armed and *no* inbound frame is coming. Such a WRITE queues
at the IOS/shared-channel level until unrelated inbound traffic happens to
complete the READ. M3-3 (UDP SENDTO) is the first API that ships this path, so
the mechanism must land before it (ADR-0025's own condition).

ADR-0025 named two candidate mechanisms: halt the outstanding READ, or move to an
attention-driven read protocol.

## Decision

**Actively park the READ by halting it with IOHALT (SVC 33) when a
locally-originated WRITE is pending and the READ is armed with no inbound frame
in flight.** The read subtask's single-ECB wait then completes — with X'48'
(purged) or, if an inbound frame raced the halt, with X'7F' and data — and
*either* outcome lands in the existing parked-read path from ADR-0025: the WRITE
issues with the READ parked, and the established `returnecb` handshake re-arms
the READ afterwards. IOHALT is the *active trigger* for the same machinery pair
sequencing already exercises passively; the ownership and completion model of
ADR-0022/0023 is unchanged.

## Evidence (Stage 0, proven on MVS 3.8j problem state)

- **IOHALT (SVC 33):** R0 = UCB address, R1 = X'00000001', SVC 33 — read live
  from `SYS1.MACLIB(IOHALT)`. Fires from problem state with no authorization
  failure and no abend.
- **PURGE (SVC 16):** 16-byte parameter list, TCB-scope + HALT option,
  TCB address = 0 ("the caller's job-step TCB" — the only scope problem state
  may use), per OS/VS2 SPL: Data Management, GC26-3830-4. Returns rc = 0,
  parmlist byte 4 = X'7F' (success). Also works.
- **The guest-visible completion is X'48' ("purged"), not the device-level
  signature.** The Hercules source predicts CE+DE with full residual for a
  halted CTCI read; IOS reclassifies the purged request and posts the ECB with
  completion code X'48', and per GC26-3830-4 Figure 13 the residual count "does
  not apply" on that code. **The post code is the discriminator between a halted
  and a data completion — not the transferred length.** (Lesson recorded: the
  emulator source defines the channel's behaviour; the guest contract is
  IOS's.)
- **The subchannel is not wedged by a halt:** a fresh READ after either
  mechanism re-arms cleanly and receives real data.
- **UCB address from problem state:** DCB+44 (`DCBDEBAD`) → DEB+32
  (`DEBSUCBA`), offsets pinned from the OS/VS2 Debugging Handbook
  (GC28-0709/0710), with a UCBNAME sanity check against the device's own 3-digit
  CUU text before issuing an SVC against a computed address.
- **Race reality:** Hercules queues inbound frames regardless of wire activity,
  so a halt can race a data completion; the probe's drain loop exists because of
  exactly this. The driver must accept either outcome after requesting a halt.

## Why IOHALT and not PURGE

The Stage-0 probe ran single-task; the production driver is multi-task (the read
subtask owns the EXCP, the write path triggers the halt). The two services scope
differently, and that decides it:

- **IOHALT is UCB-scoped** (R0 = the device). Issued from any task in the
  address space, it halts I/O on precisely the read subchannel and nothing else.
  Cross-task safe, device-precise.
- **PURGE with TCB = 0 is job-step-scoped.** Issued from the executive it would
  quiesce in-flight I/O for the whole job step — potentially including a WRITE
  outstanding on the other subchannel. Too broad, and its cross-task semantics
  are exactly what the single-task probe did not exercise.

PURGE remains the documented, proven fallback should IOHALT show a problem in
integration.

## Why not attention-driven read

Hercules CTCI has **no Device Attention handler** — the DEVHND slot is NULL
(`ctc_ctci.c`, release 4.9.1). An attention-driven protocol would require
patching Hercules and distributing that patch to every user, breaking the
stock-TK4-/TK5/MVSCE premise of the ecosystem. Rejected for v1.

**Upgrade path, recorded honestly:** attention-driven would be the cleaner
model — it removes the permanently outstanding READ altogether, which would
eliminate the (benign, live-verified) idle-MIH events *and* unlock the
ping-pong/free-buffer throughput follow-on that ADR-0025 made conditional on
this same solution. If a CTCI variant with attention support ever exists,
revisit this decision.

## Consequences

- **Read completions now have three classes:** X'7F' with data (decode), X'48'
  purged-as-requested (park, do not decode, do not count as an error), anything
  else (a genuine error). `ctr_ierr` must count only the third class. This makes
  the pending ierr counter split mandatory rather than cosmetic: today's driver
  counts every non-X'7F' completion as an input error, so without the split every
  write-kick halt would increment `ierr`. The non-IPv4 codec drops (the standing
  `ierr` conflation from live observation) belong to the same split.
- An **unrequested** X'48' (e.g. an operator-initiated purge) is possible;
  count/trace it distinctly rather than silently treating it as ours.
- The **UCB address is chased and cached once at device init** (after OPEN),
  with the UCBNAME sanity check; refuse to start the device on a mismatch.
- The **halt→data race** is normal: after requesting a halt, the read subtask
  accepts either completion; both park the READ; the WRITE proceeds; `returnecb`
  re-arms.
- The un-armed window remains loss-free — Hercules buffers inbound frames and
  back-pressures (§9.3 flow control); this is the same argument that covers the
  decode window today.
- **Cost:** one extra SVC and one halted channel program per locally-originated
  WRITE on an inbound-idle link. Cheap under Hercules; batching several queued
  PBUFs into one block (the CTCISEG chain) remains the later optimisation if it
  ever matters.
- Idle-MIH behaviour is unchanged (verified benign and transparent to the
  counters in live observation).

## Sources

- `test/mvs/tsthio.c`, `test/asm/tsthalt.asm` — the permanent Stage-0 pin.
- `SYS1.MACLIB(IOHALT)` read on the live system; OS/VS2 SPL: Data Management,
  GC26-3830-4 (PURGE, Figure 13); OS/VS2 Debugging Handbook GC28-0709/0710
  (DCB/DEB/UCB offsets).
- Hercules SDL-Hercules-390 release 4.9.1: `ctc_ctci.c` (DEVHND, halt handler,
  frame queueing), `io.c`/`channel.c` (HIO → `dev->hnd->halt` dispatch).
