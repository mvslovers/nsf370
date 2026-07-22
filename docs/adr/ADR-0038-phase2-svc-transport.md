# ADR-0038 — Phase-2 app↔stack transport: a private (dynamically installed) SVC

**Status:** Proposed (2026-07-21). **Supersedes the transport decision of ADR-0036**
(the MVS Subsystem Interface). ADR-0036's cross-AS state/key rules, CSA-anchor design,
in-flight drain, and ESTAE content **remain valid and are reused here**; only the
*dispatch* — how an application task reaches the stack across the address-space
boundary — changes from `IEFSSREQ` (SSI) to a **private SVC**.

Phase 2 puts applications in their own address spaces and the stack in the `NSFS` STC,
so the app↔stack hand-off must cross address spaces. **The `NSFRQE` request block is the
phase boundary (spec §1.3, §10.4, frozen at M3): only the transport under
`nsfreq_submit` changes, never the request format.** This ADR fixes that transport to a
**dynamically installed private SVC** whose routine runs in the caller's address space.

**Relates to:** spec §1.3 (the two-phase split), §10.4 (the frozen NSFRQE — the payload
the SVC carries in M5-2), §17.3 (security — the cross-AS boundary is the attack surface,
handled in M5-2), ADR-0022/0023 (single-task run-to-completion; the completion-POST
seam), ADR-0036 (the SSI transport this supersedes; **its cross-AS rules are retained**).
**Evidence pins:**
- `mvslovers/mvs38j-ip` — the proven SVCTABLE-steal ancestor: `src/arch/s370/mac/wastcp.mac`
  (the STCP workarea: `STP@SVCT` = the stolen slot address, `STPSVCTB DS XL8` = the saved
  original entry), `src/arch/s370/mac/stcp#bug.mac` (the `STCPSVC0`/`STCPSVC9`
  install/term entry points), `src/arch/s370/mvsasm/igc0024e.asm` (a working **Type-3 SVC
  routine shape** — entry registers, `R5 = @SVRB`, `R6 = @entry`, RENT), and
  `src/arch/s370/mvs/mvsintr.c` (`mvsauth` — the client-side `EX`-`SVC 0` issue with the
  SVC number in a register; `testauth` — `TESTAUTH FCTN=1`, R15≠0 ⇒ NOT authorized).
- `cbt571/PDS/STCPSVC0` + `STCPSVC9` — the **full** SVCTABLE-steal source (the exact walk
  `CVT → CVTABEND → SCVTSVCT → +svc#·8`, `MVC STPSVCTB,SVCENTRY` save, key-0 store,
  `MVC 0(L$SVC,R3),STPSVCTB` restore) and the model attribute `AL1(SVCTP34,0,0,0)`.
- `cbt571/PDS/STCPSVC` — the ancestor's **transport** SVC routine: an **arbitrarily-named**
  CSECT (`STCPSVC`, *not* `IGCnnn`), entered `USING …,R6`, documenting the entry registers
  (`R1`=issuer R1, `R5`=@SVRB, `R6`=@entry, `R7`=@ASCB, `R13`=18-word savearea) and the exit
  convention verbatim: **"R0, R1, R15 are the only regs returned to the issuer; R2-R14 are
  restored by the system."**
- `mvs38j-ip/src/arch/s370/mac/ihasvc.mac` and libc370 `include/ihascvt.h` — the 8-byte
  `SVCENTRY` DSECT (`svcepa`@0, `svctype:3`/`svcapf:1`@4, `svclock`@6) and the secondary
  CVT (`scvtsvct`@0x84).
- ADR-0036 and `ufsd/docs/cross-as-reference.md` — the cross-AS `__xmpost` / in-flight /
  ESTAE mechanics that carry over unchanged; libc370 `@@xmpost.c` / `@@ascb.c` /
  `@@super.c` / `@@uinc.c` / `@@loadhi` / `@@apfset.c`.

---

## Context

ADR-0036 chose the SSI (a dynamically registered SSCT/SSVT router invoked by `IEFSSREQ`)
and proved it live in the Stage-0a `NSFP` probe. That probe also surfaced the decisive
limitation, recorded as ADR-0036's **open M5-2 question**:

> `IEFSSREQ` is an **authorized** branch-entry, so the calling task must be
> APF-authorized. NSF's goal — run existing EZASOKET applications **unchanged,
> relink-only** — includes **unauthorized** problem-state applications, which cannot call
> `IEFSSREQ` directly.

Stage-0a worked because the probe client authorized *itself* at runtime
(`clib_apf_setup` → SVC 244). A relink-only application is not going to insert a
self-authorization call — and even if a shim did, handing arbitrary applications APF
authorization is the opposite of the isolation Phase 2 exists to provide. **The transport
must be usable by an unauthorized problem-state caller with no APF and no
self-authorization.**

The one MVS mechanism that transitions an unauthorized problem-state program into
authorized supervisor state **without APF** is the **SVC**. When a program issues an SVC,
the SVC first-level interrupt handler dispatches the SVC routine in **supervisor state,
key 0** regardless of the issuer's authorization — that is the entire purpose of the SVC
mechanism. A private SVC whose routine performs the cross-AS hand-off is therefore the
APF-free transport an unauthorized caller can use. This is exactly the pattern the
`mvs38j-ip` ancestor used (`STCPSVC0` steals an SVCTABLE slot; the client issues the SVC
from problem state via `EX`-`SVC 0`). ADR-0036 rejected the SVC over "SVC-slot install
friction" — but that trade-off was struck **before** the APF constraint was on the table;
with the relink-only-unauthorized requirement now decisive, the friction is worth paying
and the SSI's dynamic registration buys nothing the SVC cannot, while the SSI cannot
serve the unauthorized caller at all.

Everything cross-AS *below* the dispatch is unchanged from ADR-0036: a CSA rendezvous
anchor, `__xmpost` (branch POST via `CVT0PT01`) from supervisor state, an in-flight
counter with a shutdown drain, and a mandatory ESTAE. Stage-0a′ reuses that core
verbatim and swaps only the dispatch layer, so the reused mechanics carry their live
proof forward.

## Decision

**The Phase-2 app↔stack transport is a dynamically installed private SVC: the `NSFS` STC
steals an unused installation SVC slot at start (pointing it at a routine loaded into
CSA) and restores it at stop and on abend.** In M5-2, `nsfreq_submit`'s single call site
swaps its `xq_push`+`nsfthr_post` pair for an `EX`-`SVC` that carries the NSFRQE across;
everything above it (the frozen NSFRQE, the dispatcher, the socket layer) is unchanged.

Concretely, adopted from the ancestor:

### 1. A stolen SVCTABLE slot points at a CSA-resident routine

The STC locates the SVC table exactly as `STCPSVC0` does — `CVT` (absolute location 16) →
`CVTABEND` (CVT+X'C8', the secondary CVT / SCVT) → `SCVTSVCT` (SCVT+X'84', the origin of
the SVC table) → `+ svc#·8` (each entry is 8 bytes: `svcepa`@0, attributes@4, locks@6).
For an **unused installation SVC number (200–255**; IBM assigns user SVCs from 255 down),
in a key-0 window the STC **saves the original 8-byte entry**, then stores the routine's
entry point into `svcepa` and the model attributes into the attribute/lock bytes. The
routine is loaded into CSA via `__loadhi` (a distinct load module, `RENT`,
`startup=false`, `ac=1`) so it is addressable from any caller's address space.

### 2. SVC type 3, no APF attribute

The installed attribute byte is **`X'C0'` (`SVCTYPE3`, `svcapf=0`, no locks)** — the
ancestor's `SVCMODEL = AL1(SVCTP34,0,0,0)`. Two consequences are load-bearing:

- **`svcapf = 0`** means the SVC does **not** require the issuer to be authorized. An
  unauthorized problem-state program may issue it. This is the whole point.
- **Type 3** (versus Type 1) means the routine runs **enabled**, holding no locks, as an
  `SVRB` on the caller's TCB — so it **may issue `WAIT`** (and be redispatched by a
  cross-AS `POST`). A Type-1 SVC runs disabled and cannot wait. Type 3 (single CSA/LPA
  module) rather than Type 4 (multi-module) because the routine is one `__loadhi`'d
  module; the SVC table attribute `X'C0'` is shared by types 3 and 4, and Type 3 matches
  the load model. `R5 = @SVRB` and `R6 = @entry` are valid for types 2/3/4 (see
  `igc0024e.asm`).

### 3. The anchor is published to the routine without SSI

There is no SSCT, so the routine cannot recover the anchor via `ssct_find`. Instead the
STC **patches the anchor address into a fixed word of the CSA routine module** once at
start (a load-time relocation, written before the slot is stolen and never again — so
reentrancy is preserved and no invocation ever sees an unpatched value). The routine
addresses that word through `R6` (its own entry point, supplied by the SVC FLIH). This
keeps the transport **entirely SSI-free**: the only system-wide resource touched is the
stolen SVC slot.

### 4. Install/restore, and the mandatory restore

The STC installs the slot at start and **restores the original 8-byte entry** at stop
(orderly) and on abend (ESTAE). `STCPSVC9`'s restore is a single key-0
`MVC slot ← saved8`. Because restoring merely **redirects** the slot (it does not pull the
CSA code out from under a caller already dispatched into the routine), **the ESTAE may
restore the slot under RTM** — unlike ADR-0036's SSCT, which could not be deregistered
under RTM because a foreign PSW might be inside the router. The ESTAE therefore:
restore the slot (the SVC number is safe system-wide again) → clear the anchor `ACTIVE`
flag → **do not** free the CSA module or the anchor (a foreign PSW may still be inside) →
percolate. A dangling stolen slot is **stricter** than ADR-0036's SSCT-until-IPL leak (it
corrupts that SVC number for every caller, not just NSF), so the restore is
non-negotiable and is exercised on both paths from day one.

### 5. The state/key rules for the SVC path

The SVC routine runs **supervisor state, key 0, throughout** (the SVC FLIH sets this;
there is no problem-state phase as there was under `IEFSSREQ`). The cross-AS mechanics
carry over from ADR-0036 with the state/key adjusted to a supervisor-throughout routine:

| Operation | Mechanism | State in the SVC path | ADR-0036 (SSI router, for contrast) |
|---|---|---|---|
| cross-AS POST (both directions) | `__xmpost(ascb, ecb, code)` via `CVT0PT01` | supervisor / key 0 (native — no `__super` needed) | supervisor / key 0 (reached via `__super`) |
| WAIT on the reply ECB | `WAIT` (SVC 1) on the CSA reply ECB | **supervisor / key 0, on a key-0 CSA ECB** | problem state, on a key-8 **stack** ECB |
| routine entry | `R0`/`R1` captured raw; anchor via `R6` | supervisor / key 0 | supervisor / key 8; `R1 = SSOB` |

The WAIT row is the one genuine behavioural change and is one of the **two empirical
unknowns** (below): because the routine never leaves key 0, the reply ECB can live in the
CSA anchor (a key-0 ECB waited from supervisor state is legal) rather than as a
problem-state key-8 stack local — **ADR-0036's key-8-stack-ECB rule does not transfer.**

A refinement the SVC path allows: instead of a per-client timed WAIT + liveness re-poll
(ADR-0036's STIMER-based drain participation), the **STC posts any in-flight client's
reply ECB with a sentinel on quiesce** (it knows the CSA reply ECB and the client ASCB),
so a parked client wakes, sees `ACTIVE` cleared, gives its in-flight count back, and
bails — the drain completes without arming a STIMER on the caller's TCB (STIMER is a
per-TCB singleton; arming one inside an SVC on an arbitrary caller's TCB is a side effect
to avoid). Same safety property as ADR-0036 (the drain terminates, no infinite hang),
better suited to the SVC context.

### 6. The probe interface — `R1` = a request block (the M5-2 NSFRQE shape)

The client passes `R1` = the address of a small request block (`NSFV_REQ`: an eye-catcher
`C'NSFV'` to reject stray SVC callers, a function code, and the token) — the same
`R1 = raw plist` discipline as Stage-0a's `R1 = SSOB`, and the shape M5-2 uses (the frozen
NSFRQE is passed by pointer). The routine captures `R1` by inline register capture before
anything else, validates the eye, round-trips the token through the CSA anchor, and writes
the echoed token, the served counter, and the rc **back into the caller's block**; it also
sets `R15` = rc. The client reads the result from its own block, so the round-trip proof
has **no dependence on the SVC register-return convention** (only `R15` is a proven
type-3 return path, via `igc0024e`/`mvsauth`; the block is authoritative). The token
itself crosses the address-space boundary via the CSA anchor (SVC→STC→SVC); the block is
only the client's local interface.

**Security surface, deferred to M5-2 (spec §17.3).** A key-0 routine touching a
caller-supplied pointer is the attack surface: the probe uses a plain key-0 store into the
caller's block (the probe's client is trusted). M5-2 replaces this with `MVCP`/`MVCS`
(move with the caller's key, so a bad or hostile pointer faults instead of letting a key-0
store corrupt arbitrary storage) plus request validation and `owner_ascb` checks. Named
now; out of scope for the probe, which round-trips a token only.

### 7. Stage the transport before the payload

As with ADR-0036, the mechanics are proven **first** on an empty token — Stage-0a′, the
`NSFV` probe (distinct from the reserved `NSFS`) — so M5-2 builds on a validated seam. No
NSFRQE, socket, or protocol code is involved.

## The two empirical unknowns

Predicted here; the live run decides, and the resolution is recorded back into this ADR
(the M4 "S0C6-vs-column-72" discipline: the abend names the real rule when the prediction
is wrong).

1. **WAIT state/key.** *Prediction:* the routine issues `WAIT` (SVC 1) from **supervisor
   state, key 0**, on a **key-0 CSA ECB** embedded in the anchor. Rationale: the routine
   never leaves supervisor key 0, and a supervisor WAIT on a key-0 ECB is legal; the
   cross-AS POST reaches that CSA ECB (common storage, same virtual address in both
   spaces) targeting the caller's ASCB. ADR-0036's "problem-state, key-8 stack ECB" rule
   was a consequence of the SSI router running problem-state key 8, and **does not carry
   over.** *If wrong:* the abend (e.g. an X'0Cx' on the WAIT, or a POST/ECB key violation)
   names the correct placement.
   **RESOLVED — prediction CONFIRMED** (live MVSCE, 2026-07-21): 106 round trips, a
   supervisor/key-0 `WAIT` on the key-0 CSA reply ECB, no abend. Stage-0a's key-8-stack
   rule indeed did not transfer.
2. **SVC type.** *Prediction:* **Type 3** — installed with attribute `X'C0'`, no APF,
   no locks; enabled; a single CSA module. Rationale: the routine must `WAIT` (rules out
   Type 1), is one `__loadhi`'d module (rules out Type 4's multi-module split), and the
   ancestor's transport used exactly `SVCTP34`. The type governs entry linkage (`SVRB`,
   `R5`) and whether `WAIT` is legal. *If wrong:* a disabled-wait or SVRB-linkage abend
   names it.
   **RESOLVED — prediction CONFIRMED** (live): SVC 239 installed `X'C0'` (Type 3, no APF)
   dispatched the routine enabled/supervisor/key 0; the nested `WAIT` was legal; no
   SVRB-linkage or disabled-wait abend.

## Why

- **The only APF-free unauthorized→authorized transition.** SSI cannot serve an
  unauthorized caller; the SVC is precisely the mechanism that can. This is the capability
  the SSI path lacked (ADR-0036's open M5-2 question), and it is decisive for relink-only.
- **Proven in the ancestor on comparable hardware.** `mvs38j-ip`'s STCP stole an SVCTABLE
  slot (`STCPSVC0`/`STCPSVC9`) and its clients issued the SVC from problem state; the full
  source is in `cbt571/PDS`. The SVC-routine shape is the classic `igc0024e.asm` form.
- **All primitives already exist.** The SVC table is located by libc370's `ihascvt.h`
  structs; `__loadhi` / `__xmpost` / `__uinc` / `__udec` / `__super` / `clib_apf_setup`
  (STC-side) are the same libc370 seams Stage-0a proved live.
- **Restartable after an abend.** Because the ESTAE restores the slot, a fresh `S NSFV`
  after an abend steals the (restored) slot cleanly — no IPL to restart, unlike ADR-0036's
  SSCT (which needed an IPL to clear a stale registration). Only the orphaned CSA module +
  anchor leak until IPL, the same CSA-retention ADR-0036 already accepts.

## Consequences

- **A dangling stolen SVC slot is worse than the SSCT leak.** An unrestored slot corrupts
  that SVC number **system-wide** (every issuer, not just NSF). The restore at stop and in
  the ESTAE is mandatory; the probe verifies the slot equals the saved original after stop
  and after an induced abend.
- **The SVC routine is `RENT` HLASM.** It is entered concurrently from many address spaces
  and tasks, so it has **no writable statics** and works off the `SVRB` / the CSA anchor.
  It is written in assembler (not cc370 C): an SVC routine has no C-runtime environment
  (the cc370 prologue's `@@CRTGET` finds a per-TCB CRT that an arbitrary caller's TCB may
  not have in a usable state), and the register-in/register-out convention (`R0`/`R1`/`R15`
  to the issuer) is native to assembler — the `igc0024e.asm` model. *(Mike's call: pure
  assembler.)*
- **The cross-AS boundary is the security surface** (spec §17.3). A request originates in
  an untrusted AS. The probe dereferences no caller pointer (register-only, §6), so it
  carries **no** trust boundary yet; M5-2 adds request validation, `owner_ascb` checks,
  and `MVCP`/`MVCS` for any caller-storage access.
- **STIMER-per-TCB caution.** The drain uses the STC-posts-parked-clients mechanism (§5)
  precisely to avoid arming a STIMER on the caller's TCB. If a future path does need a
  per-caller timeout, it must not clobber a STIMER the caller already armed.
- **Not host-simulable.** There is no SVC table / ASCB / CSA on the host; the transport
  cannot be exercised natively. Host coverage is the struct-layout `NSF_SIZE_ASSERT`s
  (the CSA anchor) firing at cc370 cross-compile. **Proof shifts to a live MVSCE run.**
- **Two more load modules on MVS.** The STC (`NSFV`) and the CSA SVC routine (`NSFVSVC`,
  `entry=NSFVSVC`, `startup=false`, `ac=1`, `RENT`) are distinct load modules; the routine
  carries no writable statics.
- **The SVC routine is arbitrarily named, not `IGCnnn`.** The `IGCnnn` naming (e.g.
  `IGC0024E` for SVC 245) is only for SVC routines MVS loads **by name** (SYSGEN / the
  standard SVC loader). A stolen slot with a directly-installed **resident** CSA entry point
  bypasses that loader, so the routine name is free — the ancestor's transport SVC is CSECT
  `STCPSVC`, and `NSFVSVC` follows it. *(Mike's question, resolved against
  `cbt571/PDS/STCPSVC`.)*
- **The SSI probe (`NSFP`) stays in-repo** as the SSI reference/history (ADR-0036 records
  it). Retiring it is a separate, deliberate decision — not a silent deletion.

## Status / history

- **2026-07-21 — Proposed.** Authored ahead of the Stage-0a′ SVC probe (`NSFV`) that
  validates the SVC transport with an **unauthorized** problem-state client — the thing
  the SSI path could not do. Supersedes ADR-0036's transport decision (ADR-0036 annotated
  transport-superseded; its cross-AS rules retained).
- **2026-07-21 — VALIDATED LIVE on MVSCE (pending Mike's countersign).** The transport is
  proven with an **unauthorized** client. Steal safety: SVC 255-240 are in use on the
  target, so the STC's scan (the entry point shared by the most slots in 200-255 =
  `0000CCC8`, the invalid-SVC marker) picked **SVC 239** (highest of 40 free); the naive
  adjacent-slot heuristic that first refused 255 was replaced by this modal scan.
  **Stage 1** (`S NSFV`): `NSFV034I SVC 239 STOLEN (OLD EP 0000CCC8 → NEW EP 00A82B08)` →
  `NSFV001I READY`; `F NSFV,STATS` = 0/0; `P NSFV` → `NSFV095I SVC 239 RESTORED` → clean;
  **double-start** re-stole with `OLD EP 0000CCC8` again (proving the restore put the
  original back); no abend, no dump. **Stage 2** (`make test-mvs --only TSTSVC`): **TSTSVC
  batch CC 0 + TSO CC 0, 314 PASS / 0 FAIL** — the client `PASS: client is UNAUTHORIZED
  (TESTAUTH FCTN=1) and does not self-auth`, then 53×2 = 106 round trips, `token
  round-trips byte-exact (echo = token+1)`, monotonic served; the STC `NSFV002I SERVED=106
  INFLIGHT=0` (drain clean). No S0C/S16D/S047/S202/S102/NSFV900 anywhere; final `P NSFV`
  restored the slot, no dump — MVSCE left clean. **Both empirical unknowns confirmed**
  (above). **Not force-tested live** (as with ADR-0036): the induced-ABEND path (the ESTAE
  restoring the slot under RTM) is built + host-reasoned but not run, to avoid a dump.
  Amendments (the unauthorized-app resolution is now MET; the NSFRQE keyed move; client-
  death cleanup) will be appended as M5-2/Stage-0b/0c land.
- **2026-07-22 — countersign findings (append-only corrections).**
  1. **`MVCP`/`MVCS` correction (§6, Consequences) — WRONG for MVS 3.8j.** `MVCP` (Move to
     Primary, `DA`) and `MVCS` (Move to Secondary, `DB`) are **dual-address-space (DAS)**
     instructions (primary/secondary ASN, `SSAR`) absent on a base S/370 — and ADR-0036
     correctly records "no cross-memory services" on this target, so §6 contradicts it.
     The non-DAS key-checked move is **`MVCK` (Move with Key, `D9`)**: a key-0 routine
     copies to/from a key-8 caller buffer under the *caller's* key, so a bad or hostile
     pointer takes a protection exception instead of a silent key-0 clobber. **Read §6's
     "`MVCP`/`MVCS`" as "`MVCK`".** Confirmed empirically (a one-instruction probe) in
     **Stage-0b**, which will append the definitive result here.
  2. **`RENT` is CONDITIONAL — do not read it as unqualified.** The SVC routine's `RENT`
     claim holds for the **single-client-sequential probe** because its register-
     preservation scratch is the *shared* CSA anchor `csasave` (Decision §5; only one
     invocation is ever in flight). Under **concurrent clients from multiple address
     spaces** (M5-2), that shared scratch is a data race: each invocation then needs
     **per-invocation storage in the SVRB** (the natural per-request work area, `R5`),
     not the anchor. The probe does not exercise concurrency; M5-2 must switch the scratch
     to the SVRB before the routine is entered concurrently. Recorded so it is not hidden
     behind a bare "`RENT` ✓".
