# ADR-0049 — Recovery releases only what nobody else will

**Status:** Proposed (M5, #83). It is accepted when its PR merges; the merge
is the countersign (`docs/adr/README.md` convention 1).
**Date:** 2026-10-15
**Decisions locked by:** Mike, 2026-10-15 (fix shape A + C, D as the backstop)
**Amends:** Architecture Specification §17.1 ("call the **same** teardown the
orderly path uses") and the matching CLAUDE.md §3 recovery rule
**Builds on:** #79 (the recovery quiesce of the SVC slot and the anchor) and
its audit, `docs/measurements/m5-79/audit.md` §1
**Evidence:** `docs/measurements/83-survey/survey.md`,
`docs/measurements/m5-83/round.md`

---

## 1. Context

§17.1 had the ESTAE exit call `nsf_shutdown()`, "the **same** teardown the
orderly path uses". It did not. The orderly path quiesces the devices first:
the CTCI subtasks CLOSE, which purges their EXCPs, and are joined. Recovery
did not quiesce them and went straight to `mm_shutdown()`.

`mm_shutdown()` FREEMAINs every pool region. One of them, CTCIBUF, holds the
buffer of the READ the subtask keeps parked on the CTCI device. EXCP fixes
those pages for the duration of the I/O, so the FREEMAIN abends **A0A
reason 2, "PART OF AREA BEING FREED IS STILL FIXED"** (IEAVGM00).

The result: every recovery with the CTCI pair up ended in a second abend
inside the exit, and `NSF901I` never appeared. Reproduced on today's code
(STC00726, `IEA700I A0A- 2 ... 00 0000A040`); the length is the CTCIBUF region
after libc370's rounding.

Running the orderly sequence inside the exit is not the answer either.
Quiescing a device is OPEN/CLOSE/EXCP and a bounded join. That is blocking
device work in a damaged environment, and #79's `nsfsx_recover_quiesce`
already refuses to drain for the same reason.

## 2. Decision

**The recovery path releases only what nobody else will release.** #79's
audit drew that line; this ADR makes it the rule for the whole exit.

| Resource | Who releases it | Recovery |
|---|---|---|
| SVC slot (nucleus), CSA anchor and router (common storage) | nobody but NSF | restore the slot, mark the anchor inactive (#79). Draining and freeing are left to the next start's adoption (ADR-0048) |
| pool regions, I/O subtasks, SVC 99 allocations, the OUCB (private / address-space scoped) | MVS, at address-space end, after it has purged the I/O | **left alone**. `nsf_recover_shutdown()` disarms the STIMER exit and frees nothing |

Two further rules come from the same finding.

- **The startup failure after the devices started quiesces them first.** It
  runs on mainline, where a join is allowed. That is the orderly path, and it
  now follows the orderly sequence (`src/nsfsmain.c`, the `nsfsx_start`
  failure).
- **`nsf_shutdown()` never frees under possible I/O.** The mains register an
  I/O guard (`dev_busy_count`).
  - `dev_busy_count` counts every device not DOWN. It also counts every DOWN
    device whose driver kept its private block, which is the CTCI driver's
    `NSF206W` case when a subtask will not join.
  - If the count is not zero, no region is released, the count is returned,
    and the caller reports `NSF068W`. Skipping the free is a third state and
    is said out loud (CLAUDE.md §8.5).

## 3. Consequences

- Recovery now completes with the devices up: `NSF901I`, then the original
  abend percolates (STC00730, `S0C4`).
- **No teardown step added later to `nsf_shutdown()` runs in recovery.** That
  was already true in effect, because the exit abended before reaching it.
  Now it is the stated rule. A step that releases something nobody else will
  must go into the recovery path explicitly, as #79's quiesce did.
- §17.1's rationale stands: secondary failures at termination are harder to
  debug than the primary failure. The A0A was exactly such a secondary
  failure, and it replaced the original abend code in `IEF450I`.
- The pool regions are not freed in recovery. That costs nothing, because MVS
  reclaims the private area as the address space ends (#79 audit §1).

## 4. Not established

- That the fixed pages are the READ buffer. Length and TCB match; the address
  was not compared.
- The Phase-1 recovery (`src/nsfmain.c`) and the `NSF206W` → `NSF068W` path
  on MVS. Both are source- and host-pinned only.
