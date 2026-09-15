# M5-2e Job B — STOPPED AT P0. No measurement was taken.

**Date:** 2026-09-15 · **Stand:** MVSCE-DEV on mvsdev, freshly rebooted
**Predictions:** `predictions.md`, written before `S NSFS` and **not edited**.
**Proof kind:** none — **this round produced no measurement**, by design.

**`NSF055I` read `1187840`, not `1073152`. The locked condition says stop, and
the round stopped.** Nothing was measured, nothing was adjusted, and no run was
attempted.

---

## The gate, and the reading

The condition, as locked:

> `S NSFS`, then read `NSF055I`. Expected: **1073152**. If it is lower, a
> retained anchor is impossible after an IPL, so a different value means
> something else changed — **stop and report, do not proceed and do not
> adjust.**

What `S NSFS` reported (STC started 10:06:18, console log):

```
NSF851I NSFS NON-SWAPPABLE (SYSEVENT DONTSWAP, NDS=1)
NSF040I NSFS 0.1.0-dev STARTING (PHASE 2)
NSF210I CTCI 0500/0501 UP DD SYS00003/SYS00005 MTU 1500
NSF211I INTERFACE LNK1 CUU 0500 UP
NSF055I CSA POOL 137272 BYTES (64 SLOTS X 2144) -- LARGEST FREE BLOCK NOW 1187840
NSF042I SVC 239 STOLEN (EP 00A6E248)
NSF041I NSFS TRANSPORT READY -- ANCHOR=00A6E7C8 ECB=000BE814
NSF001I NSFS INITIALIZATION COMPLETE
```

**1187840 − 1073152 = 114688 bytes = 112 KB MORE free CSA than the baseline.**

**It is higher, not lower.** The condition anticipated *lower* and gave the
reason a lower value could not be CSA debt after an IPL. A **higher** value is
outside what the condition anticipated at all, which is more reason to stop
than less: the round has no entitlement to treat any value but 1073152 as
normal, and `predictions.md` says so in those words before the fact.

## A candidate explanation — NOT established, and not acted on

**Two fewer started tasks are consuming CSA than when the baseline was
recorded.**

| | running |
|---|---|
| now | JES2, NET, TSO, **HTTPD** |
| when `1073152` was recorded (3 Sep, and every earlier reading) | JES2, NET, TSO, **UFSD, FTPD, HTTPD** |

`UFSD` and `FTPD` were **not started after the reboot** — Mike started only
`MVSCE-DEV`'s Hercules, and inside MVS only HTTPD came up. Two absent STCs
freeing ~112 KB of common storage between them is of the right order.

**This is offered as a candidate and nothing was done about it.** It was not
tested, the round did not proceed on the strength of it, and no number was
adjusted to fit it. Confirming it would mean starting `UFSD` and `FTPD`,
restarting NSFS so its `NSF055I` reflects the new CSA state, and reading again
— **which is a change to the stand and Mike's call, not this round's.**

## Why this matters more than it looks

The baseline is not decoration. **`LARGEST FREE BLOCK` before and after every
run is one of the round's discard criteria**, and an `NSF054W` discards a run
outright. A baseline taken under a different set of started tasks would make
every per-run pool reading in this round incomparable with every figure already
in the tree — and the tree's readings (`docs/measurements/40-chk/`,
`40-ident/`, `m5-2e/stage-a.md`) all say 1073152.

**Proceeding would have produced numbers that look like the others and are not
comparable with them.** That is the failure this project has catalogued
repeatedly, and the gate exists to catch exactly it.

## What was and was not done

- **Done:** predictions written and frozen; module currency verified (no commit
  since Job A's deploy touches `src/`, `asm/`, `include/`, `samples/`); TESTLIB
  and `NSF.LINKLIB` confirmed intact after the reboot (`TSTRQXC` present, all
  six modules present); `S NSFS`; the reading; `P NSFS`.
- **Not done:** `MSP`, `MS`, `MA`/`MB` — **no run of any arm was attempted**.
  No swap instrumentation was taken, because there was no run to instrument.

**The stand is left idle and clean:** `P NSFS` gave `NSF043I SVC 239 RESTORED`
→ `NSF044I` → `NSF011I` → `IEF404I`, **no `NSF054W`**; `IEA995I` 0 and
`IEF450I` 0 for the whole IPL, so the zero dumps are "nothing failed" rather
than a suppressed dump. One emulator running, `tun0` still up.

## Instrument note, because it nearly cost the round its tooling

The `zowe` CLI is **broken on the controlling machine** under Node **26.7.0**:
`util.isNullOrUndefined` was removed, and zowe's own logger calls it — so even
`zowe --version` throws **inside the error formatter**, which hides whatever
the real error was. It is driven through a Node 22 wrapper for this round.

**`make deploy` / `make test-mvs` are unaffected** — mbt speaks to mvsMF over
Python `urllib`, not through zowe. Worth recording because the symptom
(everything zowe touches fails with an unrelated-looking TypeError) invites
diagnosing the stand instead of the workstation. **mvsMF answered a direct
`curl` in 67 ms with the expected 401 throughout**, which is what separated the
two.

## What would let this round run

1. **A ruling on the baseline.** Either start `UFSD` and `FTPD`, restart NSFS
   and re-read `NSF055I` — expecting 1073152 if the candidate above is right —
   or re-baseline deliberately, recording the new value and its conditions and
   accepting that it is not comparable with the tree's existing readings.
2. Then the round as locked: `MSP` first as the instrument gate, then three
   valid runs at the 300 s window, with swap instrumentation and per-run pool
   readings.

**Nothing flips to proven, no milestone moves, and no measurement exists to
cite from this round.**
