# d2's prior question — may the anchor be freed while the routine is retained?

**Date:** 2026-09-15 · **Proof kind: SOURCE.** No code change, no test change, no
deploy, no MVS run, no Hercules operation. Read from `asm/nsfvsvc.asm`,
`src/nsfsx.c` and `include/nsfvsvc.h` at `main` `2cfd406`.

**This is not the d2 round.** It is the question that decides whether d2 has a
design space, filed as the decision's **input**. It is deliberately **not** an
ADR-0042 annotation: an ADR that annotates an open question reads later as a
choice already made, and no choice has been made here.

---

## Verdict — NO. Freeing the anchor while the routine is retained is unsafe.

**The reason that decides it: the reply ECB the parked task is waiting on lives
INSIDE the anchor.**

`include/nsfvsvc.h:530` puts `NSFV_SLOT slots[NSFV_NSLOTS]` at offset **56** of
one contiguous 137272-byte allocation (`NSFV_OFF_ASSERT(NSFV_ANCHOR, slots,
56)`), and `reply_ecb` at slot offset **8** (`NSFV_OFF_ASSERT(NSFV_SLOT,
reply_ecb, 8)`). The task parked at `asm/nsfvsvc.asm:843` is waiting on

```
anchor + 56 + n*2144 + 8
```

So freeing the anchor returns to the CSA pool the storage an outstanding `WAIT`
depends on.

**The two-flag separation — "reap the slot, free the anchor, retain the
routine" — therefore does not exist**, because *the slot IS the anchor's
storage*. d2's space is **A** (leave the retention), **C** (make UNKNOWN rarer)
or **D** (make the retained storage recoverable at a later start). Option **B**
is dead.

### The reason that does NOT decide it, which is the one you meet first

The resume path **does** touch the anchor — one unconditional fetch, before any
branch:

```
:843    WAIT  1,ECB=(R3)
:844    CLC   ANCEYE(8,R2),=CL8'NSFVANCR'   anchor there?
:845    BNE   WGONE              freed while parked -> bail
```

**That touch is designed to survive a freed anchor, and both halves of the
mechanism are in the tree.** `src/nsfsx.c`'s `nsfsx_anchor_free` zeroes the
eyecatcher **before** `freemain`, and its comment names this exact scenario:

> *Invalidate the eyecatcher BEFORE releasing: freed SP=241 storage is reused,
> not zeroed, so a client still parked in the routine must not accept the reused
> anchor as valid (its wake path revalidates the eye).*

And the failure path touches **nothing** of ours:

```
:1302   WGONE    DS    0H         anchor freed: inflight is gone
:1303            LA    R15,RCCORR
:1304            ST    R15,REQRC(,R8)      <- the CALLER's block, not the anchor
:1305            BR    R14
```

**Both halves belong in this record**, because anyone re-deriving this hits the
register question first — as this round did. The registers are the right first
question and they produce the **wrong candidate answer**: `asm/nsfvsvc.asm:748-757`
states R2 (anchor) and R7 (slot) are restored from the SVRB save area after the
POST and that *"the later WAIT (SVC 1) preserves R2-R14"*, so the anchor address
is **carried in a register, not re-read** — and the first instruction after the
WAIT dereferences it anyway. "Works from saved state" does not save it; and it
does not condemn it either, because of the revalidation above. The register
analysis settles neither way, and the answer is one bullet further down.

---

## The enumeration

`:843` forward to every `BR R14`, across all four branches.

| line | reference | kind |
|---|---|---|
| **844** | `CLC ANCEYE(8,R2),=CL8'NSFVANCR'` | anchor header — **fetch, unconditional, before any branch** |
| 845 | `BNE WGONE` | — |
| **1302-1305** | `WGONE`: `LA R15,RCCORR` / `ST R15,REQRC(,R8)` / `BR R14` | **no anchor access at all** |
| 846 | `L R3,SLSTATE(,R7)` | slot, fetch |
| 848 | `BNE WQUIES` | — |
| 1280 | `WQUIES`: `ST R3,SLSTATE(,R7)` | slot, **store** |
| 1281, 1284 | `L R3,ANCINFL(,R2)` / `CS R3,R4,ANCINFL(R2)` | anchor header, **read-modify-write** |
| 860 | `L R3,SLTOKEN(,R7)` — ECHO | slot, fetch |
| 931, 942 | `L R10,SLXLEN(,R7)` / `LA R5,SLSTAGE(,R7)` — XFEROUT | slot, fetch |
| 1047, 1058, 1069 | `SLXLEN` / `SLSTAGE` / `SLRQE` off R7 — RQEOUT | slot, fetch |
| 950 | `REPLYC`: `L R3,ANCSERVD(,R2)` | anchor header, fetch |
| 964 | `ST R3,SLSTATE(,R7)` | slot, **store** |
| 965, 968 | `L R3,ANCINFL(,R2)` / `CS R3,R4,ANCINFL(R2)` | anchor header, **read-modify-write** |

`MOVEOUT` carries no R2/R7 reference of its own, but it **reads through
`R5 = SLSTAGE(,R7)`** — anchor storage — set up by its callers at `:942` and
`:1058`.

## The three hazard cases, graded

1. **Nobody can legitimately post it afterwards.** The STC is gone and the
   address is freed, so the task never resumes; it stays in `WAIT` until its
   address space terminates.
2. **A reallocation written with the post bit set wakes the task spuriously.**
   Handled cleanly: the eyecatcher was zeroed before the free, so the `CLC` at
   `:844` fails → `WGONE` → return, touching nothing of ours.
3. **A false eyecatcher match** takes the matched path, which **stores** into
   freed CSA (`:964`, `:968`, `:1280`, `:1284`). It needs a third party to write
   `NSFVANCR` at that exact offset, so it is **effectively impossible — which is
   not the same as excluded**, and what it produces is a store, not a fetch.

## Same class as unloading the routine, one object over

`nsfsx_stop`'s existing reasoning — that retaining the anchor while freeing the
code is *strictly worse than leaking both*, because a client that failed to
drain is parked in a `WAIT` **inside that code**, supervisor state, key 0 —
**transfers to the ECB without modification**. Freeing storage a parked task's
`WAIT` depends on is the same hazard whether the storage holds the instructions
or the ECB.

## Scope of the search

**Searched:** `asm/nsfvsvc.asm` (the code the parked task resumes on — `:843`
forward to every `BR R14` on all four branches), `src/nsfsx.c`
(`nsfsx_anchor_free`, `nsfsx_stop`'s ordering, `nsfsx_wake_parked`),
`include/nsfvsvc.h` (layout and offset asserts).

**NOT followed:** the `WAIT` macro's own expansion, and MVS's internal
WAIT/POST bookkeeping. **"MVS holds the ECB address in its own structures" is
standard-behaviour reasoning, not traced in this tree** — the sentence is kept
in that form deliberately, because it is the difference between this record and
one that would later be quoted back as if it had been measured.

---

## A finding that is larger than a side note

**`nsfsx_wake_parked` skips everything that is not LIVE:**

```c
if (nsfsx_client_state(slot) != NSFREQX_CL_LIVE) continue;
__xmpost(slot->req_ascb, &slot->reply_ecb, 0);
```

An UNKNOWN client is therefore **never posted**. So force-reaping the slot lets
the **drain** complete and still never wakes the client: **the storage decision
and the wake decision are independent**, and a client that is UNKNOWN, alive and
parked is rescued by **none** of the options — not by leaving the retention, not
by making UNKNOWN rarer, not by reclaiming later.

Two consequences, recorded and not developed further:

- **"UNKNOWN at shutdown" is not a shutdown problem.** It is the
  shutdown-visible face of a permanent property, and the 134 KB is the
  **symptom** rather than the thing.
- **It narrows option D before D is discussed.** A later start that frees a
  retained anchor frees the same ECB, so **adoption is the only safe form of D**
  — and `WGONE`, `WQUIES` and `PSTFAIL` all return **`RCCORR`** (`:1303`,
  `:1286`, `:1298`), so an adopted client cannot distinguish *"quiesced"* from
  *"the anchor was freed under me"* from *"the POST failed"*.

The options are Mike's to weigh in the memo. Nothing is proposed here.

## What this does NOT do

- **No fix, and no option named as preferred.** This step ends with a finding.
- **Does not start the d2 round.** `src/`, `asm/` and `include/` are untouched.
- **No live run.** The verdict is a source result and stays one; nothing here is
  claimed as measured.
- **Nothing flips to proven; no milestone moves.** d2 remains M5-2's one open
  named-unproven property.

---

## ADDENDUM at countersign — the closing question is answered, and one prescription of mine did not discriminate

**Appended, nothing above rewritten.**

### The closing question was decided the same day

The section above ends by asking *"whether d2 changes anything at all, or whether
leaving the retention is the right answer"*. That was the right question when it
was written and it is **no longer open**: Mike has ruled **A a no-go** and
**locked option D** — adopt a retained anchor at the next start.

The reason is `40-chk`'s own measurement, not a preference: one parked batch
client that dies costs **139264 bytes, pool plus router**, and again **on every
recycle**, with **no operator recovery short of an IPL**.

This line exists so a later reader does not find an open question that was
closed the same day it was filed.

**Release is not a safe form of D — adoption is.** That follows directly from
the verdict above: the reply ECB is inside the anchor, so a later start that
*frees* a retained anchor frees the same ECB and reproduces exactly the hazard
this round establishes. Whatever D turns out to be, it adopts.

### A correction that is mine, recorded where the reasoning is

**The register distinction was prescribed as decisive, and it does not
discriminate.** The question this round answered framed it as:

> *a resume path that re-reads is unsafe against a freed anchor; one that works
> from saved state may not be.*

Neither branch settles anything. R2 is **carried** across the WAIT — saved state,
by that framing — and the routine **dereferences it anyway** at `:844`. So
"re-reads" and "works from saved state" lead to the same place, and the test
separates nothing. It is why the deciding reason turned out to be the third
bullet (where the slot lives) rather than the headline.

**The general shape, which is the part worth keeping:** *a discriminator was
prescribed without checking that it discriminates.* This is the same class as
`#101` Stage 2's finding that a falsification clause is itself a claim needing
the same check as the prediction it guards — one step earlier, at the point
where the question is framed rather than where its answer is judged.
