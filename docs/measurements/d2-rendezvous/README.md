# d2 / option D — how does a later start find a retained anchor, by name?

**Date:** 2026-09-16 · **Proof kind: SOURCE.** No code change, no test change, no
deploy, no MVS run, no Hercules operation. Read from this tree and from the
sibling repos named in §7.

**This is not the d2 round.** Option D is locked as d2's direction; this step
establishes whether D's **precondition** can be met at all. Like
`d2-anchor-question/`, it is a **preparation finding, not an ADR** — the ADR
belongs with the decision, and the decision is d2's.

---

## Verdict — a mechanism exists: the SSCT, read by name

**Recommendation: publish the anchor address in an SSCT's `SSCTSUSE`, chained
into JESCT under a subsystem name, and have a start find it by that name.**

**And the counterfactual, stated because it was a real possible outcome:** had
ENQ been the only candidate, the answer would have been **"no mechanism
found"**, which would have foreclosed D and sent d2 back to C or to a fourth
option nobody has written. Nothing was manufactured to avoid that.

---

## 1. ENQ is refuted — by the property that made it attractive

`libc370/include/clibenq.h` already wraps ENQ with `ENQ_SYSTEM` scope,
`ENQ_SHR`/`ENQ_EXC` and `ENQ_TEST`, so the binding exists and the lead was worth
following first. It dies on the decisive question.

### Q2 — termination behaviour. **ESTABLISHED, IBM primary source.**

`mvs38-ibmsrc/ibm/IEA/IEAVENQ1.asm`, the prologue for entry point **`IEAVENQ2`,
the ENQ resource manager** (lines 344-356):

> `PURPOSE = CLEAN UP AFTER AN ABEND OR AT END OF TASK TIME. AT EITHER NORMAL OR`
> `ABNORMAL TASK TERMINATION, ALL REQUESTS MADE BY THIS TASK WILL BE DEQUEUED.`
> `FOR MEMORY TERMINATION, ALL REQUESTS MADE BY THIS ADDRESS SPACE WILL BE`
> `DEQUEUED.`

and line 29:

> `THE ENQ RESOURCE MANAGER (MANUAL PURGE) PERFORMS CLEAN-UP FUNCTION FOR END OF`
> `TASK, ADDRESS SPACE ABEND, AND OUT OF CORE ABEND.`

**ENQ is disqualified by exactly the property that made it attractive.**
Self-cleaning is the right property for a **lock** and the fatal one for a
**rendezvous that must outlive the STC that made it**: the name vanishes at the
precise moment D needs it — when the previous instance's address space
terminates. It *is* strictly better than the SSCT for the reason the SSCT was
criticised, and that is precisely why it cannot do this job.

### Q3 — enumeration. **ESTABLISHED, with its own positive control.**

No `GQSCAN` and no `ISGQ*` macro exists in the 3.8j maclib — and **`ENQ.asm` and
`DEQ.asm` are present in the same directory**. That control is what makes this a
**finding rather than a failed search**: the grep can see what is there.

So 3.8j supports **testing a name you already know** (`RET=TEST`) and nothing
else.

### Q1 — can a QNAME/RNAME pair carry or key an address? **Moot twice over, and the second reason is a circularity.**

ENQ holds its names in QCB/QEL with **no user data field**, so a name can *key*
but not *carry*. An address encoded into the RNAME text would be recoverable
only by a party that could **enumerate** — and Q3 says none exists. **You would
have to already know the address in order to test for the name that contains
it.** That is circular, not merely inconvenient, and it kills the idea
independently of Q2.

## 2. The mechanism, from primary source

```
CVT -> CVTJESCT -> JESCT.JESSSCT -> first SSCT
                -> walk SSCTSCTA, match SSCTSNAM
                -> read SSCTSUSE = the anchor
```

| field | source | |
|---|---|---|
| `JESSSCT` | `macros/maclib/IEFJESCT.asm:35` | `ADDRESS OF THE FIRST` subsystem SSCVT; JESCT is `BASED(CVTJESCT)` |
| `SSCTSCTA` | `macros/maclib/IEFJSCVT.asm:13` | `PTR TO NEXT SSCVT OR ZERO` — a chain |
| `SSCTSNAM` | `IEFJSCVT.asm:14` | `CL4  SUBSYSTEM NAME` — **a name** |
| `SSCTSUSE` | `IEFJSCVT.asm:23` | `F  RESERVED FOR SUBSYSTEM USAGE` |

`SSCTSUSE` is **a word architecturally sanctioned for exactly this** — not a
field being repurposed.

### Three independent supports

1. **UFSD** — `ufsd#sct.c:42` `ssct_new(UFSD_SSNAME, ssvt, (void *)anchor)`;
   found again at `ufsd#rcl.c:163` `ssct_find(UFSD_SSNAME)` → `ssct->ssctsuse`,
   validated by eyecatcher.
2. **`mvs38j-ip`** — this project's own ancestor. `src/arch/s370/mvs/findsgd.c`
   walks `JESSSCT`, `CLC SSCTSNAM,SSNAME`, `ICM SSCTSCTA` for the next, and
   `L R9,SSCTSUSE-SSCT(,R8)` for its SGD. The function is literally named
   *find the SGD*.
3. **libc370 already has the binding** — `include/clibssct.h`: `ssct_find`,
   `ssct_new`, `ssct_install`, `ssct_remove`, `ssct_remove_by_name`, with
   `ssctsuse` at **offset `0x14`** of the struct and the header stating the walk
   itself (`ssct = cvt->cvtjesct->jesssct`).

**Three implementations of the same walk is why this is a proven pattern rather
than a proposal.** NSF would add no new facility; libc370 is already its
sysroot.

### The two-instance test — passed **in principle**, see §4

`SSCTSNAM` is a **name** and `SSCTSCTA` is a **chain**, so two instances can be
two names. That is the property that killed the fixed CSA cell and the stolen
slot, and it is a real advantage of this mechanism.

## 3. The until-IPL property, head-on

ADR-0036 records it and it must not be waved past:

> *While the SSCT is registered, `S NSFP` fails `IEF612I`; an abend that skips
> cleanup leaves the SSCT registered **until IPL**.*

**(a) For D it is the REQUIREMENT, not the flaw.** D's premise is a deliberately
retained anchor. The pointer to it must survive too, or no later start can find
it. A self-cleaning rendezvous is disqualified — which is exactly what happened
to ENQ in §1.

**(b) ADR-0036's objection was about a DIFFERENT ROLE.** There the SSCT was the
**transport** registration: `IEFSSREQ` requires an authorized caller and NSF's
**clients** are unauthorized, which is why ADR-0036 was superseded as a
transport. Here the SSCT is a **rendezvous read by the starting STC**, which
*is* authorized (`ac=1`, `clib_apf_setup`, `__super`) — and **nothing routes
through it**. The finder walks a chain and loads a word; **no `IEFSSREQ` is
involved**. The transport objection does not transfer to this role.

**(c) `nsfv.c`'s preference is neutralised by the change of role.** It preferred
the SVC slot because *"Restore merely redirects the slot, so the ESTAE may
restore under RTM (unlike Stage-0a's SSCT)"*. For D we do **not** want recovery
to remove the rendezvous.

**(d) The residue becomes bounded and one-time instead of per-recycle** — see
§4, which is where that stops being a hope.

## 4. The rendezvous and the reclaim are ONE feature

**This is not a refinement of the mechanism. It is part of it.**

Because a registered SSCT makes a start fail `IEF612I` (§3), a stranded SSCT
with **no reclaim path does not merely leak** — the next start either **fails**
or **chains a second one**. Taking the rendezvous without the reclaim leaves NSF
**worse off than it is today**, which is the opposite of d2's purpose.

UFSD states the ordering that fixes it, in its own words
(`ufsd/src/ufsd#rcl.c` header):

> *After an abend the ESTAE path deliberately frees nothing: the SSCT stays
> chained, the SSI router module and the pools stay in CSA. … **UFSD startup
> main() reclaims BEFORE registering**, so after /C a plain /S UFSD suffices:
> **no second SSCT chained, no CSA leak**.*

That ordering is what converts "registered until IPL" from a growing leak into a
**fixed registration**: allocated once under a fixed name, found and reused by
every later start. Magnitudes, for scale: NSF today strands **139264 bytes
per recycle** with no recovery short of an IPL (`40-chk`); an SSCT is one small
CSA block that stops growing.

**So D is a single design with three parts — the retained anchor, the surviving
pointer to it, and the reclaim that adopts it — and none of the three works
alone.**

## 5. UFSD has already decided the UNKNOWN case, and split it

Promoted out of "noted, not chased", because this is a precedent on **d2's core
question**, not only on the rendezvous. `ufsd/src/ufsd#rcl.c:21-25`:

```
**   ACTIVE clear                       both callers reclaim
**   ACTIVE + ASCB assigned    LIVE     both callers refuse
**   ACTIVE + ASCB not found   DEAD     both callers reclaim
**   ACTIVE + ASCB is our own  DEAD     both callers reclaim
**   ACTIVE + no ASCB to check UNKNOWN  force reclaims, startup refuses
```

UFSD splits **forced** from **ordinary**: a standalone emergency utility
reclaims an UNKNOWN; an ordinary startup refuses it. Its liveness gate is
`server_ascb` looked up in the ASVT — the same shape as ADR-0040's classifier,
which already cites `ufsd#53`.

**The caveat travels in the same breath: it does not transfer unexamined.**
UFSD **frees** where `d2-anchor-question/` established NSF must **adopt**,
because NSF's reply ECB lives inside the anchor. So UFSD's *answer* is a data
point; the **transferable part is its shape — splitting forced from ordinary**;
and which of the two NSF wants is **d2's decision, not this record's**.

Worth knowing beside it: UFSD's quiescing rule is NSF's own retain-branch
reasoning in UFSD's words — *"Freeing the router out from under a parked client
is an S0C4 in that client's address space"* — and UFSD's drain is explicitly
best-effort, warning and freeing anyway rather than stranding CSA to an IPL.

## 6. Candidates rejected, with the property that rejects each

| candidate | rejected by |
|---|---|
| A fixed CSA cell | one well-known **place**, not a name → fails the two-instance test |
| Leaving the SVC slot stolen | same — a place, not a name |
| **ENQ** | **every request dequeued at task termination, every request of the address space at memory termination** (`IEAVENQ1.asm`) — gone exactly when needed; and no enumeration on 3.8j |
| Name/token services | do not exist on 3.8j |
| `libc370` `clibgrt.h` | **per address space** — the C runtime's process anchor reached through the CRT, not a system-wide registry; `libc370/TODO.md` plans nothing in that direction |
| `CVTUSER` | `CVT.asm:426` — `DC A(0)  A WORD AVAILABLE TO THE USER`: **one word, one slot** → fails the two-instance test |

## 7. Scope of the search

**Searched.** `nsf370`: `src/nsfsx.c`, `asm/nsfvsvc.asm`, `include/nsfvsvc.h`,
`docs/adr/ADR-0036`. `libc370`: `include/clibenq.h`, `include/clibssct.h`,
`src/clib/@@ssfind.s`, `src/clib/@@ssrem.c`. `ufsd`: `src/ufsd#csa.c`,
`src/ufsd#sct.c`, `src/ufsd#rcl.c`, `src/ufsd#ssi.c`. `mvs38j-ip`:
`src/arch/s370/mvs/findsgd.c`, `src/arch/s370/mvsasm/igg019x8.asm`,
`src/arch/s370/mac/dsssgd.mac`. `mvs38-ibmsrc`: `ibm/IEA/IEAVENQ1.asm`,
`macros/maclib/IEFJSCVT.asm`, `IEFJESCT.asm`, `CVT.asm`, and a listing of
`macros/maclib/` for the GQSCAN control.

**Not reached, and therefore not established.**

- Whether dynamic SSCT creation requires an **`IEFSSNxx`** entry at IPL. UFSD
  and `mvs38j-ip` both create dynamically at run time, which is strong evidence
  it does not — but the `IEFSSNxx` path was not read.
- Whether **`ssct_install` detects an existing same-name SSCT**. That is
  adoption mechanics and belongs to the d2 round.

**No IBM manual was consulted. Every "established" above comes from module or
macro source in this tree** — that sentence is deliberate, and it is the
difference between this record and one that would later be quoted back as if it
were documented behaviour.

## 8. Open question this creates, for the d2 round — deliberately not answered

**Where does the second instance's name come from?** §2 passes the two-instance
test *in principle*: `SSCTSNAM` is a name and the SSCTs form a chain. But UFSD
uses **one constant**, `UFSD_SSNAME`, and two NSF stacks mean two names.
Configuration, the STC name, an instance number, something else — it is
unspecified.

**Until that is answered, the two-instance test is passed the way an exam is
passed when nobody sat it.** It is Mike's constraint made concrete, and it is
the first thing d2's design has to settle after adoption itself.

## 9. What this does NOT do

- **Does not design the adoption.** What a start does *after* it finds a
  retained anchor is the d2 round.
- **Does not start the d2 round**, and does not answer §8.
- **No code.** `src/`, `asm/`, `include/` untouched; nothing run.
- **Nothing flips to proven; no milestone moves.** d2 remains M5-2's one open
  named-unproven property.

---

## ADDENDUM at review — §3 answered ONE objection and implied it answered TWO

**Appended, nothing above rewritten.** §3's argument is **right about the half
it covers** and is not withdrawn. What is corrected is its scope: it was
presented as answering ADR-0036, and ADR-0036 makes **two** objections with two
different shapes.

This was the claim the filing itself flagged for scrutiny. It was tested against
ADR-0036 rather than accepted, and it **over-claimed on one half**.

### The two objections

ADR-0036 §155-159, quoted whole:

> **ESTAE is mandatory, or the subsystem leaks until IPL.** While the SSCT is
> registered, `S NSFP` fails `IEF612I`; an abend that skips cleanup leaves the
> SSCT registered **until IPL**.

| objection | shape | does the role change answer it? |
|---|---|---|
| **Authorization** — `IEFSSREQ` requires an authorized caller, and NSF's *clients* are unauthorized | about **who calls what** | **YES, completely.** §3(b) stands as written: the finder is the starting STC, which is authorized, and a chain walk routes **nothing** through the SSI. |
| **Lifecycle** — a registered SSCT survives an abend that skips cleanup, until IPL | about **what persists**, and true whether or not `IEFSSREQ` is involved | **NO.** It is not a transport property, and §3 should stop implying the role change disposes of it. |

### The lifecycle objection falls to a MECHANISM, not to an argument

`libc370/include/clibssct.h:47`:

```c
/* ssct_remove_by_name() remove SSCT as subsystem, requires supervisor state and key 0 */
int ssct_remove_by_name(const char *name)                           asm("@@SSREMN");
```

and its implementation (`libc370/src/clib/@@ssremn.c`) is `ssct_find(name)` →
`ssct_remove(ssct)`, returning 4 when there is nothing to remove.

**It touches only the SSCT — never `ssctsuse`, never the anchor.** So a later
start can remove a stranded registration **by its name alone**, and that closes
the ugly case as well as the ordinary one: **if the anchor is corrupt, or points
at freed storage, the SSCT is still removable**, because removal never
dereferences it.

**ADR-0036's "until IPL" was written for a design in which only the owning STC's
ESTAE could clean up.** A design in which the **next start** cleans up by name
has a recovery path that the 2026 design did not have. That is the answer — a
different design, not a different reading.

### The trade, stated as a trade

| | recovery of the rendezvous | cost left stranded |
|---|---|---|
| **today** | the SVC slot is restorable under RTM — **unconditionally** | **139264 bytes per recycle**, unrecoverable short of an IPL |
| **under D** | one small SSCT block outlives the STC until the **next start removes or reuses it by name** | the **139264 becomes recoverable** |

**The residual, because it is small and nameable: if NSF is never started again,
the SSCT stays.** That costs **one small CSA block** — not 139 KB per cycle.

### This STRENGTHENS the recommendation

The record's strongest claim is now a **weaker** one, and that is the
improvement: §3 rested on an **argument about roles**, which is one reviewer's
disagreement away from collapsing. It now rests on a **function signature and
its implementation**. An argument can be out-argued; `@@SSREMN` either exists or
it does not.

### `ssct_remove_by_name` is part of the mechanism, not an implementation detail

It joins `ssct_find` and `ssct_new` in §2's account of what libc370 already
provides, for the same reason §4's reclaim does: **the rendezvous, the reclaim
and the removal are one design.** A start must be able to *find* a retained
anchor, *adopt* it, and — when there is nothing worth adopting — *remove* the
registration so the next start is not met by `IEF612I`.

**So D's three parts, restated:** the retained anchor, the **surviving named
pointer** to it, and the reclaim that **adopts rather than frees**. None of the
three works alone, and the removal path is what makes the second one safe to
leave behind.

**Unchanged by this addendum**, and not re-argued: the ENQ refutation and its
controls, the three independent implementations of the chain walk, the rejection
table, the scope, and the counterfactual. **§8's naming question stays open** —
it is d2's first design question and nothing here answers it.
