# M5-2d0 — what (d) still contains

**Status: survey complete. No code, no live run.** Every finding below was read
from source in the working tree at `main` (`a85aedb`, post-#95). Nothing in this
step touched the machine, and §8 states that separately rather than leaving it
to be inferred.

Branch `m5-2d0-survey`, based on `main`. Host suite **3414 PASS / 0 FAIL**,
unchanged — and that is a **no-regression check only**, evidence of nothing
else, since this step changes no code at all.

---

## 0. Why the branch matters for Question 3

The kickoff specifies base `main`. The session began on `m5-2e-exit-gate`, which
carries `40c556b` — and that commit adds **447 lines to `test/mvs/tstrqxc.c`**,
the single file Question 3 is about. Answering Q3 from that branch would have
surveyed (e)'s own extension while reporting it as the state (d) inherits.

So Q3 was read at **both** revisions and each finding is attributed. The other
three questions touch no file that differs between them.

---

## 1. Question 1 — is `ubuf`/`ulen` validation still needed?

**The answer splits, and the split is the finding.** The criterion's two branches
are not exclusive here: one applies to the security half, the other to the
robustness half, and they resolve opposite ways.

### 1.1 The enumeration, exhaustive rather than sampled

Every write into caller storage made by `asm/nsfvsvc.asm` falls into exactly two
sets. **Exhaustiveness needs TWO searches, and neither alone establishes it** —
a point worth stating because the obvious one is structurally blind to Set A.

- **Search 1 — direct writes through `R8`.** Any store form
  (`ST`/`STC`/`STH`/`STM`/`MVC`/`MVI`/`XC`/`OC`/`NC`) with an `R8` base returns
  **20**, and restricting it to `ST Rn,REQ*(,R8)` returns the same **20** — so
  no non-`ST` write into the caller's block hides outside the count. But this
  search **cannot by construction** find a store through a register *loaded
  from* the caller's block, which is exactly how `ubuf` is written.
- **Search 2 — writes through a caller-derived register.** The caller's block is
  read at **17** sites, of which **6** load a pointer. Their fates are complete
  and disjoint:

  | load | register | fate |
  |---|---|---|
  | `:552` `REQUBUF` | R5 | `MVCK` **source** (`:557`, read-in) |
  | `:622` `REQUBUF` | R5 | `MVCK` **source** (`:627`, read-in) |
  | `:632` `REQRQEI` | R5 | `MVCK` **source** (`:638`, read-in) |
  | `:840` `REQUBUF` | R4 | `MOVEOUT` **destination** (`:845`) |
  | `:956` `REQUBUF` | R4 | `MOVEOUT` **destination** (`:961`) |
  | `:966` `REQRQEI` | R4 | `MOVEOUT` **destination** (`:971`) |

  The other 11 loads (`REQFUNC`, `REQTOKN`, `REQULEN`, `REQSLOT`, `REQSEXP`,
  `REQSNEW`) are scalars used as values, never as a base.

  A search for **any** store through R4 or R5 returns exactly **one
  instruction** — `MVCPIEC` (`:1041`), `MOVEOUT`'s own `EX` target. (The first
  attempt at this search returned empty because the pattern could not match the
  length form `0(1,R4)`; it was re-run with `MVCPIEC` as a **positive control**,
  which is why an empty result is not being read as a finding.) The three raw
  `D9` `MVCK` destinations are `SLSTAGE`/`SLRQE` off **R7**, the CSA slot —
  verified at each site — and so are never caller storage.

**Together the two searches are exhaustive**: every write to caller storage is
either a direct `R8` store (Set B, 20) or passes through `MOVEOUT` (Set A, 3).

**Set A — writes derived from `ubuf`. Three, all windowed.**

| site | routine |
|---|---|
| `asm/nsfvsvc.asm:845` | `XFEROUT` (`:804`) — the XFER read-out |
| `asm/nsfvsvc.asm:961` | `RQEOUT` (`:909`) — the `ubuf` move |
| `asm/nsfvsvc.asm:971` | `RQEOUT` — the `rqeimg` move |

All three are `BAL R15,MOVEOUT`, and `MOVEOUT` (`:1036-1041`) is four
instructions:

```
MOVEOUT  DS    0H
         SPKA  0(R9)              -> the caller's key
         EX    R1,MVCPIEC         move R1+1 bytes
         SPKA  0(R12)             -> back to our own key
         BR    R15
```

There is no fourth caller-storage move and no second key-borrowing block. A
store the caller could not itself make **faults** rather than corrupting.

**Set B — the caller's `NSFV_REQ` block. Twenty, none windowed.** These run in
the SVC routine's own key 0 and are *not* derived from `ubuf` at all; the
destination is the request pointer `R8`. Gated only by the entry check at
`asm/nsfvsvc.asm:343-345` (null test, then `CLC REQEYE(4,R8),=CL4'NSFV'`), which
validates a **pointer and not a key** — a caller that stamps the eyecatcher into
storage it does not own still gets 20 key-0 stores through a pointer it chose.

### 1.2 The length is clamped, in both directions, and a negative one is inert

`ulen` is clamped to `XFCHUNK` (2048, `:248`) on the way in — `XFERIN`
`:538-542` and `RQEIN` `:608-612` — and the clamped value is stored in `SLXLEN`,
which is the same word the read-out reloads (`:831`, `:947`). The C side re-clamps
through the same expression, `nsfreqx_stage_len` (`src/nsfreqx.c:18-21`), which
80-FIX made govern **both** landing-area copies.

A detail worth recording because it looks like a hazard and is not: the clamp
compare is `C` (signed), so a `ulen` with the high bit set is **not** clamped.
It cannot produce a large move, because all three loops guard with `LTR Rn,Rn` /
`BNP` (`:545-546`, `:615-616`, `:833-834`) — a negative length exits the loop
before the first piece. The move degenerates to a no-op in both directions.

### 1.3 Verdict against the stated criterion

**The cross-address-space security half is CLOSED.** Every store derived from
`ubuf` is enumerated above and every one is inside the borrowed-key window. This
is §17.3's actual subject — "the SSI/SVC boundary is where request validation
(addresses, lengths, socket ownership by ASID) becomes a real security surface"
— and for *addresses and lengths of `ubuf`* the boundary now enforces it
structurally instead of by inspection.

**The robustness half stays open and is RECLASSIFIED.** The clamp bounds the
move to 2048 bytes; **nothing validates `ubuf`/`ulen` against the caller's
actual buffer extent.** A caller that declares `ulen = 2048` over a 10-byte
buffer, in storage it owns, gets 2048 bytes written into it — successfully, no
fault, because the borrowed key permits exactly the stores the caller could make
itself. That is the criterion's second branch precisely: **robustness inside the
caller's own address space, not a cross-AS security item**, and it should not
continue to be carried as one.

**Set B remains open and remains (d)'s**, under its own name. It is not the
`ubuf`/`ulen` item and should stop being counted with it. ADR-0041 already
enumerates it as category 3 and homes it in (d); this survey confirms the count
of 20 is still exact on `main` and is now confirmed exhaustive across all store
forms, which the original count did not establish.

**The source carries the pin itself:** `src/nsfreqx.c:43` reads *"nothing is
interpreted or validated here — validation is M5-2d."*

---

## 2. Question 2 — is the guard-arithmetic obligation discharged?

**CLOSED, with evidence.** The criterion has three clauses and all three hold.

**Clause 1 — the rule exists once.** Four extracted functions in
`src/nsfreqx.c`: `nsfreqx_classify` (`:128`), `nsfreqx_slot_action` (`:163`),
`nsfreqx_actionable` (`:191`), `nsfreqx_reap_ok` (`:237`).

**Clause 2 — the live paths run through those same functions, not a second
implementation.** Both STCs:

| caller | site |
|---|---|
| NSFS (production) | `src/nsfsx.c:374`, `:477` (classify), `:509` (reap_ok), `:976` (slot_action), `:980` (actionable) |
| NSFV (probe) | `src/nsfv.c:342` (classify), `:369` (reap_ok) |
| Phase-1 registry | `src/nsfreq.c:315` via the `g_classify` seam, registered at `src/nsfsmain.c:469` |

A search for hand-rolled ASVT arithmetic outside `nsfreqx.c` returns only field
*fetches* (`asvtmaxu`, `asvtenty`) being passed **as arguments** — no second
implementation of the index, the AVAIL bit or the address compare.
`nsfsx_reap` (`src/nsfsx.c:509`) routes **every** production reap through
`nsfreqx_reap_ok`, so the pinned assertions are statements about code that runs.

**Clause 3 — the four rows and the reap decision are host-pinned.**
`test/tstreqx.c`, 150 assertions:

| region | line | what it pins |
|---|---|---|
| the classifier, every row | `:238-297` | LIVE ×2, the `asid-1` index ×2 (each off-by-one becomes a neighbour mismatch → DEAD), DEAD-by-AVAIL ×2 incl. AVAIL winning over a still-matching address, DEAD-by-reuse, UNKNOWN all four ways, and `ASID == ASVTMAXU` in range |
| the drain's per-slot decision | `:299` | `slot_action` |
| the slot state machine | `:367` | |
| the reap predicate | `:411` | `reap_ok`, incl. the UNKNOWN row |
| the two helpers must not contradict | `:455` | the implication, not an equality |
| can this pass consume that outcome | `:500` | `actionable` (M5-2b4) |
| the landing area's bounded copy | `:632` | 80-FIX |

**Recorded so it is not misread later:** M5-2c2 stage c reduced `TSTDEATH` from
four rows to one. That removed a **live driver**, not host coverage —
`src/nsfreqx.c` and `test/tstreqx.c` were untouched by it, and all four rows plus
all four UNKNOWN branches keep their host pinning. A reader a year from now is
likely to get this exactly backwards.

**One finding, reported and NOT fixed (red line: no code).**
`src/nsfv.c:294` defines `NSFV_ASVT_AVAIL 0x80000000U` and **never uses it** — a
vestigial duplicate of `NSFREQX_ASVT_AVAIL` (`include/nsfreqx.h:228`, the one
`src/nsfreqx.c:153` actually reads, with `test/tstreqx.c:267`/`:272` pinning it).

**Scope, established rather than assumed:** a search across `src/`, `include/`,
`test/` and `asm/` returns that **single line** — it is defined in a `.c`, not a
header, so no other file can pick it up. It is dead, a `#define` draws no
unused-symbol warning, and it sits in the probe STC as a ready-made seed for a
future hand-rolled check — the precise shape this obligation exists to prevent.
Not a second implementation today; a second *encoding of the constant*.

---

## 3. Question 3 — does (e)'s tooling drive probe verbs against NSFS?

**Yes — and not the ones #67 is about. #67 is unreachable during (e).**

That is a sharper answer than the question's yes/no, and both halves matter.

### 3.1 Which verbs the file stages — identical at both revisions

`TSTRQXC` stages exactly three, through the single helper `xc_req_init`:

| verb | code | `main` | `40c556b` (e) | staged by |
|---|---|---|---|---|
| `NSFV_REQ_QUERY` | 4 | `:247` | `:261` | `xc_query` |
| `NSFV_REQ_SLOT` | 7 | `:264` | `:278` | `xc_slot_cas` |
| `NSFV_REQ_RQE` | 6 | `:384`, `:724` | `:398`, `:1123` | `xc_request`, `t_bad_ubuf` |

**`FNECHO` (1) and `FNXFER` (2) are never staged at either revision.** (e)'s
extension added **no new verb**: the measurement roles reuse `xc_request`, whose
SVC verb is `RQE` and whose *inner* NSFRQE `fn` is `XC_FN_UNKNOWN` (250) — a code
no dispatcher case claims.

### 3.2 The measurement roles do reach `SLOT`

Stage a's §1 describes the measured request stream as side-effect-free, and that
is true of the stream. It is **not** true of the roles as a whole:
`xc_run_measure` (e-branch `:1069`) brackets `MA`/`MB` with the barrier, which
issues `NSFV_REQ_SLOT`. The solo roles `MS`/`MSP` take the `else` branch and
issue none.

**The cost is two requests per client, and it is bounded** — which is what makes
§3.3's conclusion checkable rather than merely asserted. `xc_barrier` (`:474`)
raises its own flag with one `xc_slot_cas` (`:478`), then **polls the partner by
reading `g_anchor->slots[theirs].req_state` straight out of CSA** (`:487`) — a
direct key-8 read through the mapped anchor, **not** an SVC call, as the
function's own header says. The flag is lowered by one more `xc_slot_cas`:
on success back in `xc_run_measure` (`:1082`, the barrier's teardown half — not
a separate mechanism), on timeout by the barrier itself (`:494`).

So a measurement client issues **exactly 2 `SLOT` requests for the whole run**,
one at each end, outside the timed region, *independent of window length*.

### 3.3 Why that still cannot strand a slot — the decisive fact

`FNQUERY`, `FNUNSTG` and `FNSLOT` are dispatched **before the claim loop**:

- dispatch chain at `asm/nsfvsvc.asm:390-395` — the same pre-claim position the
  retired `FNORPH` is rejected from ("claim nothing", `:388-389`)
- `CLAIMLP` begins at `:445`; `CLAIMOK` at `:472`
- `DOSLOT` (`:1163-1174`) ends `BR R14` — it never claims, never publishes,
  never parks, and never reaches the executive

And the contention counters live **only** in the claim path: `ANCCOLL` is
incremented at `:464-466`, inside `CLAIMLP..CLAIMOK`; `ANCEXH` at `:1245-1248`
in `POOLFUL`, reachable only from claim exhaustion.

**So no (e) role can strand a slot, move `EXHAUSTED`, or move `COLLISIONS`
through a probe verb.** The `SLOT` and `QUERY` verbs the roles do issue are
inert with respect to every number (e) measures.

### 3.4 Reported as the kickoff asks

**Decision 3's stated reason did not apply.** #67 was placed in d1 because a
probe verb stranding a slot would move `EXHAUSTED` and the contention figures;
the verbs that strand (`ECHO`, `XFER`) are not reachable from any `TSTRQXC` role,
and the verbs that are reachable cannot strand. #67 could have waited for c3.

**The decision does not change without Mike** — this is the record, not a
proposal. Worth noting on the other side: the verbs (e) *does* issue are probe
scaffolding reachable by an unauthorised caller, which is c3's subject
regardless, so d1 and c3 are not made independent by this finding.

---

## 4. Question 4 — how big is d2 now?

**Small in code. One open decision that is not small, and it is not the decision
already taken.** The 2026-08-22 ruling is not re-derived here; only sized.

### 4.1 Where an UNKNOWN slot goes today

At **service** time: `nsfreqx_slot_action` returns `ACT_HOLD` for UNKNOWN
(`src/nsfreqx.c`, the `verdict == NSFREQX_CL_UNKNOWN` arm), so the slot is set
`HELD` — never dispatched, never POSTed through.

At **shutdown**: `nsfsx_wake_parked` (`src/nsfsx.c:609-621`) skips any slot whose
client is not LIVE —

```c
if (nsfsx_client_state(slot) != NSFREQX_CL_LIVE) continue;   /* :618 */
```

— so an UNKNOWN slot is **never nudged**, its client never takes the `WQUIES`
bail, `inflight` is never given back, `nsfsx_drain_inflight` (`:634-649`) burns
its 10 s ceiling and returns 0, and `nsfsx_stop` takes the retain branch:
`NSF054W`, CSA **and** the SVC router retained until IPL.

### 4.2 What force-reap-after-ceiling would have to touch

**The reap machinery is pure C.** `nsfsx_reap` (`src/nsfsx.c:498-528`) performs
the two-move reclaim — `__cas(state → CLAIMED)`, clear, plain store to `FREE` —
through `__cas`, a libc370 seam. **No assembler.** So d2 moves no layout, bumps
no `ANCVERNO`, and does **not** trigger "re-run every stage's live gate".

**b3's fourth-state problem does not apply here.** That concern —
`CS(observed → CLAIMED)` succeeding trivially — is about a **CLAIMED** slot, the
write-in fault leak, which is a different open item (ADR-0039/0041 fault
recovery). An UNKNOWN slot at shutdown is `PENDING` or `HELD`, and
`nsfreqx_reap_ok` already lists **both** among its published states, so once the
UNKNOWN gate is passed the compare is well-defined and exclusive.

**The one blocking edit** is `nsfreqx_reap_ok`'s first clause, which returns 0
for UNKNOWN unconditionally with a long rationale attached. d2 needs a shutdown
context — a parameter, or a sibling predicate — and must preserve the
one-encoding rule that clause exists to serve.

Estimated shape: one predicate change, one loop on the drain's failure path,
host rows in `test/tstreqx.c`. **Small.**

### 4.3 The consequence that is NOT sized by the 2026-08-22 decision

Force-reap decrements `inflight`. If it reaches zero, `nsfsx_drain_inflight`
returns 1, and `nsfsx_stop` takes the **`else`** branch —
`nsfsx_router_unload()` **and** `nsfsx_anchor_free()`. Force-reap therefore
converts "retain everything" into "**free everything, including the module**".

The retain branch's own comment (`src/nsfsx.c:676-684`) is explicit that this is
the worse outcome:

> a client that failed to drain is parked in a `WAIT` *inside that code*,
> supervisor state, key 0 … Retaining the anchor while freeing the code is
> **STRICTLY WORSE** than leaking both

The 2026-08-22 decision was about the **slot**. It did not rule on the
**module**, and the two are coupled through `drained` — a single flag that today
means "nothing is in flight" and would come to mean "nothing is in flight *or* we
gave up on it". Separating them is another flag, not another investigation.

### 4.4 Answer to the sizing question

**d2 is a small change carrying one decision**, not its own investigation. On
the kickoff's escape hatch: the condition for decoupling the flip from d2 —
that d2 has grown into an investigation — **is not met**. Mike decides; this is
the size.

**But the decision in §4.3 is a gate, not a detail.** It is carried into §6 and
§7 as its own item so that a reader who takes only "small change" from this
section does not miss that the smallness is conditional on ruling first on
whether shutdown may free the router under a possibly-parked client.

---

## 5. Gathered for d1, so it is not rediscovered

### 5.1 Descriptor resolution — THREE points, not one

The kickoff asks whether `req_socket()` is the only one. **It is not.**

| site | kind | d1 relevance |
|---|---|---|
| `src/nsfreq.c:547` `req_socket()` | **client-directed** — the dispatcher path | the obvious one |
| `src/nsfsel.c:83` `sel_scan()` | **client-directed** — resolves *each* SELECT item's descriptor from the client's mask, carried in `ubuf` | **a check at `req_socket` alone misses this entirely** |
| `src/nsfsoc.c:273` (in `soc_complete`) | **internal** — clears `r`'s own pend slot on its own socket | a check here would be wrong; the stack is completing its own request |

`sel_scan` is the one worth flagging: SELECT resolves N descriptors in a loop
from an array the client supplied, so an ownership check that lands only on the
per-verb dispatch path leaves a client able to select on another client's
descriptors.

### 5.2 The inherited child — d1's first gate

`src/nsftcp.c:1397-1409`, `tcp_child_create`:

```c
    cs = soc_create(ls->domain, ls->type, ls->proto, ls->ops);
    if (cs == NULL && tcp_reclaim_timewait()) {
        cs = soc_create(ls->domain, ls->type, ls->proto, ls->ops);
    }
    if (cs != NULL) {
        cs->apptok = ls->apptok;        /* same app scope (TERMAPI)       */
    }
```

The child inherits the listener's token **unconditionally** whenever allocation
succeeded. The ordering is safe: `sock_alloc` (`src/nsfsoc.c`) does
`memset(s, 0, sizeof(*s))` before returning, so `apptok` starts at **0** and is
set only by `do_socket` (`src/nsfreq.c:540`, `s->apptok = r->apptok;`) or by the
line above — there is no path that leaves a reused slot carrying a previous
app's token.

**Consequence for d1:** a check of the form *"the socket's `apptok` must equal
the requesting caller's token"* **passes** for an accepted child, because the
token is genuinely inherited rather than zeroed. The gate case is satisfiable.

### 5.3 Where the caller identity arrives — both sides already exist

- It enters at `nsfreq_dispatch_id(r, caller_ascb, caller_asid)`
  (`src/nsfreq.c:746`), called from `src/nsfsx.c:1389` with `slot->req_ascb` /
  `slot->req_asid` — the identity the FLIH captured at the claim.
- **Today exactly one verb consumes it:** `RQ_INITAPI` (`:757` →
  `do_initapi(r, caller_ascb, caller_asid)` → `app_alloc`). The header
  (`include/nsfreq.h:267-269`) says so in as many words.
- `app_alloc` records `(ascb, asid)` per app slot; `app_index(token)` validates a
  token against index, `inuse` and generation. **So both sides of the comparison
  d1 needs are already present** — no new plumbing.

**And the trustworthy side is the captured identity, not `r->apptok`.**
`nsfreqx_dispatch_in` (`src/nsfreqx.c:64-75`) rewrites only `ecb`, `ubuf` and
`ulen`, so the client-supplied `apptok` crosses the boundary unchecked. (`apptok`
occurs **28** times across `src/` and `include/` on `main` — the same count stage
a measured on the (e) branch.)

### 5.4 Phase 1 with a zero identity — the one enforcement point

`nsfreq_dispatch(r)` is exactly `nsfreq_dispatch_id(r, 0u, 0u)`
(`src/nsfreq.c:743`), so every Phase-1 call site supplies a zero identity. The
single point that refuses to turn that into a verdict is
`nsfreq_app_classify` (`src/nsfreq.c:311-315`):

```c
    /* THE RED LINE, IN ONE PLACE (see the header). */
    if (ascb == 0u || g_classify == NULL) {
        return NSFREQ_APPCL_NONE;
    }
```

d1's check must reach the same conclusion the same way: a zero identity is **not
a verdict**.  Phase 1 registers no classifier: `nsfreq_set_classifier` is called
only from `src/nsfsmain.c:469` (the NSFS module), so `g_classify` stays NULL in
the `NSF` module.  `src/nsfmain.c:306` DOES register the Phase-1 request drain
(`evt_set_request`) -- the two seams are separate, and only the classifier one
is absent.

---

## 6. What is closed, what stays open

| item | status after this survey |
|---|---|
| §17.3 `ubuf`/`ulen` validation — **cross-AS security** | **CLOSED** (§1.1/1.3): 3 stores, all inside `MOVEOUT`'s borrowed-key window; length clamped both directions |
| `ubuf`/`ulen` — **caller-side robustness** | **open, RECLASSIFIED** — not a §17.3 item; no validation against the caller's real buffer extent |
| the caller's `NSFV_REQ` block — 20 key-0 stores | **open**, (d)'s, count re-confirmed and now confirmed exhaustive across all store forms |
| obligation #5 — guard arithmetic extracted + host-tested | **CLOSED** with evidence (§2) |
| #67 — probe verbs during (e) | **unreachable during (e)** (§3); decision 3's stated reason did not apply; the decision itself is Mike's |
| d2 — UNKNOWN at shutdown | **small change** (§4.2): pure C, no asm, no layout move; the escape-hatch condition is **not** met |
| **may shutdown free the SVC router under a possibly-parked client?** | **OPEN — a decision nobody has taken** (§4.3). Force-reap flips `drained`, which takes `nsfsx_stop`'s `else` branch and **unloads the module**. The 2026-08-22 ruling was about the *slot*; it did not rule on the *module*. **d2 cannot be implemented without answering this.** |

Nothing was closed without evidence, and the two closures are recorded in the
table above rather than only in prose.

---

## 7. Findings reported and deliberately not acted on

1. `src/nsfv.c:294` — `NSFV_ASVT_AVAIL` defined and never used (§2).
2. The signed clamp compare in `XFERIN`/`RQEIN` (§1.2) — inert today because
   every loop guard is also signed; recorded because the safety rests on the
   pairing, and either half changing alone would break it.
3. `sel_scan` as a second client-directed descriptor resolution point (§5.1) —
   d1's scope is wider than `req_socket`.
4. **`nsfsx_stop` couples the slot decision to the module decision through one
   flag** (§4.3). `drained` today means "nothing is in flight"; force-reap would
   make it mean "nothing is in flight *or* we gave up on it", and the `else`
   branch it selects calls `nsfsx_router_unload()` — freeing storage a client
   may be parked in, supervisor state, key 0. The retain branch's own comment
   (`src/nsfsx.c:679`) calls that "**STRICTLY WORSE** than leaking both".
   Separating the two meanings is one more flag; *deciding* which behaviour is
   wanted is Mike's, and it is the gate on d2 rather than a detail inside it.

---

## 8. Source vs. machine

**Everything above was read from source.** No question required the machine, so
none was asked of it: no live run, no STC started or stopped, no deploy, no
console command, nothing submitted.

The one command executed against the build was `make test-host`
(**3414 PASS / 0 FAIL**, matching the baseline), and the diff is docs-only —
this file and nothing else.
