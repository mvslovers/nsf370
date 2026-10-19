# ADR-0015 — NSFMM acquires pool regions via libc370 malloc, not a raw GETMAIN SVC

**Status:** Accepted (2026-07-06)
**Refines:** Architecture Specification §2.3 (Memory Manager region acquisition)

## Context

The Memory Manager owns all storage: it acquires one contiguous region per
pool at initialization (spec §2.1, §2.3) and never again. Spec §2.3 states the
intended path as "the MVS build uses GETMAIN (subpool 0, private area)", with
the host build using one `malloc`, "same code above the two-line acquisition
primitive."

Implementing this at M0-2 forced an explicit choice for how the MVS build
issues that GETMAIN:

1. A **raw GETMAIN SVC** in a small assembler helper (`asm/nsf*.asm`) exposing
   a C-callable `region_get()/region_free()`, as rexx370 does for its ENVBLOCK
   when it needs subpool-controlled storage.
2. **libc370 `malloc`/`free`**, called from portable C, as mvsMF does for all
   of its buffers.

Relevant facts, verified against the ecosystem rather than assumed:

- `NSFMM` lives in `src/nsfmm.c` — the **portable** source set. The flat layout
  (ADR-0014) reserves `asm/*.asm` for genuinely MVS-only code. Option 1 would
  move a piece of the Memory Manager into assembler.
- libc370 provides `malloc`/`free` (`libc370/include/stdlib.h`); its allocator
  resolves to a real `getmain()` → `GETMAIN` SVC (`libc370/src/clib/getmain.c`,
  `malloc.s`).
- MVS 3.8j is a 24-bit system: **all** storage GETMAINed by libc370 is below
  the 16 MB line regardless of subpool, satisfying the AMODE/RMODE 24
  requirement inherently.
- NSFMM performs exactly one region acquisition per pool, at init only (a
  handful total), never on a hot path — so `malloc` bookkeeping overhead is a
  one-time, negligible cost, and the no-runtime-allocation invariant (spec
  Goal 1/2) is untouched.

## Decision

**NSFMM acquires each pool region with libc370 `malloc` and releases it with
`free`, from portable C.** On MVS 3.8j this resolves to `GETMAIN` below the
16 MB line; `mm_shutdown` releases every region via `free`. The host build uses
the identical calls. This is the single storage seam in the whole stack
(`mm_region_get`/`mm_region_free` in `nsfmm.c`); no other component touches a
storage service.

This realizes spec §2.3's intent ("GETMAIN on MVS, malloc on host, same code
above the primitive") through a single primitive on both worlds, rather than
deviating from it.

## Consequences

- **NSFMM stays pure portable C — no assembler region helper.** This is the
  load-bearing consequence: a future reader must not "restore" a raw `GETMAIN`
  SVC in assembler on the belief that §2.3 mandates one. If a specific subpool
  or a bulk `FREEMAIN`-by-subpool at ESTAE time is ever required (e.g. for the
  recovery path in M0-8/ch. 17), that is a deliberate future change with its
  own rationale, not a correction of this one.
- The subpool number is a libc370 implementation detail and is **not** relied
  upon; the guarantee NSF depends on is "below the line, released at
  shutdown", which holds on any 24-bit configuration.
- The leak gate observes region acquisition directly: `mm_region_get`/`_free`
  maintain a live-region counter, asserted back to 0 after `mm_shutdown`
  (`mm_debug_live_regions`, under `NSF_DEBUG`).

## Alternatives rejected

- **Raw GETMAIN SVC via an assembler helper:** would split the Memory Manager
  across `src/` and `asm/`, add an MVS-only CSECT the host build must shim, and
  buy nothing at M0-2 — libc370 already issues the GETMAIN below the line.
  Reconsider only if a concrete subpool/FREEMAIN requirement appears.
