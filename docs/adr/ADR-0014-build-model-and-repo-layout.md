# ADR-0014 — Build model and repository layout follow MBT V2 ecosystem conventions

**Status:** Accepted (2026-07-05)
**Supersedes (in part):** Architecture Specification §16.1, §16.2

## Context

The Architecture Specification (v1.1) described the host test build as a
standalone `host.mk` that "never invokes MBT", placed HLASM under `src/mvs/`,
and treated `libc370` as a project dependency. Building the M0-1 skeleton
against the real MBT V2 projects in the mvslovers ecosystem (rexx370, mvsMF,
httpd) showed these assumptions diverge from established practice.

Findings from the reference repos:
- `mbt` is a git submodule; the `Makefile` is two lines
  (`MBT_ROOT := mbt` + `include $(MBT_ROOT)/mk/mbt.mk`).
- MBT drives **both** builds. The host build is a first-class target
  (`make test-host`, native compiler) configured by a `[host]` table in
  `project.toml`, including a `replace` map that swaps MVS-only CSECTs for
  host shims.
- `libc370` is the **cc370 sysroot**, provided by the toolchain — rexx370's
  `project.toml` states verbatim that crent370/libc370 is "no longer a
  declared dependency" in v2.
- CI is the reusable workflow `mvslovers/mbt/.github/workflows/build.yml@main`.
- Layout is flat: portable C in `src/*.c`, HLASM in `asm/*.asm`, headers in
  `include/`, tests in `test/` (dual) with `test/mvs/` and `test/asm/`.

## Decision

1. **MBT V2 drives both builds.** No separate `host.mk`. The host build is
   `make test-host` (native compiler) via the `[host]` table; MVS-only code
   is swapped in through `[host].replace`. CI still needs no MVS.
2. **`libc370` is the cc370 sysroot, not a `[dependencies]` entry.**
3. **Flat repository layout** matching the ecosystem: `src/*.c`, `asm/*.asm`
   (not `src/mvs/`), `include/`, `test/` + `test/mvs/` + `test/asm/`. `mbt`
   is a git submodule; the `Makefile` includes `mbt/mk/mbt.mk`.
4. The `nsf*` filename prefix already namespaces every source, so a flat
   `src/` needs no per-layer subdirectories; components stay logically grouped
   by prefix and by the module map in CLAUDE.md §9.

## Consequences

- Spec §16.1/§16.2 are superseded on these points; a short erratum in the Spec
  points here. The frozen "how" narrative otherwise stands.
- `src/host/` and `src/mvs/` as directories are replaced by `asm/` plus the
  `[host].replace` mechanism; host shims live beside their C peers.
- The invariants (no runtime allocation, single-owner buffers, size asserts,
  ESTAE, teardown checklists) are unaffected — they are code rules,
  independent of build layout.

## Alternatives rejected

- **Separate `host.mk` outside MBT** (the Spec's original wording): duplicates
  build logic and diverges from every other mvslovers repo for no benefit.
- **`libc370` as a declared dependency:** incorrect; it is the sysroot.
