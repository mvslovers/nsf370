# M5-2c2 stage c, part 1 — option E: `TSTDEATH` split by row

`TSTDEATH` stays a **named, isolated Stage-0 probe on NSFV**, reduced to the one row it can
prove there. `NSFV_REQ_ORPHAN` is deleted — the block that kept it alive was the `TSTDEATH`
decision, and that decision is now taken.

**Chosen over retiring the file** because an *incidental* witness still fails when the guard
breaks but does **not name the mechanism** — which is the property the Stage-0 set exists to
provide.

**Nothing closed.** #67, #88, #92 open; obligation #4 still discharged in substance for the
**identity half only** — the remaining scaffolding is c3, after (e).

`NSFV_REQ` layout unmoved (`rsvd_pascb`/`rsvd_pasid` at 28/32, size assert 64), `ANCVERNO` 3,
**`NSFRQE` frozen**, and **`src/nsfreqx.c` / `test/tstreqx.c` untouched** — the host pinning
is the floor under every row and is not part of this restructuring.

---

## The coverage table, named in all three places

`docs/adr/ADR-0040`'s annotation, CLAUDE.md's M5 cell, and `test/mvs/tstdeath.c`'s own header
each carry this. It says **"procedure"**, never "test", for row 2 — a coverage kind that is
not named is read as a test a year later.

| row | coverage, and its KIND |
|---|---|
| **1** LIVE | **live, named probe** (`TSTDEATH` on NSFV) + host-pinned |
| **2** DEAD (avail bit) | **operator-driven PROCEDURE at milestone gates — not a test, not in the matrix** (`docs/procedure-row2-client-death.md`) + host-pinned |
| **3** DEAD (ASCB reuse) | **host-pinned only — not live-producible**: 0 of 9 reuses, the ASCB comes back unchanged |
| **4** UNKNOWN | **host-pinned only** — four branches, none live-producible; `ORPHAN` drove one |

**"Not a test" and "not covered" are different things.** Row 2's rig is in the repo and
delivered **6 of 6**; what is missing is the batch form, not the reproducibility.

---

## Verified host-side (survives without a stand)

- `make test-host` **3342 PASS / 0 FAIL**, unchanged.
- **`src/nsfreqx.c` and `test/tstreqx.c` untouched** — verified in the diff. All four rows and
  all four `UNKNOWN` branches keep their host pinning.
- **Layout unchanged**: `rsvd_pascb`/`rsvd_pasid` still at 28/32, `NSF_SIZE_ASSERT(NSFV_REQ,
  64)`, `ANCVERNO` 3. Only the retired constant went.
- **Cross-build clean with no unused-static warnings** on the reduced file — checked
  specifically, because `tstdeath.c` is `host = false` and cc370 is therefore the *only*
  warning source for it.
- **Function code 3 is permanently reserved**, documented where the constant was: removing the
  name did not remove the code, and `asm/nsfvsvc.asm` still rejects `FNORPH` ahead of the slot
  claim. That asm is **not** dead code — it is what stops a retired verb falling through to
  the ECHO default.

## Verified live (MVSCE)

| round | result |
|---|---|
| NSFV **with `TSTDEATH` back** — + `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTXFW` | **438 PASS / 0 FAIL**, CC 0 batch+TSO |
| NSFS — `TSTRQXC`/`TSTRQXF` | **122 PASS / 0 FAIL**, CC 0 batch+TSO |

**438 was predicted before the run**, not reported after it: 13 surviving assertion sites × 2
(batch+TSO) = 26, against the 412 the retirement left. 412 + 26 = 438. A number that moves
without an explanation is the failure this round is written against, so it was forecast.

### The row-2 procedure, executed once from its own text

Every expected output matched:

```
step 1  +TSTAPPD: PARK ARM -- ASCB=00FF96C0 ASID=000C
step 2  NSF813I BUSY=1 BUSYSLOT=0 INFLIGHT=1 ... REAPED=0      <- the conjunction, BEFORE the cancel
step 3  IEF450I TSTAPPDS - ABEND S222
   (*)  rowwatch: entry=80FDB048 -> DEAD-row2-avail            <- independent ASVT witness: row 2, not row 3
step 4  printf 'X' | nc -u -w2 192.168.200.1 7799
step 5  NSF050I CLIENT DEAD (ASCB=00FF96C0 ASID=000C) -- REQUEST REAPED
        NSF813I BUSY=0 BUSYSLOT=-1 INFLIGHT=0 ... REAPED=1
```

The identity in `NSF050I` matches step 1 exactly, and `REAPED` moved 0 → 1 — **the counter is
the proof, the message is the narration.**

**Executing it found one real underspecification, now fixed.** The `TSTAPPD`-in-TESTLIB
prerequisite was written to be discovered by `S TSTAPPDS` drawing `IEA703I 806-4 ... MODULE
ACCESSED TSTAPPD` — **discovery-by-failure**, costing a start, an abend and a restart, and
readable by an operator who did not know the remedy as a broken rig. It is now a **positive
pre-flight check** (`zowe zos-files list all-members`), together with the consequence that
surfaced with it: **`--only` replaces the whole test library**, so `TSTAPPD` and the Stage-0
set cannot be resident at the same time. That discovery is the value of running a procedure
from its own text rather than from shell history.

**Zero dumps**; both STCs start and stop clean; `SVC 239` stolen and restored; no `NSF054W`;
stand left with nothing running.

---

## One asymmetry, recorded rather than hidden

**Row 1's failure mode is a HANG, not a clean FAIL.** A misclassified live client is never
POSTed, so `TSTDEATH` blocks in `WAIT` and its SYSPRINT is lost to the S222 — it cannot report
a failed assertion because it never regains control. The file therefore brackets its blocking
call with **console markers**, which survive a hang, and its header states which failure mode
is which: if the last `TSTDEATH` line is the "ISSUING THE BLOCKING ECHO" marker, the guard did
not answer LIVE, and `NSFV050I` / `NSFV051W` in the same log says which way it went.

A clean-but-wrong verdict is still caught as a normal FAIL: the round trip asserts `reaped`
unchanged, so a reap under a live client moves a counter and fails the assertion.
