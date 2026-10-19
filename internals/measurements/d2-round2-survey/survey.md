# d2 round 2, survey: what a start finds, and what it could do with it

**Date:** 2026-10-15. **Kind:** a survey, read from source. Nothing was built
and nothing was run; every claim below is a reading of the code at
`main` 6e47f7d, cited by file and line. **Decides nothing.** The questions in
§5 are Mike's.

ADR-0048 §7 left three questions to round 2:
1. What does a start do with a retained anchor it found?
2. Is the stale HELD slot quiesced?
3. Is a client that now classifies LIVE posted?

They cannot be answered without first knowing what a retained anchor looks
like when the next start finds it, so this survey starts there.

---

## 1. Where a retained anchor comes from

**Exactly two paths retain, and both leave the SSCT chained.**

| Path | Code | What it leaves |
|---|---|---|
| `P NSFS` whose drain did not reach `inflight == 0` within its ceiling | `src/nsfsx.c:717-749` | anchor **and** router module; SSCT **not** removed (the comment at :736 says why) |
| ESTAE recovery (#79) | `src/nsfsx.c` `nsfsx_recover_quiesce` | the retain posture **unconditionally**: no drain, no free, no unload; `nsfsct_remove` is not called either |

The second row matters as much as the first: **every abend of NSFS today
leaves a retained anchor** (139264 bytes with the router, `internals/measurements/40-chk`),
whether or not a client was in flight. So adoption is not only the
`NSF054W` case, it is also the recovery from #79's retained state.

## 2. What the next start finds

### 2.1 The anchor (`include/nsfvsvc.h`, `NSFV_ANCHOR`)

Both retaining paths do the same two stores before letting go (`src/nsfsx.c:711-712`
and in recovery):

- `flags &= ~ACTIVE`: the SVC routine refuses new requests at entry
  (`asm/nsfvsvc.asm:425`).
- `server_ecb_ptr = NULL`: the STC-private wake ECB is gone with the STC.

Everything else is as the dead STC left it:

| Field | State at the next start |
|---|---|
| `eye`, `version` | intact: `NSFVANCR`, 3. This is what `nsfsct_classify` checks first. |
| `server_ascb` | the **dead** STC's ASCB address |
| `inflight` | **≥ 1** on the drain-retain path, by definition; any value after an abend |
| `served`, `reaped`, `exhausted`, `collisions` | counters, meaningless across instances |
| `jobname_tail` | the registering instance's chars 4..7 (round 1) |
| `slots[64]` | every state is possible: FREE, CLAIMED, PENDING, DONE, HELD |

### 2.2 The router module

**Retained, still in CSA, and nobody knows where.** `nsfsx_router_load` keeps
its address only in the STC's statics `g_router_lpa`/`g_router_epa`
(`src/nsfsx.c:263-264`). Those die with the STC. The anchor does not record
it, and the SVC slot was restored to the invalid-SVC entry point
(`src/nsfsx.c:705`), so the table no longer points at it either.

The router still carries the anchor's address, patched at load
(`src/nsfsx.c:267`). A client parked in it resumes in that copy of the code.
**So a start can find the anchor but not the router**, and that decides what
adoption can recover:
- the anchor, about 137 KB;
- **not** the router (a few KB), unless its address is recorded somewhere.

### 2.3 The SSCT

`nsfs` is chained with `SSCTSUSE` = the anchor (round 1).

**Today's behaviour, with round 1 merged and round 2 not yet built, worth
knowing:**
- **Start N+1** finds `nsfs` chained. `nsfsct_register` refuses
  (`src/nsfsct.c`, "REFUSE RATHER THAN CHAIN A SECOND ONE"), and NSFS says
  `NSF059W` and runs on a **new** anchor B.
- **A clean stop of N+1** calls `nsfsct_remove()` (`src/nsfsx.c:754`), which
  removes **by name**. That removes the SSCT that points at the **old** anchor
  A, not at B.

So the retained anchor becomes unnameable again: the status quo before d2,
not worse. It holds until round 2 gives start N+1 something better to do than
refuse.

## 3. Who can still be using a retained anchor

This is what decides whether anything in it may be touched.

**The SVC routine's resume path** (`asm/nsfvsvc.asm:842-847`):

```
WAIT  1,ECB=(R3)                    the slot's reply_ecb, in the anchor
CLC   ANCEYE(8,R2),=CL8'NSFVANCR'   anchor still there?   no -> WGONE
L     R3,SLSTATE(,R7)
C     R3,=A(STDONE)                 serviced?             no -> WQUIES
```

`WQUIES` (:1277) sets the slot FREE, decrements `inflight` with a CS, and
returns `RCCORR` to the caller.

**So a parked client that is posted while its slot is not DONE bails out
cleanly.** It gives back its slot and its in-flight count, whatever `ACTIVE`
says. The resume path does not test `ACTIVE`.

Which clients can still be parked when the drain gave up:

| Slot state | Client verdict at the stop | Still parked? |
|---|---|---|
| PENDING | **LIVE** | No. `nsfsx_wake_parked` posted it (:665), it took `WQUIES`, and inflight came back. This row is not why a drain fails. |
| PENDING | **UNKNOWN** | **Possibly.** It is never posted (ADR-0040) and is the usual cause. |
| PENDING | **DEAD** | No: its address space has ended. Its inflight never comes back, because the drain does not reap (§4.2). |
| PENDING / DONE | **"LIVE" but dead** (a batch client in a reused initiator, 40-CHK, #88) | **No**, though the guard says LIVE: the post lands in a WAIT nobody is in. Its inflight never comes back. |
| DONE | any | The client was posted. If it was alive it took the reply and released the slot. A DONE slot that stays DONE is 40-CHK's permanent leak. |
| CLAIMED | any | A client that faulted in its write-in move (M5-2b3) and **nobody is waiting**. Its inflight never comes back. |

**The conclusion that matters:** the only row where a task may really be
waiting inside the anchor (and inside the retained router) is **UNKNOWN**.
Every other non-FREE slot is storage plus a leaked count. That holds at the
stop; at the next start the same slot may classify differently, which is
ADR-0048 §7's observation.

## 4. What the existing machinery already does with such slots

### 4.1 If a start simply adopted the anchor and changed nothing else

The executive's drain would run over the adopted slots as it does over its own:
- **PENDING with a DEAD client:** reaped (`nsfreqx_slot_action` → REAP,
  `nsfsx_reap` gives inflight back).
- **PENDING with a LIVE client:** **dispatched as a new request.** Its
  `sockdesc`/`apptok` belong to the dead stack, so it most likely gets
  `EBADF`. It is answered either way, and that is not wrong.
- **PENDING with an UNKNOWN client:** HELD.
- **DONE, HELD, CLAIMED:** nothing. `slot_action` acts on PENDING only. They
  stay leaked in the adopted anchor, and their counts stay in `inflight`.

The consequence: `inflight` never returns to 0. So **the next `P NSFS` retains
again**, every time. With adoption that costs no new anchor, but each start
loads a **new router** (`nsfsx_router_load` always does), so **every retain
leaks one router copy.**

### 4.2 What a deliberate scrub at adoption could reuse

`nsfreqx_reap_ok` (`include/nsfreqx.h:341ff`) already permits reclaiming
**PENDING, HELD and DONE** when the client is **DEAD**, or when the storage is
untrusted. `nsfsx_reap` does the two-move CS reap. **No new rule is needed**
for those three states with a DEAD owner. What is missing is only a caller
that walks all slots once at adoption, instead of the drain's PENDING-only
`slot_action`.

The two states the existing rules do **not** cover:
- **CLAIMED.** Excluded by `reap_ok`. The b3 record says why it needs a fourth
  state (`CLAIMED → REAPING`), and that it belongs with fault recovery.
- **Any state with a false-LIVE owner** (40-CHK). No sound answer exists
  (#88).

## 5. The questions, with the options and what each costs

### Q1. What does a start do with a found anchor?

The verdict comes from `nsfsct_find`: NONE / OK / BADEYE / BADVER / FOREIGN.

| Verdict | Options |
|---|---|
| **NONE** | allocate fresh, as today |
| **OK** | **(a) adopt:** reuse the anchor, re-stamp `server_ascb`, `server_ecb_ptr`, `ACTIVE`, and keep the SSCT. **(b) Free and allocate fresh:** forbidden by ADR-0048 §3 while any slot may have a waiter, and §3 shows which do (UNKNOWN). |
| **BADVER** | layout not understood: **remove the SSCT by name**, leak the anchor as today (`NSFxxxW`), allocate fresh. ADR-0048 §3.2 already says "removable but not adoptable". |
| **BADEYE** | `SSCTSUSE` null or not ours: remove by name, allocate fresh |
| **FOREIGN** | a CL4 collision with another instance, whose tail differs: **touch nothing**, allocate fresh, and the rendezvous stays unavailable to this instance (`NSF059W`). |

### Q2. The router: reuse, reload, or record?

| Option | Cost | Gain |
|---|---|---|
| **R1. Reload, leak the old copy** | nothing to build | a few KB leak per retain, instead of 139 KB today |
| **R2. Record the router address so a start can find it** | one new field. **Append it AFTER `slots[]`**: every existing offset stays, the SVC routine never reads it, `ANCVERNO` could stay 3. Only `NSF_SIZE_ASSERT` moves (137272 → 137280). | the old copy becomes findable, so it can be **reused** (no load at all) or freed once nothing is parked in it |
| **R3. Keep it in the SSCT** (`SSCTSSVT` is unused) | `SSCTSSVT` is the subsystem vector table pointer. An `IEFSSREQ` to `nsfs` would branch through it. Nobody issues one, but it is a system field. | none over R2 |

I would not take R3. R2 is the clean one, but it is a layout change in the
CSA contract, so it carries its own cross-check: the size assert, and a full
Stage-0 round because the anchor size changes.

### Q3. What happens to the stale slots at adoption?

| Option | What it does | Risk |
|---|---|---|
| **S0. Nothing** (§4.1) | the drain handles PENDING; everything else stays | every later stop retains again |
| **S1. One scrub pass**, using only `nsfreqx_reap_ok` + `nsfsx_reap` | reclaims PENDING / HELD / DONE whose owner is **DEAD**, and gives their inflight back | **one trap, see below.** Otherwise none beyond today's: the rule already pinned in TSTREQX, applied once at start instead of per pass |
| **S2. S1, plus posting LIVE owners** | LIVE owners of non-DONE slots get posted, and bail via `WQUIES` (§3) | §3: `WQUIES` is the routine's own, already-exercised quiesce path. But a false-LIVE (dead batch client) post is a post into nothing, as at today's stop. |
| **S3. S2, plus recomputing `inflight`** from the slots that remain non-FREE | `inflight` is honest again | **must not undercount**: a count lower than the number of tasks really inside the router lets a later stop free code a task is in. Recounting can only ever *lower* it, so it needs care. |

**The trap in S1, found while checking this survey against the code.**
`nsfreqx_reap_ok` reclaims for **two** reasons: a DEAD owner, **or untrusted
storage with a LIVE owner** (`src/nsfreqx.c:254-264`, the `REAP_BAD` row).
Storage trust includes the published wake-ECB pointer
(`nsfreqx_slot_action`'s `ptr_ok`). In an adopted anchor that pointer is
**NULL**, because the retaining stop cleared it (§2.1). A scrub that computed
`storage_ok` the usual way **before** re-stamping `server_ecb_ptr` would judge
every slot untrusted and **reclaim LIVE owners' slots**: storage freed under a
live client, the one failure the safe-side asymmetry exists to prevent. So S1
must either:
- run **after** the re-stamp, or
- pass `storage_ok` from the slot's guard alone, and that choice must be
  pinned by a test.

**CLAIMED stays out of S1–S3**, and so do UNKNOWN owners. Neither has a sound
answer today: CLAIMED needs the fourth state, UNKNOWN is #88.

### Q4. Is a client that now classifies LIVE posted? (ADR-0048 §7)

That is S2, and §3 is the reason it is plausible: posting a parked client
whose slot is not DONE makes it bail cleanly with `RCCORR`. The alternative is
to dispatch its PENDING request into the new stack, which §4.1 shows happens
by itself without S2. The two differ:
- **S2:** the client gets `RCCORR` and knows the stack restarted.
- **Dispatch:** the client gets an answer from a stack that does not know its
  socket, most likely `EBADF`.

## 6. What a round-2 gate would have to show

These are the claims each option would make, not predictions:
- **Adoption:** the anchor address before and after a retain/start cycle is
  the **same**, and `LARGEST FREE BLOCK` does not drop by 137 KB. Today it
  does: `NSF055I` after each retain.
- **S1:** a DEAD owner's slot, retained, is FREE after the next start, and
  `inflight` came down by one.
- **R2 (if chosen):** the router EP after the second start is the old one, or
  the old copy is freed and `LARGEST FREE BLOCK` recovers the rest.

The inductions available:
- `TSTRQXC PARM='LEAK'` (b4) gives a CLAIMED leak. It is outside S1, so it
  would test only "adopt" and not "scrub".
- A DEAD-owner PENDING slot needs a real dying STC client: the `TSTAPPDS` rig
  (40-IDENT), cancelled while parked.

Each retain still costs CSA until IPL **until adoption works**. After that,
the same induction is recoverable by the next start, which is itself the
gate.
