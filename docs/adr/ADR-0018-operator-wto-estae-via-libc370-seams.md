# ADR-0018 — Operator interface, WTO and ESTAE recovery reuse libc370 seams (no hand-rolled asm)

**Status:** Accepted (2026-07-08); implemented at M0-8, host-green + cross-link
clean. Ratified with the maintainer (the ESTAE choice).
**Relates to:** §5 (NSFEVT main loop / operator command flow), §17 (Recovery &
Serviceability), ADR-0006 (ESTAE + teardown from M0), ADR-0007 (prefix NSF),
CLAUDE.md §3 ("C-callable HLASM" / issue #8) and §6 (repo layout).

## Context

M0-8 assembles the foundation into a real started task: a `S NSF` STC that reads
its PROFILE, runs the executive loop, answers `F NSF,*` / `P NSF`, and recovers
under ESTAE. Three MVS-service surfaces are needed: **operator commands**
(EXTRACT COMM / QEDIT + CIB), **WTO** for the `NSFnnns` messages, and an **ESTAE**
recovery routine that captures first-failure data and tears down cleanly.

The Project Brief / spec §17.1 and ADR-0006 name `asm/nsfestae.asm`, and
CLAUDE.md §6 also listed `asm/nsfwto.asm`, written before it was established that
**libc370 already provides all three seams**, proven in the ecosystem STCs:

- `__gtcom` / `__cibset` / `__cibget` / `__cibdel` — the CIB/QEDIT operator seam
  (used by `ufsd`, `httpd`).
- `wto` / `wtof` / `vwtof` — WTO with printf-style formatting (used everywhere).
- `__estae(op, fp, udata)` — an ESTAE whose RECOVERY glue **GETMAINs a stack,
  chains save areas and BALRs into a C recovery function** with the SDWA
  (`ufsd_recover`, httpd `try`/`abendrpt`), the recovery percolating via
  `sdwa->SDWARCDE = SDWACWT`.

## Decision

**Reuse the libc370 seams for all three; add no new NSF assembler at M0-8.**

- **CIB/QEDIT** behind a thin seam (`src/nsfopr_plat.c` on MVS, a host
  injectable-queue shim). The portable command router (`nsfopr_dispatch`) is
  separate and host-tested; the seam only delivers the string.
- **WTO** behind one C choke point `nsfmsg()` (`src/nsfmsg.c` formats;
  `src/nsfmsg_plat.c` emits via `wto`, a host capture ring for tests).
- **ESTAE** via `__estae(ESTAE_CREATE, nsf_recover, NULL)`; `nsf_recover` is a
  **C** function that WTOs a marker, calls the same `nsf_shutdown` teardown the
  orderly path uses (§17.1), and percolates with `SDWARCDE = SDWACWT`. The ESTAE
  is deleted **first** in shutdown so a fault while freeing pools cannot re-enter
  recovery.

So `asm/nsfestae.asm` and `asm/nsfwto.asm` are **not created**; §17.1, ADR-0006
and CLAUDE.md §6 are amended to point at the libc370 seams.

Rationale (the tie-breaker):
- **§17.1's own contract is "recovery calls the same *C* teardown functions the
  orderly path uses."** Reaching C teardown from a *raw asm* OS-exit means
  re-implementing exactly libc370 `@@estae`'s RECOVERY bridge — GETMAIN a stack,
  chain save areas, BALR into C — which is precisely the **C-runtime-from-exit
  hand-roll that issue #8 proved breaks on MVS** (S0C6, host-clean). The
  `nsf_now` precedent cited for "write your own asm modeled on the reference"
  does not transfer: `@@getclk` is a trivial `STCK` leaf, whereas `@@estae`
  *is* the C bridge. Using it honours the issue-#8 lesson rather than routing
  around the deliverable.
- **Prefer libc370 / in-repo code** (CLAUDE.md, "Dependencies are permanent"):
  these seams are battle-tested by `ufsd`/`httpd`.
- **It pays forward.** M1 device-quiesce and later teardown become plain C calls
  on the already-established recovery path — mirroring ADR-0017's argument for
  the async STIMER exit convention.

Rejected — a from-scratch `asm/nsfestae.asm` + `asm/nsfwto.asm`: re-derives the
libc370 C-bridge (issue-#8 risk), MVS-runtime-only to validate, and buys nothing
over the proven seams. The one genuinely-NSF async exit (`NSFTMEXP`, ADR-0017)
already exists; M0-8 legitimately adds **zero** new assembler.

## Two CIB stumbling blocks baked in (from ufsd)

1. **Drain CIBs unconditionally every loop pass — never gate on the console ECB
   bit.** MVS can queue the startup CIB (CIBSTART) *without* POSTing the ECB;
   gating would hold the single CIB slot and reject every later MODIFY with
   `IEE342I TASK BUSY`. The loop calls the operator drain each pass regardless of
   the bit (`evt_set_operator` + the drain in `evt_mainloop`).
2. **Delete the ESTAE first in shutdown** (clean and emergency paths) so teardown
   errors do not re-enter recovery.

## Consequences

- New portable, host-tested code: `nsfopr_dispatch` (TSTOPR), the NSFCFG→init
  wiring `nsf_init_from_cfg` (TSTSTC), the `nsfmsg` formatter, and the
  `evt_set_operator` cibECB slot in the §5.3 loop. Host suite 354 → 408.
- New MVS-only glue: `src/nsfmain.c` (the STC `main` + `nsf_recover`), the CIB
  and WTO `*_plat.c` seams. Cross-compiles + links clean (cc370/as370/ld370);
  alias scan clean.
- The `S NSF`/`F`/`P`/induced-ABEND-percolate operator flow is validated by the
  PROC-driven run (`jcl/NSFPROC.jcl`); `test/mvs/tststcm.c` is the automatable
  MVS component check (config→init + ESTAE establish/delete + pool reclaim).
- `NSFRQE` is untouched; this ADR changes no wire/request contract.
