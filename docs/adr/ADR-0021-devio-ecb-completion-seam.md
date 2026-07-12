# ADR-0021 — DEVIO: an optional ECB-completion I/O seam on NETDEV

**Status:** Accepted (2026-07-12) — **and superseded the same day by ADR-0022**
after live STC integration disproved its central premise (the executive waiting
on the driver's raw IOB ECBs). Recorded append-only because the code carrying the
seam (the `DEVIO` vtable, `NETDEV.io`, `dev_set_io`, and the CTCI driver's use of
it) was committed at M1-4, and because the design is a coherent option that a
future driver style might revisit.
**Relates to:** §9.2 (DEVOPS / NETDEV), §5.3 (executive loop), ADR-0019, ADR-0022,
M1-2 (the doneq model this was an alternative to). **Superseded by:** ADR-0022.

## Context

ADR-0019 decided the CTCI driver completes via the IOB ECB (no appendage, no
subtask) and that the §5.3 executive loop WAITs on that ECB directly. NSFDEV's
M1-2 loop seam, however, modelled exactly one completion style: a single
`dev->ecb` plus a `doneq` an async producer pushes to (NSFHOST's reader thread).
CTCI needs **two** completion ECBs (read + write), demux by which posted, a
re-drive-before-parse read path, and one-outstanding WRITE draining — none of
which fits "one `dev->ecb` + drain `doneq`", and ADR-0019 says `doneq` stays empty
for CTCI.

## Decision (as implemented at M1-4)

Add an **optional** per-device vtable `DEVIO { collect, service, kick }` referenced
by a new `NETDEV.io` pointer (NULL → the default M1-2 doneq/`dev->ecb` model, kept
for NSFHOST). It is the per-device mirror of the loop's three global device hooks:
NSFDEV's `collect_ecbs` / `poll_input` / `kick_output` consult `dev->io` and, when
present, use it instead of the default:
- `collect` — contribute the driver's own completion ECB(s) (CTCI: `recb`+`wecb`)
  to the executive ECBLIST;
- `service` — demux the posted ECB(s) each pass (CTCI: read completion → re-drive
  then decode; write completion → free the in-flight PBUF);
- `kick` — start one outstanding I/O from the sendq (CTCI: one WRITE).

NSFEVT stays decoupled (it calls the three global hooks); NSFDEV stays
driver-agnostic (it calls through the `dev->io` pointers a driver installs). The
CTCI driver attaches `&g_ctci_io` in `ops->init` via `dev_set_io`. `NETDEV` grew
64 → 68 bytes for the `io` pointer.

## Why it was superseded (ADR-0022)

The `collect` step puts the driver's **raw IOB ECBs** (`recb`/`wecb`) into the
executive's multi-ECB `WAIT ECBLIST` — the ADR-0019 premise. Host tests and an
isolated single-ECB live probe (TSTCTCM) passed, but the **production STC hangs
the operator the moment IOS posts `recb` asynchronously** (issue #18, bisected
live). The executive must not WAIT on a foreign-posted I/O ECB; ADR-0022 reverses
this and restores the M1-2 `doneq` model behind an I/O subtask.

**What survives:** the *idea* of a per-device completion seam is fine — but its
`service`/`kick` become consumers of the `doneq` handoff (fed by the subtask),
not holders of `recb`/`wecb` in the executive WAIT. Whether the seam keeps the
`DEVIO` shape or folds back into the M1-2 hooks is an M1-4b (issue #18) decision.

## Consequences

- The `DEVIO` code is committed but its `recb`/`wecb`-in-WAIT usage is **known
  broken for the STC** and must not be treated as the completion architecture.
  See ADR-0022 and issue #18.
- `NETDEV` is 68 bytes (`NSF_SIZE_ASSERT(NETDEV, 68)`); §9.2 documents the `io`
  field.

## Sources

- §9.2 (DEVOPS/NETDEV/doneq), M1-2 (the doneq model), ADR-0019 (the IOB-ECB
  premise), ADR-0022 (the reversal), issue #18 (M1-4b).
