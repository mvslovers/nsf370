# d2 round 2b: register stamps the tail, live predictions

Written 2026-10-15, **before the 2b deploy**, and not edited afterwards. The
record goes in a separate file beside this one.

Build under test: branch `d2-round2` plus the 2b change (register stamps
`jobname_tail`; `NSF060I` prints it; TSTSCTM `TAIL` role), anchor version 4,
mbt 3, cc370 1.6.3, libc370 2.11.0. Stand: dev1. Issue #136.

Why the round exists: round 2's Q3 was falsified. No production code wrote
`jobname_tail`, so the anchor of STC00710 (A) carries `00000000`. STC00711
read it as FOREIGN (`NSF064W ... NOT OURS`) and allocated a second anchor.
The host red run before the fix is `host-red-tstsct.txt`: 8 failures, `find`
returning 4 (FOREIGN) where 1 (OK) was expected.

State at the start, from round 2: no NSF STC running. A = `00A9C788` is
RETAINED and chained under `nsfs`. It carries LEAK's slot 0 CLAIMED, inflight
1, and its retained router EP E1 = `00A9C208`. `NSF.LINKLIB` holds the round-2
build.

**THE RIG WRITES A's TAIL (step 3).** Every adoption of A in this round
adopts by an identity the test supplied. Only the fresh cycle (Q6) adopts by
an identity the production build wrote.

CSA figures are deltas between readings in this round (#120), never absolute.
X1..X5 name the `NSF055I` "LARGEST FREE BLOCK NOW" readings in order.

## Q0. The deploy took effect

- The deployed `NSF.LINKLIB(NSFS)` contains the EBCDIC string ` TAIL=`. The
  round-2 module does not.
- The TESTLIB's `TSTSCTM` contains `TAIL ANCHOR=`.

## Q1. TSTSCTM GATE (NSFS down)

`mbt test --mvs --only TSTSCTM`, batch and TSO: **33 PASS / 0 FAIL per step,
66 in all.** Round 2 had 30. Arm B gains three assertions:
- register stamped our tail over `ZZZZ` in the private anchor;
- the CSA anchor's tail was `ZZZZ` before register;
- after register it is our tail, written into key-0 CSA.

The arm-B `find` OK on the CSA control is now possible only through the stamp.

## Q2. TAIL: repair A

`TSTSCTT` (`TSTSCTM PARM='TAIL'`), CC 0:
- it prints `TAIL ANCHOR=00A9C788 SERVER_ASCB=<S> INFLIGHT=1 TAIL=00000000`;
- `TAIL: before` gives FOREIGN;
- the store takes;
- `TAIL: after` gives OK;
- it ends with `TAIL=40404040 -- NOW S NSFS`.

Before the start, `<S>` is held against `D A,L` and noted.

## Q3. Adopt A, the REUSE branch (inflight stays 1)

`S NSFS`:
- `NSF061I RENDEZVOUS FOUND A RETAINED ANCHOR=00A9C788 -- ADOPTING`;
- `NSF055I ... 137280 ...` gives X1;
- `NSF062I ADOPTED ANCHOR=00A9C788 -- REAPED=0 POSTED=0 INFLIGHT 1 -> 1`;
- `NSF065W 1 CLIENT(S) STILL IN FLIGHT -- REUSING THE RETAINED SVC ROUTINE
  (EP 00A9C208)`, then `NSF042I SVC 239 STOLEN (EP 00A9C208)`;
- `NSF041I ... ANCHOR=00A9C788`;
- **no** `NSF060I` and no `NSF059W`.

Then `P NSFS` gives `NSF054W 1 CLIENT(S) ...`, and `TSTSCTP` reports
`CHAINED 00A9C788 TAIL=40404040`.

**Falsified if** the start says `NSF064W ... ITS SERVER HAS NOT ENDED`. In
that case STC00710's ASCB is occupied again (STC00711, oftpd and its tests
have run since), A stays, and a fresh anchor is allocated. The round then
**stops** and the question goes to Mike, because the occupant is not ours to
stop. Also falsified by `NSF064W ... NOT OURS`, which would mean the repair
did not hold.

## Q4. Adopt A again, S1 and the FREE branch

1. `TSTSCTD` (`DEAD`) prints `DEAD SLOT 0 PENDING ASCB=00FFFFF8 ASID=1
   INFLIGHT=1 ANCHOR=00A9C788 -- NOW S NSFS`, CC 0. **Synthetic death**, as
   in round 2.
2. `S NSFS`:
   - `NSF061I ... ANCHOR=00A9C788`;
   - X2 within 8 KB of X1, because nothing is allocated;
   - `NSF062I ... REAPED=1 POSTED=0 INFLIGHT 1 -> 0`;
   - `NSF065I RETAINED SVC ROUTINE (EP 00A9C208) FREED`;
   - `NSF042I SVC 239 STOLEN (EP E2)`. E2 may equal E1, so **`NSF065I` is
     the discriminator**;
   - `NSF041I ... ANCHOR=00A9C788`.
3. The NSFS half of the Stage-0 set on the twice-adopted anchor:
   `mbt test --mvs --only TSTSCTM --only TSTRQXC --only TSTRQXF --only
   TSTRQXM --only TSTRQX2`.
   - TSTSCTM 66/0 again;
   - TSTRQXC + TSTRQXF **130 PASS / 0 FAIL**. TSTRQXC's "the lowest slot"
     assertion is the check that slot 0 came back FREE;
   - TSTRQXM batch 32/32 with the host listener running. The TSO step fails
     CONNECT if the one-shot listener is not up again in time, which is not a
     defect;
   - TSTRQX2 in the matrix CC 20 (no PARM), as always.

   Job A (`RQX2A` + `RQX2B` together): A 18/18, B 23/23.
4. `P NSFS` takes the **free** path: no `NSF054W`, `NSF044I`.
5. `TSTSCTP` reports **`PROBE nsfs ABSENT`**. A is freed.

## Q5. Fresh start: the production build writes the tail

`S NSFS` (call its anchor C):
- `NSF055I ... 137280 ...` gives X3 **within 8 KB of X2**, because A's 139264
  bytes came back and C took as many. Had A not been freed, X3 would sit about
  139 KB below X2;
- **`NSF060I RENDEZVOUS REGISTERED -- ANCHOR=C TAIL=40404040`**. This is the
  2b fix observed in NSFS itself, read back from key-0 CSA. The round-2 build
  could only print it without the tail, and the tail it left was `00000000`;
- `NSF041I ... ANCHOR=C`. No `NSF061I`/`NSF063W`/`NSF064W`.

C may equal `00A9C788` (first fit), but that is not required.

## Q6. The fresh cycle: adopting an anchor nobody repaired

1. `TSTRQXCL` (LEAK) prints `LEAK slot=0 state=4 inflight 0->1 anchor=C`.
2. `P NSFS` gives `NSF054W 1 CLIENT(S) ...`. `TSTSCTP` reports
   `CHAINED C TAIL=40404040`.
3. `S NSFS`:
   - **`NSF061I ... ANCHOR=C -- ADOPTING`**. This is round 2's Q3 as it was
     meant: the predecessor written by the production build, with no rig in
     between;
   - X4 within 8 KB of X3;
   - `NSF062I ... REAPED=0 POSTED=0 INFLIGHT 1 -> 1`;
   - `NSF065W ...` reusing C's router EP;
   - no `NSF060I`.
4. `P NSFS` gives `NSF054W`. `TSTSCTD` gives `DEAD SLOT 0 ... ANCHOR=C`,
   CC 0.
5. `S NSFS`:
   - `NSF061I`;
   - X5 within 8 KB of X4;
   - `NSF062I ... REAPED=1 ... INFLIGHT 1 -> 0`;
   - `NSF065I ... FREED`.
6. `P NSFS` takes the free path (`NSF044I`, no `NSF054W`). `TSTSCTP` gives
   `ABSENT`.

**Falsified if** step 3 says `NSF064W ... NOT OURS`. That would be the round-2
defect still present in the deployed NSFS.

## Q7. REPLACE: a private SSCTSUSE under our name

1. `TSTSCTH` (HOLD) chains `nsfs` into its own private storage and ends.
2. `S NSFS`:
   - `NSF063W RENDEZVOUS BADEYE -- SSCT REMOVED`, with no `ANCHOR=`;
   - then `NSF060I REGISTERED -- ANCHOR=D TAIL=40404040`.
3. `TSTSCTP` gives `CHAINED D TAIL=40404040`. `P NSFS` takes the free path,
   and `TSTSCTP` then gives `ABSENT`.

**Falsified if** NSFS abends at start, or says `NSF061I` or `NSF059W`.

## Q8. The NSFV half of the Stage-0 set (version 4)

With `S NSFV`, `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` give
**438 PASS / 0 FAIL**, CC 0, batch and TSO. That is the last recorded figure;
none of these tests changed in 2b.

## End state

- No NSF STC is running, `nsfs` is ABSENT, and SVC 239 is restored.
- A and C are freed. No anchor of this round stays in CSA.
- `NSF.LINKLIB` is restored from `NSF.V2X.LINKLIB` and verified by content.
- No IEA995I and no ABEND in the console log over the round.

## Not run this round, and why

As in round 2:
- **S2** (post a LIVE owner) and **ASIDE live** are host-pinned only;
- **BADVER live** would cost 139 KB until IPL;
- **the ESTAE retain path** is not induced.

Also not run: **a real CL4 collision** (`NSFSTACK` against `NSFS`). It needs a
second procedure. It is host-pinned in TSTSCT section 4, now registered under
the other jobname rather than planted.
