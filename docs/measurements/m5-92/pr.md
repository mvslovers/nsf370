# #92 — the STATS renderer truncated silently, and the host ring dropped its tail

**Two problems in two layers**, fixed separately and gated separately. A prerequisite for
(e), which measures throughput by reading counters.

Red lines held: no counter removed or renumbered (verified in the diff), anchor layout
unmoved, `ANCVERNO` 3, `NSFRQE` frozen, `asm/nsfvsvc.asm` untouched.

---

## Problem 1 — `F NSFS,STATS` rendered a third of the registry and said nothing

**Mechanism, from source.** `op_stats` (`src/nsfopr.c`) put one `sts_render` into one
`char buf[512]`. `sts_render` **breaks** at the first line that would not fit and returns a
**byte count** — so "rendered everything" and "stopped a third of the way in" are the *same
observation* to the caller. Each line is `"%.8s %.12s %u\n"`, and **it carries the counter's
value**, so the cut point **moves as values gain digits**.

**Measured live on the deployed module, before any change:**

```
NSF810I STATS 52 COUNTER(S)
   ... 33 NSF811I lines ...          <- 19 counters missing, nothing said so
```

**The 19 that were invisible are not a random tail** — they are the *entire UDP counter set*
plus most TCP diagnostics: `NSFUDP in/out/binds/noport/badcksum/badlen/rxfull`, `NSFTCP
rexmit / twreclaim / wndprobe / oooseg / dupack / rxfull / datadrop / …`, and `NSFSX
wakeposts`. Earlier rounds that quoted those figures read them through `sts_value` inside the
tests, not through this report, so **those results stand** — but (e) reads the report.

**The fix, two halves.**

1. **Complete** — `sts_render_from(buf, bufsize, first, *next)` renders from an index and
   reports where it got to; `op_stats` resumes until every counter is out. The buffer now
   bounds one *chunk*, not the whole reply.
2. **Visible** — `op_stats` counts what it emitted against `sts_count()` and emits
   **`NSF818W STATS INCOMPLETE -- RENDERED n OF m`** if they ever differ. This is the durable
   half: without it the next counter added past some future boundary is lost in the same
   silence.

`nsfsx_stats_extra` stays as a *mechanism* but is no longer the *reason* a counter is placed
outside the registry — it was a workaround for this, it worked, and it was not a fix.

## Problem 2 — the host capture ring dropped its tail (host-only)

**Mechanism, from source.** `nsfmsg_emit` stores only while `g_capn < CAP_MAX` but always
increments, so it retains the **first** 64 lines and drops the rest — the **newer** ones, not
the older ones as the old comment claimed. `F NSFS,APPS` at a full registry emits 1 + 64 + 1
= **66** lines, so the last slot *and* the `NSF816I` summary were gone, and
`nsfmsg_cap_line()` returned `NULL` for them — which reads as "no such line", not "dropped".

**Fix:** `CAP_MAX` 64 → 256, **and** `nsfmsg_cap_dropped()`, because a bigger number alone is
*the same defect at a new threshold*. The overflow is now countable, so a test asserts it is
zero instead of inferring completeness from a line count it cannot distinguish from a short
reply.

---

## Verified host-side

- `make test-host` **3342 → 3414 PASS / 0 FAIL** (TSTOPR 34 → 113).
- **Discriminating by construction, and proven:** with 52 counters registered at
  **ten-digit values**, reverting `op_stats` alone gives
  **`FAIL: EVERY counter rendered, at ten-digit values (got 22, want 52)`**.
  **That 22 also measures the moving boundary** — ~33 at live (small) values, **22** at
  ten-digit ones. A fix that worked at small values and failed at large ones would be the
  same defect at a new threshold; this is the case that catches it.
- **The ring gate discriminates too:** reverting `CAP_MAX` alone gives
  **`dropped (got 2, want 0)`** plus the two missing lines — exactly the last slot and the
  `NSF816I` summary.
- **`NSF818W` is proven to FIRE**, not merely present. It is unreachable in production (a
  line is ~33 bytes, the chunk 512), so the chunk size became an `NSF_DEBUG`-only settable —
  because **a warning nobody has seen fire is not designed in, it is asserted**, which is
  this issue's own shape one level up. The test shrinks the chunk, sees
  `NSF818W ... RENDERED 0 OF 52`, restores it, and sees the warning gone and all 52 render.
- Cross-build clean (6 modules + 53 test modules); alias scan **244 unique, all ≤ 8**
  (`NSFSTRNF`, `NSFOPSCH`, `NSFMSGCD` new).
- The cross-build caught a real error mid-work: the gate called `NSF_DEBUG`-only helpers from
  outside the guard — 4 unresolved externals, invisible to the host build.

## Verified live (MVSCE)

**Before/after on the same stand, one assertion moving:**

| | `NSF810I` header | `NSF811I` lines | `NSF818W` | last counter |
|---|---|---|---|---|
| **before** (deployed `main`) | STATS **52** | **33** | absent | `NSFTCP resetsent` |
| **after** (fixed) | STATS **52** | **52** | absent | `NSFSX wakeposts` |

`NSF817I APPSWEEP` still comes last, so the supplement's ordering is unchanged.

| round | result |
|---|---|
| NSFV — `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTXFW`/`TSTDEATH` | **438 PASS / 0 FAIL**, CC 0 batch+TSO |
| NSFS — `TSTRQXC`/`TSTRQXF` | **122 PASS / 0 FAIL**, CC 0 batch+TSO |

**Zero dumps**; both STCs start and stop clean; stand left with nothing running.

Raw console captures: `docs/measurements/m5-92/stats-before.log`, `stats-after.log`.
