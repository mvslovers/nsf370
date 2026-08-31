# ADR-0038 — Phase-2 app↔stack transport: a private (dynamically installed) SVC

**Status:** Accepted (2026-07-21). **Supersedes the transport decision of ADR-0036**
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
- **2026-07-22 — `MVCK` promise CLOSED (Stage-0b Step 1, live on MVSCE; `TSTMVCK`
  batch+TSO CC 0, 12/12).** The definitive result the correction above promised:
  1. **`MVCK` exists and executes** on MVS 3.8j / Hercules (no `S0C1`), and a
     supervisor/key-0 `MVCK` reading a key-8 source under `R3`=8 (the SVC routine's
     write-in) copies **byte-exact** and honours the length. **Stage-0b uses `MVCK`** for
     the `ubuf` app↔staging copies (ADR-0039), not the key-0 `memcpy` fallback.
  2. **`MVCK` decodes and enforces `R3`** (it is not a silent clobber): a foreign source
     key (`R3`=0, the master key, from problem state) is a privileged operation and
     **faults `S0C2`** — proving the key field is used, and that an unauthorized caller
     cannot misuse `MVCK` to read under an arbitrary key.
  3. **Toolchain finding:** `as370` **mis-assembles the `MVCK` mnemonic** — it treats the
     first operand as an SS-format length-immediate (like `MVC`) and drops the `R1`
     length-register and `R3` key-register (encoding both as 0, verified in the listing).
     `MVCK` must be emitted as a **raw `D9` opcode** with the register fields
     (`D9 R1R3 B1D1 B2D2`). Recorded for M5-2's use of the same seam.
  4. **Supervisor storage-key protection `S0C4` — demonstrated live** (Mike's call). A
     key-8 `MVCK`-read (`R3`=8) of a genuinely **fetch-protected** frame takes a
     **protection exception (`S0C4`)** — the real hostile-pointer case, not a silent
     key-0 clobber. Because `SSK` under MVS DAT sets **real-frame** keys, the probe `LRA`-
     translates the virtual test page to its real address, `SSK`s it to key 0 +
     fetch-protect (`ISK` confirms `keyback=08`), then the supervisor `MVCK` faults
     `S0C4`. So the write-in read genuinely enforces the caller's key at the hardware
     level; M5-2 still owns the *recovery* (turning that `S0C4` into a clean `EFAULT`) and
     address/length validation (ADR-0039). **The `MVCK` seam is fully proven.**
  2. **`RENT` is CONDITIONAL — do not read it as unqualified.** The SVC routine's `RENT`
     claim holds for the **single-client-sequential probe** because its register-
     preservation scratch is the *shared* CSA anchor `csasave` (Decision §5; only one
     invocation is ever in flight). Under **concurrent clients from multiple address
     spaces** (M5-2), that shared scratch is a data race: each invocation then needs
     **per-invocation storage in the SVRB** (the natural per-request work area, `R5`),
     not the anchor. The probe does not exercise concurrency; M5-2 must switch the scratch
     to the SVRB before the routine is entered concurrently. Recorded so it is not hidden
     behind a bare "`RENT` ✓".
- **2026-08-25 — M5-2b2: the POST save area is per-invocation; `csasave` retired.**
  This closes the RENT/shared-scratch caveat this ADR has carried since Stage-0a′:
  *"Single-client-sequential, so the shared scratch is safe here; a concurrent-client
  M5-2 needs per-invocation scratch (the SVRB / GETMAIN)."* It is now the SVRB, with no
  lock, no pool and no `GETMAIN`. The **pool** is still (b3); b2 moved the storage and
  proved the home, b3 is what will exercise it.

  ### Where the area is — read, not guessed

  `SYS1.AMODGEN(IHARB)` and `(IKJTCB)` read live, and the offsets **computed by IFOX00
  from the macros themselves** (jobs `RBOFF`, `RBOFF2`) rather than hand-counted — these
  DSECTs are conditional-assembly, and a wrong offset in a RENT routine with no writable
  statics has no cheap failure mode:

  | field | value |
  |---|---|
  | `RBEXSAVE` — *"EXTENDED SAVE AREA FOR SVC ROUTINES (SVRB-BOTH)"* | `RBSECPTR+X'60'`, `L'` = `X'30'` |
  | `RBBASIC - RBPRFX` | `X'40'` (the prefix precedes the pointer) |
  | `RBSIZE - RBBASIC` | 8 |
  | `TCBRBP - TCB` | 0 |

  **Twelve words, not eighteen.** The old shared block was an 18-word standard save area
  and `STM R14,R12,12(R13)` wants 72 bytes — 24 more than exist. Not made to fit: this is
  a documented area with a documented size, and running past its end is precisely the
  *"an area that merely looks unused"* class. Only **four** registers are live across the
  POST — `R2` (anchor), **`R6` (the CSECT base, `USING NSFVSVC,R6`)**, `R8`, `R14` — so
  four stores of **16 bytes** replace an `STM` of eleven. Deliberately minimal: the less
  of the area is touched, the smaller the question of who owns the rest.

  ### The bug that cost the first attempt — and it is b1's lesson, one register over

  **`A(SVRB)` must come from `TCBRBP`, not from `R5`.** The FLIH does set `R5 = A(SVRB)`
  for SVC types 2/3/4 — this routine's entry block and the ancestor both say so — but
  **`R5` is the `MVCK` source pointer in `RQEIN` and `XFERIN`**, so by the time `DOPOST`
  is reached it holds `A(caller ubuf)` or `A(caller NSFRQE)`. This is exactly the
  `PSATOLD`-over-`R4` finding from M5-2b1, one register over, and the same rule catches
  both: **at `DOPOST`/`RQEOUT`, a register the FLIH set at entry is only trustworthy if
  no staging block has since used it as scratch.**

  Measured: `R5 = X'000991EC'` (the caller's image) against `TCBRBP = X'009DE5F0'` (LSQA),
  and `RBSIZE` read off `TCBRBP` is **28 doublewords = 224 bytes** — a sensible RB — while
  off `R5` it read `X'FFFFD9D8'`, garbage.

  The first attempt therefore put the save area **inside the client's own storage**
  (`A(caller image)+96`) and stored the caller ASCB over the test program's variables,
  including its anchor pointer — which is why the diagnostic read back `C1E2C3C2`, EBCDIC
  `"ASCB"`. Every gate in the set stayed green throughout.

  ### The prediction, recorded before the run, was REFUTED

  Predicted (and independently hypothesised at review): that `RBEXSAVE` would be usable
  while the routine runs but **not across a suspension** — "available to SVC routines" not
  meaning "preserved across a wait", with `RBGRSAVE` sitting immediately before it
  (32 + 64 = 96) being what that neighbourhood looks like.

  **It did not happen.** A canary stamped in the area reads back intact at all three
  points: **before the POST, after the branch POST, and after the WAIT** — the last
  spanning a real task switch, the STC's cross-AS POST and an `SVC 1`. Neither axis killed
  it; the single cause was the clobbered `R5`. Recorded because a refuted prediction is
  worth as much as a confirmed one, and because the neighbourhood argument is plausible
  enough that someone will raise it again.

  ### The positive check is permanent, not a one-off

  Every existing gate runs through `DOPOST`, so a wrong area MVS happens not to use would
  pass all of them — which is not theoretical, as above. The routine therefore records its
  own self-check into the (now dead) `ANCSAVE` words and **`TSTRQXF` asserts them**: the
  area address is **outside the anchor** *and* **outside the client's own storage** (the
  first attempt's failure mode was neither shared CSA nor per-invocation, and no
  "outside the anchor" test would have caught it), the stamp took, the sentinel survived
  the POST, the sentinel survived the WAIT, and the **register half** — reaching past the
  post-WAIT eyecatcher check proves the restored `R2` still addresses the anchor *and* the
  restored `R6` still resolves the literal pool, so the 16 bytes actually stored into came
  back rather than only the untouched tail.

  ### `ANCSAVE` stays, dead

  Removing it would shift every field after it and cost a full four-gate Stage-0 round for
  no benefit; b3 moves the layout substantially anyway, so it comes out there for free.
  The anchor layout does not move in this step and no `NSFV_OFF_ASSERT` changes.

  ### The ancestor's contribution, stated rather than left as silence

  `igc0024e.asm` saves no registers anywhere and never dereferences `R5`, though it lists
  `R5 = @ SVRB` in its register table. It therefore **corroborates the entry convention and
  says nothing about the save area** — it neither supports nor undermines `RBEXSAVE`.

  ### Gates

  `TSTRQXF` **68 PASS, CC 0 batch + TSO** (now including the three-point check);
  `TSTRQXM` **batch CC 0, 32/32** with the host peer verifying **9353 bytes byte-exact**;
  `TSTSVC` / `TSTMVCK` / `TSTUBUF` / `TSTDEATH` **444 PASS, 0 FAIL, CC 0 batch + TSO`.
  NSFS and NSFV start and stop clean, `SVC 239` restored, **no dump**. Host suite
  **2846 PASS / 0 FAIL**. `as370 -a=` listing checked for the changed block.

  **A column-71 overrun was caught by the scan during this step** and is worth recording as
  a live sighting of the CLAUDE.md §3 rule: a 73-character instruction line made `as370`
  swallow the following instruction as a continuation, emitting
  `ST R14,0(,R9)R2,4(,R9)` — one store silently gone, clean link, no diagnostic.
- **2026-08-25 — M5-2b2 follow-up: the self-check is EVIDENCE, not a runtime guard, and it
  is one-sided.** No mechanism change; three states on the self-check words, an explicit
  register table in the entry-convention block, and this note.

  **The post-restore self-check stores cannot report the failure they would most want to.**
  `ANCSAVE+8` (sentinel after the POST) and `ANCSAVE+12` (sentinel after the WAIT) both
  execute *after* `R2` has been reloaded from the save area. If that reload were broken,
  `R2` is garbage and `ST R3,ANCSAVE+8(,R2)` does not record a 0 — it stores into
  **arbitrary key-0 storage**. So those two words can prove success and are structurally
  incapable of reporting that specific failure.

  This is **inherent, and deliberately not fixed**: only `R9` survives the branch POST, and
  the anchor cannot be re-derived from it. Re-deriving it through `R6` would not help either
  — `R6` comes out of the same reload, so a broken reload takes the diagnostic with it.
  (That is not hypothetical; it is exactly what the first attempt at this step did, and the
  wild stores that followed are why the diagnostic rule is now "trusted registers only".)

  **The note the brief asked for, with one correction of scope:** the observation was made
  about `+8`, and it applies equally to `+12` — both are post-reload. It does **not** apply
  to `+16`: that store sits behind the post-WAIT `CLC ANCEYE(8,R2)`, which needs a good
  `R2` *and* a good `R6` to compare correctly, so reaching it is itself the proof and a
  broken reload diverts to `WGONE` instead. `+0` and `+4` are pre-POST, where `R2` is
  trusted.

  **So read the five words for what they are:** evidence, collected once, that the SVRB
  home is real — not a guard that will catch a future regression in the reload. What catches
  that is the gate set, all of which runs through `DOPOST`.

  **Three states, because two were not enough** (M5-2b1's CC-20 contract applied to the
  routine's own evidence): **1** = the check ran and passed, **2** = it ran and failed,
  **0** = it was never written. With a 1/0 pair a word that was never reached reads exactly
  like one that ran and failed, and those are different faults with different next steps —
  "the sentinel was dead" points at the save area, "control never got there" points at the
  path. `TSTRQXF` (C) prints the decoded state next to each value so `1/2/1/1` and `1/0/1/1`
  are distinguishable in the spool, not just red in both cases.

  **The entry-convention block now carries a register TABLE, not prose.** Both registers the
  ancestor's convention names as useful are destroyed before `DOPOST` is reached — `R4`
  (A(TCB)) and `R5` (A(SVRB)), each used as an `MVCK` pointer in `RQEIN`/`XFERIN` — and
  between them they exhaust the registers anyone would reach for. That is not two
  coincidences: it cost M5-2b1 one debugging round (`R4`, the `TCBPKF` read) and M5-2b2
  another (`R5`, the save-area address). The rule and the trustworthy sources (`PSATOLD` for
  the TCB, `TCBRBP` off it for the RB) are now in one table rather than split across two
  comment blocks, which is how it was missed twice.

  **`ANCSAVE` dies in b3, and the self-check dies with it.** b3 moves the anchor layout
  substantially — which is exactly why the field was left dead in place rather than removed
  here — so when it goes, these five words and `TSTRQXF` (C) go with it. Filed as a b3
  prerequisite rather than left to be discovered when (C) starts failing.

---

## Addendum (2026-08-25, M5-2b3) — the RENT / shared-scratch caveat is fully retired

This ADR shipped with a caveat: the routine is RENT and holds no writable statics, but it
*did* use two shared writable regions in the CSA anchor — an 18-word save area (`csasave`)
and a 2048-byte staging buffer — both of which every invocation in every address space
computed the same address for. The probe's single-client-sequential model made that safe, and
the caveat said so explicitly, naming a concurrent-client pool as the thing that would have to
resolve it.

It is resolved, in two steps:

- **M5-2b2** moved the save area into the **SVRB's own `RBEXSAVE`**, which the FLIH allocates
  per SVC invocation — no lock, no pool, no `GETMAIN`. `csasave` stayed in the anchor only
  because removing it would have shifted every later field.
- **M5-2b3** (ADR-0042) makes the staging buffer **per slot**, in a 64-slot pool, and removes
  `csasave` outright with the layout move. Two clients in two address spaces now share nothing
  writable but the claim discipline itself: `inflight`, `served`, `reaped` and `exhausted` are
  counters updated by interlocked instructions, and every byte a request actually owns lives
  in the slot it claimed.

**The save-area self-check goes with it (issue #61).** The five words this routine wrote into
the dead `csasave` were one-time evidence that `RBEXSAVE` is a genuine per-invocation area —
including the measured fact that a canary in it survives both the branch POST and the WAIT,
which *refuted* the prediction that it would be usable while the routine ran but not across a
suspension. That evidence is recorded here, permanently, which is what makes it safe to stop
paying five stores and three `CLC`s per request to keep re-establishing it. `TSTRQXF` part (C)
was converted rather than deleted, so the same shape of positive check now covers the pool.

**What has NOT changed:** `MOVEOUT` is still the only block in the routine that runs under a
PSW key other than 0 (ADR-0039 / M5-2b1). The claim loop is deliberately *not* wrapped in a
key window — a claim needs an interlocked compare, which is serialisation, not protection.
