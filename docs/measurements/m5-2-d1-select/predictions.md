# #101 Stage 2 — predictions, written BEFORE the run

**Date written:** 2026-09-02, before any deploy or job submission.
**Rule:** not edited afterwards. Results go in `README.md`, beside these.

Stage 1 (PR #103, merged `cbbca13`) proved the length arithmetic host-side
against a *modelled* transport, in one address space. Nothing in it establishes
that a cross-AS SELECT works. That is this round.

---

## Arm 1 — d1 §2.3, poll form (`TSTD1B` roles A + B)

A foreign descriptor in the mask must be refused by ownership, indistinguishably
from an unknown one, while the rest of the mask is still served.

> **P1.** `it[0].ready == 0` (the foreign entry is not ready), `it[1].ready != 0`
> (B's own entry IS served — the positive control, so a green arm is not a
> broken instrument), and `r.errno_ == 0` (the call itself is not an error).
>
> **Falsified** if the foreign entry is reported ready (ownership not enforced
> across the crossing); if B's own entry is *also* not ready (the instrument is
> broken, or the byte length did not survive, and the arm proves nothing); or if
> the call returns an error, which would make SELECT an existence oracle.

## Arm 2 — d1 §2.3, parked form (`TSTD1B` roles A + B)

The same, on the **re-scan** path rather than the immediate one — never driven
before Stage 1's own round, and it reads the stored array rather than the
request.

> **P2.** B parks on A's descriptor alone with an 8 s timeout while the host
> connects to A's port, making A's listener read-ready. B's SELECT **times out**
> (`retcode == 0`, `ready == 0`): a readiness change on a socket B does not own
> must not complete B's parked SELECT.
>
> **Falsified** if B is completed by A's readiness (`retcode == 1`) — the
> ownership check is absent on the re-scan path, which is a different code path
> from the poll form and reads its identity from the SELCB, not the request.

## Arm 3 — the wedge (`TSTD1B` roles W + V)

### The original prediction, verbatim, and why it was amended

Recorded in Stage 1 and countersigned:

> **Prediction (Stage 2, arm 3).** On the *unfixed* module, a cross-AS
> block-forever SELECT from client A leaves `g_busy` set for the life of the
> STC, and a subsequent request from client B is never served. On the *fixed*
> module, B is served. Falsified if B is served in both states (the wedge was
> never reachable), or served in neither (something other than `g_busy` holds
> it).

**The source refutes its second half.** Read before the arm was written:

- `soc_complete` is the only thing that posts `g_priv.ecb` (`src/nsfsoc.c:303`,
  `nsfthr_post((NSFECB *)&r->ecb, 0u)`).
- `nsfsel_dispatch`'s **park path calls no `soc_complete`** — all four of its
  early exits do, the only fall-through parks.
- `g_busy` has exactly one clear (`src/nsfsx.c:1239`), gated on
  `(g_priv.ecb & NSFECB_POSTED)`.

So **a parked block-forever SELECT holds `g_busy` on the FIXED module too.**
That is serialised service (ADR-0042 §10), not the `ulen` defect. With nothing
making A's socket ready, A parks on both modules and B is served in *neither* —
and the original falsification clause would then read that as "something other
than `g_busy` holds it", which is false: `g_busy` is holding it, correctly.

**What the defect actually adds is permanence.** `nsfsel_on_notify` re-scans the
**stored** array — `sel_scan(cb->items, cb->nitems, cb->ascb, cb->asid)` — so
with a count-valued `ulen` those items are residue, nothing can ever match them,
and no readiness poke can ever complete the parked SELECT.

### The amended prediction

The arm therefore makes W's socket **become ready mid-run**.

> **P3.** W parks a block-forever cross-AS SELECT on its own listener. V then
> issues an ordinary cross-AS request and is **not** served — *on either
> module*, because W holds the single private NSFRQE. The host then connects to
> W's port.
>
> - **FIXED:** the readiness poke matches W's stored items → `sel_finish` →
>   `soc_complete` → `g_busy` clears → **W completes `rc=1` and V is then
>   served**.
> - **UNFIXED:** the stored items are residue → the poke never matches → **W
>   never completes and V is never served**, until the STC is restarted.
>
> **Falsified** if W completes on the unfixed module (the stored array was not
> corrupted, so the crossing is not what it is claimed to be); if W fails to
> complete on the fixed module after a connection that is confirmed on the wire
> (something other than the item array prevents the poke matching); or if V is
> served *while W is still parked* on either module (service is not serialised,
> which would contradict ADR-0042 §10 and make the whole framing wrong).

### The control this arm cannot do without

"V was not served" and "the STC died" produce the same silence.

> **C3.** `F NSFS,STATS` must **answer** during the wedge window, and its
> `SERVED` counter must **not advance** across V's attempt. The executive is not
> hung either way — only cross-AS request service stalls; timers, devices and
> the console keep running. Without this the arm's central observation is
> unreadable.

---

## Deploy and hygiene, predicted

> **P0.** The deploy must be seen to take effect. This round changes
> `src/nsfsel.c`, which is in the `NSFS` module, and `src/nsfeza.c`, which links
> into the clients — so the tell for a mid-chain deploy failure (CLAUDE.md §5)
> is **identical values across supposedly different builds**, and it is checked
> explicitly rather than assumed. Order is `P NSFS` → deploy → `S NSFS`.
>
> **P0b.** Arms 1 and 2 are expected to be **unchanged** from their Stage-1
> behaviour except that `2.3 poll` — red in all three states of the d1c round
> because of this very defect — now passes. That single assertion moving is the
> round's cleanest before/after.
