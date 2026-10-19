# d2 round 2: adoption, live predictions

Written 2026-10-15, **before any deploy**, and not edited afterwards. The
record goes in a separate file beside this one.

Build under test: branch `d2-round2` (PR #138), anchor version 4, mbt 3,
cc370 1.6.3, libc370 2.11.0. Stand: dev1. Issue #136.

Symbols used below: **A** is the anchor of the first start, **E1** the SVC
routine EP that start reports in `NSF042I`, **L1** its `NSF055I` largest free
block.

## Q0. The deploy took effect

The first `S NSFS` says `NSF055I CSA POOL 137280 BYTES (64 SLOTS X 2144)`.
Every build on `main` says **137272**, so 137280 is impossible on the old
module. Later starts in this round that adopt say `NSF061I`, which no earlier
build contains.

## Q1. Fresh start, and the regression set on it

`S NSFS`: `NSF055I ... 137280 ...`, `NSF042I SVC 239 STOLEN (EP E1)`,
`NSF060I RENDEZVOUS REGISTERED -- ANCHOR=A`, `NSF041I ... ANCHOR=A`. No
`NSF061I`/`NSF063W`/`NSF064W`.

`TSTSCTM` (GATE), batch and TSO: **30 PASS / 0 FAIL per step, 60 in all.**
Round 1b had 25 per step. Arm B gains five assertions:
- the private anchor registers;
- `find` says BADEYE for it;
- no address is handed back;
- the private one is removed;
- the CSA anchor for the control was obtained.

The rest of arm B is unchanged.

## Q2. The retain posture, by LEAK

1. `TSTRQXC PARM='LEAK'` prints
   `TSTRQXC: LEAK slot=0 state=4 inflight 0->1 anchor=A`.
2. `P NSFS` gives `NSF054W 1 CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE
   RETAINED`.
3. `TSTSCTP` reports `PROBE nsfs CHAINED A`.

## Q3. Adoption, the REUSE branch (inflight stays 1)

`S NSFS`:
- `NSF061I RENDEZVOUS FOUND A RETAINED ANCHOR=A -- ADOPTING`.
- `NSF055I ... 137280 ... LARGEST FREE BLOCK NOW` within 8 KB of L1. A start
  that allocated would show a drop of about 139 KB, as every retain cycle on
  `main` does.
- `NSF062I ADOPTED ANCHOR=A -- REAPED=0 POSTED=0 INFLIGHT 1 -> 1`.
- `NSF065W 1 CLIENT(S) STILL IN FLIGHT -- REUSING THE RETAINED SVC ROUTINE
  (EP E1)`, then `NSF042I SVC 239 STOLEN (EP E1)`: the same EP.
- `NSF041I ... ANCHOR=A`.
- **No** `NSF060I` and no `NSF059W`: the SSCT already points at A and is not
  registered a second time.

**Falsified if** the start says `NSF064W ... ITS SERVER HAS NOT ENDED`. That
would mean the old NSFS's ASCB address was taken by another address space
between the stop and the start. It is the safe outcome, but it is not this
row. **Also falsified** by any `NSF055I` drop of about 137 KB.

Then `P NSFS` gives `NSF054W` again, because LEAK's count is still in flight.
`TSTSCTP` reports `CHAINED A`.

## Q4. Adoption, S1 and the FREE branch

1. `TSTSCTD` (`TSTSCTM PARM='DEAD'`) prints `TSTSCTM: DEAD SLOT 0 PENDING
   ASCB=00FFFFF8 ASID=1 INFLIGHT=1 ANCHOR=A -- NOW S NSFS`, CC 0.
   **Synthetic death:** the identity is fabricated and asserted DEAD through
   `nsfreqx_classify` against the live ASVT. A real dying client's slot is
   reclaimed by the running STC within a second (M5-2c1), so it never reaches
   a retained anchor.
2. `S NSFS`:
   - `NSF061I ... ANCHOR=A`;
   - `NSF062I ADOPTED ANCHOR=A -- REAPED=1 POSTED=0 INFLIGHT 1 -> 0`;
   - `NSF065I RETAINED SVC ROUTINE (EP E1) FREED`;
   - `NSF042I SVC 239 STOLEN (EP E2)`;
   - `NSF041I ... ANCHOR=A`.

   E2 may well equal E1, because the freed block is the first that fits. So
   **`NSF065I` is the discriminator, not the EP.**
3. The NSFS half of the Stage-0 set on the twice-adopted anchor, batch and
   TSO:
   - `TSTRQXC` and `TSTRQXF`: **130 PASS / 0 FAIL**. `TSTRQXC`'s "the lowest
     slot" assertion is the check that slot 0 came back FREE.
   - `TSTRQXM` and Job A (`TSTRQX2`) as last recorded: TSTRQXM batch 32/32,
     Job A A 18/18 and B 23/23.
4. `P NSFS` takes the **free** path: no `NSF054W`, `NSF044I`.
5. `TSTSCTP` reports **`PROBE nsfs ABSENT`**.

## Q5. REPLACE: a private SSCTSUSE under our name

1. `TSTSCTH` (HOLD) chains `nsfs` pointing into its own private storage, and
   ends.
2. `S NSFS`:
   - `NSF063W RENDEZVOUS BADEYE -- SSCT REMOVED`, with **no** `ANCHOR=`. `find`
     never reads a private address, so it hands none back.
   - Then a fresh start: `NSF055I ... 137280`, `NSF060I REGISTERED -- ANCHOR=B`.
3. `TSTSCTP` reports `CHAINED B`.
4. `P NSFS` takes the free path, and `TSTSCTP` then reports `ABSENT`.

**Falsified if** NSFS abends at start, which is the defect the check exists
for. **Also falsified** if it says `NSF061I` or `NSF059W`.

## Q6. The NSFV half of the Stage-0 set (version 4)

With `S NSFV`, `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` give
**438 PASS / 0 FAIL**, CC 0, batch and TSO. That is the last recorded figure;
none of these tests changed.

## End state

- No NSF STC is running. `nsfs` is ABSENT.
- Retained anchor A has been freed: Q4's free path freed it.
- `NSF.LINKLIB` is restored from the backup and verified by content.
- No IEA995I and no ABEND in the console log over the round.

## Not run this round, and why

- **S2 (post a LIVE owner).** No induction leaves a LIVE client parked in a
  retained anchor: the stop's drain posts every LIVE one. Host-pinned only.
- **ASIDE live** (`S NSFS` during the old instance's 10 s drain). The timing
  goes through a console path that 500s on `S`/`P`. Host-pinned only.
- **BADVER live.** It would need a version-3 anchor retained by the `main`
  build before the deploy, which costs 139 KB of CSA until IPL. Host-pinned
  only.
- **The ESTAE retain path** and #83. Not induced. Its stores are unchanged
  since #79.
