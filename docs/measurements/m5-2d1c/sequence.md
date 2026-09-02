# d1c — the stand sequence, written down before touching the stand

The red line: write this down first.  The round before c2 stage c part 1 cost an
`ABEND SFEF` for the lack of it, and #100's round cost a cancelled job, a leaked
slot and a retained anchor.

## Prerequisite, checked POSITIVELY before starting

Both STCs steal `SVC 239`, so only one may run at a time.

```
zowe zos-console issue command "D A,L"          # neither NSFS nor NSFV running?
zowe zos-files list all-members "NSF.TESTLIB"   # is the rig I need actually there?
```

The second is a pre-flight check, not a discovery-by-failure: `make test-mvs
ARGS="--only ..."` REPLACES the whole TESTLIB, so a test from an earlier round is
gone, and its absence draws `IEA703I 806-4`, which looks exactly like a broken
rig (CLAUDE.md 8.5).

## Order

`--only` replaces TESTLIB, so the NSFV set and the NSFS set cannot be resident at
once.  A redeploy sits between the two halves and is scheduled here, not
discovered.

```
1.  P NSFS                                   free SVC 239
2.  make deploy                              (module changes only; P NSFS first)
3.  make test-mvs ARGS="--only TSTSVC --only TSTMVCK --only TSTUBUF
                        --only TSTDEATH --only TSTXFW --only TSTD1R"
4.  S NSFV                                   probe STC: PROBE bit SET
5.  run the NSFV half (2.1, and the NSFV arms of 2.2)
6.  P NSFV
7.  make test-mvs ARGS="--only TSTRQXC --only TSTRQXF --only TSTD1A --only TSTD1B"
8.  S NSFS                                   production STC: PROBE bit CLEAR
9.  run the NSFS half (2.2's ownership arm, the 67 rejection gate)
10. 2.6: P NSFS -> S NSF -> F NSF,APPS -> ping -> P NSF
11. P NSFS / P NSF, leave the stand as found
```

`TSTMVCD` is deliberately excluded from the Stage-0 set (issue #53 makes it fail
for environmental reasons; a re-run in a full-green round erodes confidence).

## READ A RED TSTSVC CORRECTLY — the fail-closed direction looks like a bug

`NSFVSVC` and the STCs are **separate load modules**, and d1c 1 splits across
them: the routine gained the gate, `src/nsfv.c` gained the bit that makes the
gate pass at NSFV.  A deploy that lands the new `NSFVSVC` and leaves an **old
`NSFV`** gives an anchor with the bit CLEAR and a router that tests it, so every
probe verb at NSFV is refused `rc=4`.  That is the documented fail-closed
direction working exactly as designed — but hit by accident mid-round it reads
as "the gate is broken".

**Nothing reports the bit** (`F NSFV,STATS` does not show it), so the check is
behavioural and it is already in the set: **`TSTSVC` drives `ECHO` at NSFV, and
its passing IS the positive check that the bit took.**  Run it FIRST in the NSFV
half.  A red `TSTSVC` means *the bit is clear* — suspect the deploy before the
gate, and confirm with §5's mid-chain `HTTP 500` signature that BOTH modules
landed.

## Debts a run can leave, and how each is paid

* **A claimed slot.** Every 2.1 run `UNSTAGE`s the slot it claimed and asserts it
  went back to FREE with `inflight` 0 (`tstxfw.c`'s pattern).  Without it each run
  costs a slot and eventually forces the retain branch — what #100 paid.
* **A parked client.** With 1 deployed a probe verb at NSFS is refused, not
  parked, so the shape that cost #100 a cancel cannot recur there.  It can still
  arise at NSFV, where the verbs are serviced.
* **A retained anchor + router (~139 KB CSA to IPL).** Only if `P NSFS`/`P NSFV`
  finds `inflight` non-zero.  Check `F NSFS,STATS` before stopping.

## Leaving the stand

`NSF043I SVC 239 RESTORED` from whichever STC ran last, no `NSF054W`, zero dumps
(`IEA995I` count against the deliberate `IEF450I`s as the positive control).
