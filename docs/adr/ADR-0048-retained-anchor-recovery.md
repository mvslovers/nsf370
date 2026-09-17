# ADR-0048 — Recovering the CSA a parked client's failure strands

**Status:** Proposed (M5-2, d2) — flips to Accepted on merge, per
`docs/adr/README.md` convention 1
**Date:** 2026-09-17
**Decisions locked by:** Mike, 2026-09-04
**Builds on:** ADR-0039 (the CSA anchor), ADR-0040 (the client-death guard),
ADR-0042 (the slot pool), ADR-0043 (the wake contract)
**Reverses, deliberately and in one named respect:** `src/nsfv.c`'s recorded
preference for the SVC slot over the SSCT — see §5.3
**Evidence:** `docs/measurements/40-chk/`, `docs/measurements/d2-anchor-question/`,
`docs/measurements/d2-rendezvous/`

---

## 1. Context — what is stranded, and how much

A client parked in the SVC routine that dies without draining leaves `inflight`
non-zero with **nobody left to wake it**: `nsfsx_wake_parked` posts only slots
whose client classifies LIVE. The shutdown drain therefore runs its full ceiling
and `nsfsx_stop` takes the **retain** branch, keeping both the anchor and the
router module.

**`40-chk` measured the cost: 139264 bytes — pool plus router — and again on
every recycle, with no operator recovery short of an IPL.**

**It is per-recycle rather than once because nothing ever looks for what was
left behind.** `nsfsx_anchor_alloc` calls `getmain(sizeof(NSFV_ANCHOR), 241)`
unconditionally on every start; there is no search, and no way for a start to
discover that a previous instance retained an anchor. Each failed shutdown adds
a full anchor to CSA that nothing will ever reclaim.

Retention itself is **correct and is not in question here** — §3 is why. What is
wrong is that retention is permanent.

## 2. Decision

**NSF adopts a retained anchor at the next start, finding it through an SSCT
registered under a per-instance name.**

**Three parts, and none of them works alone:**

1. **The retained anchor** — unchanged from today. `nsfsx_stop` keeps retaining
   on a failed drain.
2. **A surviving named pointer to it** — an SSCT whose `SSCTSNAM` is this
   instance's name and whose `SSCTSUSE` holds the anchor address.
3. **A reclaim at the next start that ADOPTS rather than frees** — plus
   **removal by name** when there is nothing worth adopting.

**Part 3 is not optional, and `IEF612I` is why.** ADR-0036 records it:

> While the SSCT is registered, `S NSFP` fails `IEF612I`; an abend that skips
> cleanup leaves the SSCT registered **until IPL**.

So a stranded registration with no reclaim path does not merely leak — the next
start either **fails** or **chains a second SSCT**. **Taking the rendezvous
without the reclaim leaves NSF worse off than it is today**, which is the
opposite of this ADR's purpose.

## 3. Why release is not an option — the anchor must be adopted, never freed

**The reply ECB the parked client is waiting on is inside the anchor.**
`NSFV_SLOT slots[]` sits at offset **56** of a single 137272-byte allocation and
`reply_ecb` at slot offset **8**, so a parked task waits on
`anchor + 56 + n*2144 + 8`. Freeing the anchor returns to the CSA pool storage
**MVS holds an outstanding WAIT on**.

That is **the same class of hazard as unloading the routine, one object over**,
and `nsfsx_stop`'s existing reasoning — that retaining the anchor while freeing
the code is *strictly worse than leaking both*, because a client that failed to
drain is parked in a WAIT **inside that code** — **transfers to the ECB without
modification**.

**The reason that does NOT decide it, recorded beside the one that does.**
Anyone re-deriving this meets the register question first, and it settles
neither way. The resume path *does* touch the anchor unconditionally at
`asm/nsfvsvc.asm:844` — but that fetch is a **revalidation built for exactly
this case**: `nsfsx_anchor_free` zeroes the eyecatcher **before** `freemain`, and
the failure path `WGONE` touches nothing of ours. And R2 is **carried** across
the WAIT rather than re-read, yet is dereferenced anyway — so "re-reads" and
"works from saved state" lead to the same place. The decisive fact is **where
the slot lives**, not how the anchor address is held.

## 4. The rendezvous, and the alternatives that were refuted

```
CVT -> CVTJESCT -> JESCT.JESSSCT -> first SSCT
                -> walk SSCTSCTA, match SSCTSNAM
                -> read SSCTSUSE = the anchor
```

`SSCTSUSE` is `F  RESERVED FOR SUBSYSTEM USAGE` (`IEFJSCVT.asm`) — a word
architecturally sanctioned for exactly this, not a field being repurposed.

**NSF adds no new facility.** Three existing implementations of that same walk:
**ufsd** (`ssct_find(UFSD_SSNAME)` → `ssctsuse`), **`mvs38j-ip`**'s
`findsgd.c` — this project's own ancestor — and **libc370's binding**
(`clibssct.h`: `ssct_find`, `ssct_new`, `ssct_install`, `ssct_remove`,
`ssct_remove_by_name`).

| candidate | the property that rejects it |
|---|---|
| A fixed CSA cell | one well-known **place**, not a name → cannot distinguish two instances |
| Leaving the SVC slot stolen | same — a place, not a name |
| **ENQ** | **purged at termination** — see below |
| Name/token services | do not exist on 3.8j |
| `libc370` `clibgrt.h` | **per address space** — the C runtime's process anchor reached through the CRT, not a system-wide registry |
| `CVTUSER` | `CVT.asm:426` — `DC A(0)  A WORD AVAILABLE TO THE USER`: **one word, one slot** |

**ENQ deserves its refutation in full, because it was the best-looking
candidate and libc370 already wraps it** (`clibenq.h`, with `ENQ_SYSTEM` scope
and `RET=TEST`). From `IEAVENQ1.asm`'s own prologue, for entry point
**`IEAVENQ2`, the ENQ resource manager**:

> `PURPOSE = CLEAN UP AFTER AN ABEND OR AT END OF TASK TIME. AT EITHER NORMAL OR`
> `ABNORMAL TASK TERMINATION, ALL REQUESTS MADE BY THIS TASK WILL BE DEQUEUED.`
> `FOR MEMORY TERMINATION, ALL REQUESTS MADE BY THIS ADDRESS SPACE WILL BE`
> `DEQUEUED.`

**Self-cleaning is the right property for a lock and the fatal one for a
rendezvous that must outlive the STC that made it.** ENQ is disqualified **by
the property that made it attractive** — the name vanishes at precisely the
moment a later start needs it. Two further readings support it: 3.8j has **no
`GQSCAN` and no `ISGQ*`** (with `ENQ.asm` and `DEQ.asm` present in the same
directory as the control that makes this a finding rather than a failed search),
so there is no enumeration; and a name can *key* but not *carry*, so an address
encoded into an RNAME would be recoverable only by a party that could
enumerate — **circular**, not merely inconvenient.

## 5. ADR-0036 objects twice, and the two objections need different answers

This is the part a reviewer will press hardest, so it is separated rather than
answered in one move.

### 5.1 Authorization — falls to the change of role, completely

ADR-0038 records why 0036's transport was superseded:

> `IEFSSREQ` is an **authorized** branch-entry, so the calling task must be
> APF-authorized. NSF's goal — run existing EZASOKET applications **unchanged,
> relink-only** — includes **unauthorized** problem-state applications, which
> cannot call `IEFSSREQ` directly.

**That objection is about who calls what, and it does not apply here.** In this
ADR the SSCT is **not a transport**: nothing routes through the SSI, no
`IEFSSREQ` is issued, and no client touches it. It is read by the **starting
STC**, which is authorized (`ac=1`, `clib_apf_setup`, `__super`), by walking a
chain and loading a word. The transport stays the private SVC of ADR-0038,
unchanged.

### 5.2 Lifecycle — does NOT fall to the change of role

> an abend that skips cleanup leaves the SSCT registered **until IPL**

**This is a property of a registered SSCT and holds whether or not `IEFSSREQ` is
involved.** The role change does not touch it, and this ADR does not pretend
otherwise.

**It falls to a mechanism instead.** `libc370/include/clibssct.h:47`:

```c
/* ssct_remove_by_name() remove SSCT as subsystem, requires supervisor state and key 0 */
int ssct_remove_by_name(const char *name)                           asm("@@SSREMN");
```

Its implementation is `ssct_find(name)` → `ssct_remove(ssct)` — **it touches
only the SSCT and never `ssctsuse`**. So a later start removes a stranded
registration **by name alone**, and that closes the ugly case as well as the
ordinary one: **a corrupt anchor, or one pointing at freed storage, does not
prevent removal, because removal never dereferences it.**

**ADR-0036's "until IPL" was written for a design in which only the owning STC's
ESTAE could clean up.** A design in which the **next start** cleans up by name
has a recovery path that one did not have. That is a different design, not a
different reading.

### 5.3 `nsfv.c`'s preference is reversed here, deliberately

Quoted verbatim, because reversing a recorded decision silently is worse than
reversing it:

> The STOLEN SVC SLOT IS RESTORED at stop AND on abend (a dangling stolen slot
> corrupts that SVC number system-wide -- stricter than Stage-0a's SSCT leak).
> **Restore merely redirects the slot, so the ESTAE may restore under RTM (unlike
> Stage-0a's SSCT)**, which also makes a fresh S NSFV after an abend restartable.

That reasoning preferred the SVC slot **because it does not survive**. For this
decision, **survival of the pointer is the requirement, not the flaw** — a
rendezvous that cleans itself up cannot be found by the start that needs it,
which is exactly what disqualified ENQ in §4. The preference is not overturned
as wrong; it is **inverted by a change in what the mechanism is for**.

### 5.4 The trade, stated as a trade

| | recovery of the rendezvous | cost left stranded |
|---|---|---|
| **today** | the SVC slot is restorable under RTM — **unconditionally** | **139264 bytes per recycle**, unrecoverable short of an IPL |
| **under this decision** | one small SSCT block outlives the STC until the **next start removes or reuses it by name** | the **139264 becomes recoverable** |

**The residual, named because it is small:** if NSF is never started again, the
SSCT stays. That costs **one small CSA block — not 139 KB per cycle.**

## 6. Instance identity — derived from the jobname, not configured

**NSF has no instance identity today.** Not in `NSFPRM0` and not in the
configuration grammar: the 17 statement keywords `nsfcfg.c` accepts are all
interface, pool, trace and port settings, and none names the instance.

Two stacks need one anyway — **and for the SVC number as much as for the SSCT
name**, since an SVC slot is system-wide per number. **One identity from which
both follow, not two schemes.**

**It is derived from the jobname** (`__jobname()`, `libc370/include/clibtiot.h`),
**not configured**, and the failure mode is what decides it. A configured name
can **drift** from the one the retained SSCT was registered under: someone edits
the PROFILE, the next start looks under the new name, finds nothing, and the
139 KB is orphaned. **That is this ADR's own leak, reintroduced through a typo.**
A jobname-derived name cannot drift without the instance itself changing.

**The price, stated rather than smoothed:** `SSCTSNAM` is `CL4` and a jobname is
eight characters, so `NSFSTACK` and `NSFSTEST` both truncate to `NSFS`.
**Collision handling must be explicit, never a silent truncation.** The rule
itself is **round 1's and is not decided here** (§7).

## 7. What this ADR does not claim

- **It does not pay into #88.** `nsfsx_wake_parked` skips everything that is not
  LIVE, so an UNKNOWN client is never posted: **this decision recovers storage,
  not a client.** What it changes is *when the question is asked* — UNKNOWN is a
  **point-in-time verdict**, and at the next start the same slot may classify
  cleanly. That is an **observation, not a claim**, and it is the first place in
  the system where the question gets asked twice.
- **The adoption protocol is not designed here** — what a start does after it
  finds a retained anchor, whether the stale HELD slot is quiesced, whether a
  client that now classifies LIVE is posted. **Round 2.**
- **The collision rule is not decided here.** **Round 1.**
- **UFSD's precedent is on the shape, not the answer.** `ufsd#rcl.c` records
  `ACTIVE + no ASCB to check  UNKNOWN  force reclaims, startup refuses` — a
  standalone utility reclaims an UNKNOWN, an ordinary startup refuses. **The
  transferable part is splitting forced from ordinary**; the answer is not
  transferable, because **UFSD frees where NSF must adopt** (§3).

## 8. Consequences

**Two rounds, each with its own gate:**

- **Round 1 — rendezvous plus removal.** Register an SSCT under the
  jobname-derived name, find it, remove it by name, and the collision rule.
  Gated across **two names**, because one name never exercises the property the
  whole scheme rests on.
- **Round 2 — adoption.** What a start does with a retained anchor once it has
  found it.

**They are separate because one PR would put two different claims under one
countersign.** Round 1 claims *a later start can find and remove what a previous
one left*; round 2 claims *it can safely take it over*. A single gate returning
green would not say which of the two had been demonstrated.

**Follows from the decision, and is new surface:** NSF acquires a **system-wide
named registration point** and a **concept of instance identity** it did not
have, and `nsfsx_start` gains a path it does not have today. That is why
`docs/measurements/m5-2d0/survey.md` §4.4's sizing no longer describes d2 — see
its appended annotation.
