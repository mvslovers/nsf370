# libc370 2.x port: live round predictions

Written 2026-10-14, **before any deploy**, and not edited afterwards. The
round record goes in a separate file beside this one.

Build under test: branch `libc370-2x-port`, cc370 1.6.3, libc370 2.11.0,
mbt 3. Stand: dev1 (MVSCE on mvsdev, `mvsdev.lan:8080`).

## What the offline gate already showed, and what it cannot show

The assembler of all 99 MVS TUs matches the 1.x build, except for label
numbering and the documented memset() reload (#425/#427). What no `.s`
comparison can show is a change inside libc370 itself: the C startup, the
thread driver, return codes, stdio locking. Those are what this round is for.

## P1. The deploy is seen to take effect

This port adds no message and no field, so the positive check is P2's printed
code: on the old, 1.x-built TSTCTHR the line reads `rc=0`, because 1.x's
`clib_identify_cthread()` always returned 0. An `rc=4` therefore cannot come
from a stale module.

## P2. `clib_identify_cthread()` returns 4 in a program that links threads

Read from libc370's source (`src/mvs/thread/@@idecth.c`) and CHANGELOG (#427
and 2.3.0 #159), not yet run:
- Since 2.3.0 the C startup IDENTIFYs CTHREAD when `cthread_create()` is
  linked.
- Since #427 the call returns IDENTIFY's real code.

So TSTCTHR prints **`clib_identify_cthread rc=4`** and passes, because it now
accepts 0 or 4.

**Falsified if** it prints `rc=0`. The fix is then harmless, but the
explanation in the commit is wrong and must be corrected. If it prints another
code, stop: that is a different failure.

Consequence for the unfixed code, deduced and not run: `nsfthr_setup()` would
return 4, `dev_start` would fail, and `S NSF` would not bring the CTCI device
up. This round runs only the fixed build, so it does not test that branch.

## P3. NSFV Stage-0 round: 438 PASS / 0 FAIL

`TSTSVC`, `TSTMVCK`, `TSTUBUF`, `TSTDEATH` and `TSTXFW`, batch and TSO, the
figure the record gives for this set. `TSTMVCD` is excluded (#53). Order:
`P NSFS`, `S NSFV`, run, `P NSFV`.

## P4. NSFS round: 130 PASS / 0 FAIL

`TSTRQXC` and `TSTRQXF`, batch and TSO, after `S NSFS`.

## P5. The production device path comes up

`S NSF` shows `NSF210I CTCI 0500/0501 UP ... MTU 1500`, `NSF211I` and
`NSF001I`, and a 20-packet ping gives 20/20. This is the only check that runs
`nsfthr_setup()` through `dev_start` in a production module.

**Precondition:** `tun0` up and devices 0500/0501 online. If `HHC01463E` (no
`tun0`), P5 is reported **not run**, never inferred from P2.

## P6. Deploy behaviour under mbt 3 (a measurement, not a pass/fail)

The CLAUDE.md rule "deploy fails while an STC holds `NSF.LINKLIB`" was
measured on mbt 2. mbt 3's help says it replaces members and never deletes the
library. This round stops the STCs before deploying anyway. Whatever
`mbt deploy` reports is recorded as it is.

## Edited files this round does NOT run

- `test/mvs/tsthio.c`: device-gated, M3-0a probe.
- `test/mvs/tstctcm.c`: device-gated.
- `test/mvs/tsttcp{d,h,r,w}.c`, `tstudpm.c`, `tstezam.c`, `tstezat.c`: need
  the host-side peers.
- `test/tstctci.c`: host-only suite, trivial on MVS.
- `test/mvs/tstd1r.c`: needs d1's rig.

Their only check is the cross-compile with and without their `-D` variants.
