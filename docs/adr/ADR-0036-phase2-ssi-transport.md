# ADR-0036 — Phase-2 app↔stack transport: the MVS Subsystem Interface (SSI)

**Status:** Proposed (2026-07-21). Pins the transport that carries an application's
request across the address-space boundary to the stack in **Phase 2** (the `NSFS`
subsystem STC). Phase 1 is same-address-space (an ATTACHed subtask; `nsfreq_submit`
= `xq_push` + `nsfthr_post`, a real SVC 2 POST on one TCB). Phase 2 puts apps in
their **own** address spaces and the stack in an STC — so the app↔stack hand-off
must cross address spaces. **The `NSFRQE` request block is the phase boundary
(spec §1.3, §10.4, frozen at M3): only the transport under `nsfreq_submit` changes,
never the request format.** This ADR fixes that transport to the MVS **Subsystem
Interface (SSI)**. It is grounded entirely in the UFSD AP-1c cross-AS implementation,
which is proven live on this exact target (MVSCE on Hercules).
**Relates to:** spec §1.3 (the two-phase split), §10.4 (the frozen NSFRQE — the
payload SSI carries in M5-2), §17.3 (security — the cross-AS boundary becomes the
attack surface, handled in M5-2), ADR-0022 (single-task run-to-completion; the
completion-POST seam), ADR-0023 (the `nsfthr` threading seam Phase 1 posts over).
**Evidence pins:** `ufsd/docs/cross-as-reference.md` (five successive abends, each
symptom→root-cause→fix), `ufsd/src/ufsd#ssi.c` (the SSVT router template),
`ufsd/src/ufsd.c` + `ufsd#csa.c` + `ufsd#sct.c` (the STC / CSA-anchor / SSCT
lifecycle), `ufsd/docs/concept.md` §3–4, and the libc370 primitives
`@@xmpost.c` / `@@ascb.c` / `@@super.c` / `@@uinc.c` / `@@apfset.c` / `iefssreq.c`
with the `clibssct.h` / `clibssvt.h` / `iefssobh.h` / `iefjssib.h` interfaces.

---

## Context

Phase 2 needs a **cross-address-space request/response transport**: an application
task in address space A submits a request and blocks; the stack, running in the
`NSFS` STC address space B, picks it up, services it, and wakes the app with a
reply. On S/370 / MVS 3.8j there are **no** cross-memory services (no PC/PT, no
ALESERV) — the primitives are cross-AS `POST` (branch-entry `POST` via `CVT0PT01`),
`WAIT` on a same-AS ECB, and CSA (common storage) for the shared control blocks.

UFSD already solved exactly this problem (its AP-1c "SSI router + first round-trip")
and documented the solution as five successive abends
(`ufsd/docs/cross-as-reference.md`). The mechanics are subtle and
**state/key-dependent**: a wrong combination can *run green once* and still be
wrong, because the failures are timing- and state-dependent. NSF inherits the
solution wholesale rather than re-deriving it.

Two transport candidates were considered:

1. **SSI** — register a subsystem (SSCT/SSVT) whose function routine (the "router")
   MVS dispatches, via `IEFSSREQ`, **in the caller's address space**. The router
   hands the request across to the STC through a CSA anchor and waits for the reply.
2. **A private SVC** — install a user SVC (e.g. 200–255) whose routine performs the
   same hand-off.

## Decision

**The Phase-2 app↔stack transport is the MVS Subsystem Interface: a dynamically
registered SSCT/SSVT whose function routine (the NSF "SSI router") is invoked by
`IEFSSREQ`.** In M5-2, `nsfreq_submit`'s single call site swaps its
`xq_push`+`nsfthr_post` pair for an `IEFSSREQ` call carrying the NSFRQE across;
everything above it (the frozen NSFRQE, the dispatcher, the socket layer) is
unchanged. Concretely, the design NSF adopts **verbatim from UFSD**:

### 1. A thin SSI router, running in the caller's address space

A distinct load module (its entry point IS the router function, built **`RENT`**,
`startup=false`, `ac=1`) is loaded into CSA via `__loadhi` from the STC's steplib
and registered in the SSVT. MVS invokes it, via `IEFSSREQ`, **in the caller's AS**,
supervisor state. It contains **zero protocol logic**. Its contract:

- Capture `R1 = SSOB` by inline asm (`__asm__("LR %0,1")`) **before any C** — the
  SSI convention passes R1 = raw SSOB pointer, not a C parameter list.
- Validate the SSOB + its extension; locate the CSA anchor via
  `ssct_find(name)->ssctsuse`; revalidate the anchor eye-catcher + ACTIVE flag.
- In a key-0 window: take a request slot, `__uinc(anchor->inflight)`, stage the
  request, set `client_ascb = __ascb(0)` and `client_ecb_ptr = &local_ecb` (a
  **key-8 stack-local** ECB in the router's frame), wake the STC with
  `__xmpost(server_ascb, &server_ecb, 0)` **from supervisor state**.
- Back in problem state: a **timed, liveness-checked WAIT** on the key-8 local ECB
  (`ecb_timed_wait`, ~5 s, a sentinel timeout post code distinct from a normal
  reply). On each timeout, revalidate the anchor eye-catcher + ACTIVE and bail
  `CORRUPT` (giving the in-flight count back first) if the server is gone.
- On reply: read the result, `__udec(inflight)` **last** (after every other CSA
  write), return the result via `SSOBRETN`.

### 2. The STC (address space B) owns the anchor and services requests

A CSA anchor (`GETMAIN SP=241`, key 0, eye-catcher) holds `server_ascb` (`__ascb(0)`
at startup), `server_ecb`, `inflight`, and the request slot(s). The STC dynamically
registers the subsystem (`ssvt_new` → `ssct_new` → `ssct_install` into the JESCT
chain), `__loadhi`s the router into CSA and wires it into the SSVT
(`ssvt_set` + `ssvt_funcmap`), then runs its event loop **WAITing in supervisor /
key-0** on `{server_ecb, console-CIB ECB}`. Per wake it drains the request(s),
services each, and wakes the client with `__xmpost(client_ascb, client_ecb_ptr, 0)`.

### 3. The five state/key rules — inherited verbatim (non-negotiable)

| Operation | Mechanism | Required state | Wrong approach → abend |
|---|---|---|---|
| cross-AS POST (both directions) | `__xmpost(ascb, ecb, code)` via `CVT0PT01` | **supervisor (key 0)** | SVC 2 cross-AS from problem state → **S102** |
| POST from supervisor | not SVC 2 | — | SVC 2 from supervisor → **S202** |
| WAIT on the reply ECB | `WAIT` (SVC 1) on a **key-8 stack-local** ECB | **problem state** | key-0 CSA ECB from problem state → **X'201'** / **S047** |
| SSVT router entry | `R1 = SSOB`, captured by inline asm | supervisor | C plist signature dereferencing the raw SSOB → **S0C4** |

Plus the ECB/anchor placement rule (`server_ecb`/`inflight`/request slot in CSA
key-0; the reply ECB is a key-8 **stack local** in the router — never in CSA) and
the SSOB-extension layout rule (a pointer must never land where the SSI path reads
`SSOBINDV+12` — S0C4 abend #2).

### 4. Dynamic SSCT registration — no PARMLIB/IPL

The SSCT is installed **dynamically** into the JESCT chain (`ssct_install`), not
declared in an `IEFSSNxx` PARMLIB member. This is UFSD's proven path and needs no
IPL. *(If dynamic registration ever turns out to require an `IEFSSNxx`/IPL step on
a target, that is an install decision to escalate, not to push through.)*

### 5. Runtime self-authorization — no APF library install

`IEFSSREQ` is an **authorized** branch-entry (the libc370 wrapper issues
`MODESET MODE=SUP`), and the router does key-0 CSA work in the caller's AS
(`__super(PSWKEY0)` → `__isauth()`), so **the calling task must be APF-authorized**.
On this target a task authorizes **itself at runtime** via
`clib_apf_setup` → `__autask()` (**SVC 244**), independent of which library it was
fetched from — verified against `SYS1.PARMLIB(IEAAPF00)`: **NSF.LINKLIB is not in
the APF list, and neither are UFSD's/HTTPD's steplibs**, yet both run authorized on
MVSCE via exactly this SVC. **No library needs an APF (`IEAAPFxx`/IPL) install.**

### 6. Stage the transport before the payload

The transport mechanics are proven **first** on an empty token (Stage-0a, the `NSFP`
**probe** subsystem — distinct from the reserved `NSFS`), so M5-2 (the real NSFEZA
stub carrying the NSFRQE) builds on a validated cross-AS seam. No NSFRQE, socket, or
protocol code is involved in Stage-0a.

## Why

- **UFSD-proven on this exact target.** The five-abend gauntlet is already walked;
  re-deriving it risks re-walking it. All of NSF's needed primitives —
  `__xmpost` / `__ascb` / `__super` / `__uinc` / `__udec`, the SSCT/SSVT API
  (`ssct_new`/`ssct_install`/`ssct_find`, `ssvt_new`/`ssvt_set`/`ssvt_funcmap`),
  `__loadhi`, the `iefssreq` wrapper, and `clib_apf_setup` — **already exist in
  libc370** and are exercised live by UFSD.
- **No SVC-slot install friction.** A private SVC would need a system SVC slot
  installed (a per-target install step, and coordination against other users of the
  slot) for **no capability the SSI path lacks**. The SSI path registers
  dynamically at STC start and deregisters at stop. *(Mike, Q1: SSI over a private
  SVC.)*
- **No PARMLIB/IPL and no APF-library install** (§4, §5): the subsystem is registered
  dynamically and the task self-authorizes at runtime — the probe (and later M5-2)
  deploys and runs without any system install action.

## Consequences

- **The cross-AS boundary becomes the security surface.** A request now originates
  in an untrusted AS; the router copies caller-supplied data into CSA. Request
  validation, ownership (`owner_ascb`), and RACF/authorization checks belong to
  **M5-2** (spec §17.3) — out of scope here, but named now so the Stage-0a probe is
  understood to carry **no** trust boundary yet (it round-trips a token only).
- **ESTAE is mandatory, or the subsystem leaks until IPL.** While the SSCT is
  registered, `S NSFP` fails `IEF612I`; an abend that skips cleanup leaves the SSCT
  registered **until IPL**. The STC's ESTAE exit MUST close the SSVT door (null the
  function entry + clear the anchor ACTIVE flag) and, on a clean stop, deregister the
  SSCT + free CSA — the same destroy path as the orderly stop. On the abend path it
  must **not** free CSA (a foreign PSW may still be inside the router). NSF builds
  the in-flight drain (`__uinc`/`__udec` + a shutdown drain to zero) in **from day
  one**, not as a retrofit.
- **Not host-simulable.** There is no ASCB/SSI/CSA on the host; the transport cannot
  be exercised natively. Host coverage is at most the struct-layout `NSF_SIZE_ASSERT`s
  (SSOB extension, CSA anchor) firing at cc370 cross-compile. **Proof of the
  transport shifts to live MVS** — the probe's gate is a live MVSCE run.
- **OPEN M5-2 question — an unauthorized application cannot use this path as-is.**
  Because `IEFSSREQ` requires an authorized caller (above), the SSI transport as
  designed serves **authorized** clients. UFSD's real clients were always authorized
  STCs (ftpd/httpd link `libufs`). NSF's goal — run existing EZASOKET applications
  **unchanged, relink-only** — includes **unauthorized** apps, which cannot call
  `IEFSSREQ` directly. **How an unauthorized app reaches the stack is deferred to
  M5-2** (candidates: an authorized transition shim the relinked app enters, or a
  distinct entry for the unauthorized path). Recorded here, deliberately unresolved:
  Stage-0a proves the transport with an **authorized probe client** and does not
  settle the unauthorized-app path. *(Mike, Option A.)*
- **Two more load modules on MVS.** The STC and the router are distinct load modules;
  the router (`entry=<router>`, `startup=false`, `ac=1`, `RENT`) is `__loadhi`-loaded
  into CSA from the STC steplib and must carry no writable statics.

## Status / history

- **2026-07-21 — Proposed.** Authored ahead of the Stage-0a SSI probe (`NSFP`) that
  validates the transport mechanics on an empty token. Supersedes nothing.
  Amendments (the unauthorized-app resolution; the NSFRQE keyed move; client-death
  cleanup) will be appended as M5-2/Stage-0b/0c land.
- **2026-07-21 — Transport superseded by ADR-0038** (append-only annotation). The
  Stage-0a `NSFP` probe proved the SSI transport live (520 PASS, no abend, PR #45), but
  its **open M5-2 question** (above) proved decisive: `IEFSSREQ` is authorized-only, so
  the SSI cannot serve NSF's **unauthorized, relink-only** EZASOKET applications. With the
  APF constraint now on the table, the Phase-2 transport is changed to a **dynamically
  installed private SVC** (the APF-free unauthorized→authorized transition) — see
  **ADR-0038**. This annotation supersedes only the **transport mechanism** (§Decision,
  §Why's SVC-vs-SSI trade-off, which was struck before the APF constraint was decisive).
  **Everything else in this ADR remains valid and is reused verbatim by ADR-0038:** the
  cross-AS state/key rules (§3 — the `__xmpost`-from-supervisor and WAIT placement,
  adjusted for a supervisor-throughout SVC routine), the CSA-anchor design (§2), dynamic
  registration's no-IPL principle (§4), runtime self-authorization for the **STC** side
  (§5 — the STC still self-auths via SVC 244 for `__loadhi`/key-0 CSA; only the
  **client** no longer needs it), and the ESTAE / in-flight-drain discipline
  (§Consequences). The `NSFP` SSI probe stays in-repo as the SSI reference/history.
- **2026-07-22 — SSI probe code RETIRED.** With ADR-0038's SVC transport proven live
  (an unauthorized client completing the round trip), the `NSFP` SSI probe is no longer
  carried in the build: `src/nsfp.c`, `src/nsfpssir.c`, `include/nsfpssi.h`,
  `test/mvs/tstssi.c`, `jcl/NSFP.jcl` and their `project.toml` entries are removed. **This
  ADR is the SSI reference** (the design is described in full above); the code remains
  recoverable from git history. The decision to retire (rather than keep the code in-tree)
  is deliberate — the SSI transport is superseded and carrying dead cross-AS code that is
  built/deployed/tested every cycle is a maintenance cost the ADR already covers.
