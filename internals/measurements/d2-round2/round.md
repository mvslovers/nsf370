# d2 round 2: adoption, live on dev1 (halted at Q3)

**Date:** 2026-10-15, about 00.22–00.30 MVS time. **Stand:** dev1 (MVSCE on
mvsdev). The slot was cleared by the MBT coordinator. **Build:** branch
`d2-round2` at 7e0edde, anchor version 4, mbt 3, cc370 1.6.3, libc370 2.11.0.
**Predictions:** `predictions.md`, written before the deploy and not edited.

The round **stopped at Q3**, which was falsified by a defect from round 1.
Round 2b (`../d2-round2b/`) fixed it and ran the remaining rows.

## Preparation

| Step | Result |
|---|---|
| No NSF STC running | `D A,L` |
| Backup | `NSFBKP3 JOB02291`: IEBCOPY `NSF.LINKLIB` into a new `NSF.V2X.LINKLIB` (250 trk), CC 0, 6 of 6 members. The backed-up NSFS **lacks `NSF060I`**, so dev1 was carrying the pre-d2 merged build, not `main` |
| Deploy | first refused for lack of space. `mbt deploy --reallocate` then gave 400 trk, 26% used, 6 members |
| Deploy check | the deployed NSFS contains `NSF061I` and `NSF063W` (EBCDIC search of the downloaded member). Q0 below is the live check |

## The predictions

| | Prediction | Result |
|---|---|---|
| Q0 | `NSF055I CSA POOL 137280` | **held**: STC00710 says 137280; every `main` build says 137272 |
| Q1 | fresh start; TSTSCTM 60/0, TSTRQXC+F 130/0, TSTRQXM 32/32 batch | **held**, below |
| Q2 | LEAK → `NSF054W` → `CHAINED A` | **held** |
| Q3 | the next start adopts A | **FALSIFIED**: `NSF064W ... FOREIGN ... NOT OURS` |
| Q4–Q6 | | not run here; run in round 2b |

### Q1

| Time | Event |
|---|---|
| 00.24.11 | `S NSFS` (**STC00710**): `NSF055I CSA POOL 137280 ... LARGEST FREE BLOCK NOW 1003520`, `NSF042I SVC 239 STOLEN (EP 00A9C208)`, `NSF060I RENDEZVOUS REGISTERED -- ANCHOR=00A9C788`, `NSF041I` |
| 00.25 | `mbttest-JOB02295.txt`: TSTSCTM **30/30 per step, 60 in all**, the private-SSCTSUSE rows included. TSTRQXC 8 + TSTRQXF 57 per step, **130 in all**. TSTRQX2 CC 20 (no PARM), as always. TSTRQXM failed CONNECT, errno 61, because no listener was up |
| | `mbttest-JOB02296.txt`, with the host listener up: TSTRQXM batch **32/32**, 9353 bytes byte-exact. The TSO step failed CONNECT because the listener takes only one connection |

### Q2

| Time | Event |
|---|---|
| 00.27.32 | `tstrqxcl-JOB02297.txt`: `TSTRQXC: LEAK slot=0 state=4 inflight 0->1 anchor=00A9C788` |
| 00.27.45 | `P NSFS`: `NSF043I`, then `NSF054W 1 CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE RETAINED (EXHAUSTED=2)`, `NSF044I` |
| 00.28.15 | `tstsctp-q2-JOB02298.txt`: `PROBE nsfs CHAINED 00A9C788` |

### Q3: falsified

| Time | Event |
|---|---|
| 00.28.30 | `S NSFS` (**STC00711**): **`NSF064W RENDEZVOUS FOREIGN ANCHOR=00A9C788 NOT ADOPTED (NOT OURS) -- NOT REGISTERING`**, then a fresh anchor `00ABE788` (EP `00ABE208`). The largest free block fell to 864256, **139264 below** STC00710's reading: one more anchor and its router |
| 00.29.31 | `P NSFS`: the free path, no `NSF054W`. B was freed |
| 00.29.43 | `tstsctp-q3b-JOB02299.txt`: **`PROBE nsfs CHAINED 00A9C788`**, still A's |

**The cause, a round-1 defect.** No production code writes `jobname_tail`.
`nsfsct_register` derived the tail and discarded it, so a real anchor holds
`00000000`, while job `NSFS` derives four blanks: FOREIGN. Only the test
fixtures wrote a tail (`tstsctm.c:77`, `tstsct.c:29`), and they wrote the
right one, which hid the defect. Round 1b only showed that the SSCT was
**found**. Nothing in it classified a real anchor.

**What Q3's falsification did confirm**, as a side result: STC00711's stop did
not remove the SSCT, because STC00711 had neither registered nor adopted it.
That is bdcb8b5 live; the round-1 code would have removed A's SSCT on that
stop.

The `getmain request ... failed` console lines come from the largest-block
probe under libc370 2.x. Round 1b had them as well (6 in STC00704), so they
are not new.

## State left for round 2b

- A (`00A9C788`) RETAINED: LEAK's slot 0 CLAIMED, inflight 1, tail
  `00000000`, router EP `00A9C208`, chained under `nsfs`.
- No NSF STC running, SVC 239 restored.
- `NSF.LINKLIB` holding the round-2 build, the backup in `NSF.V2X.LINKLIB`.
- dev1 was lent to oftpd for its OFTPD-only abend test in the pause. Through
  MBT, oftpd confirmed that it touched no NSF library and not the SSCT.

## Files

- `mbttest-JOB02295.txt`, `mbttest-JOB02296.txt`: the Q1 runs.
- `tstrqxcl-JOB02297.txt`: LEAK.
- `tstsctp-q2-JOB02298.txt`, `tstsctp-q3b-JOB02299.txt`: the two PROBEs.
- `nsfs-STC00710.txt`, `nsfs-STC00711.txt`: the two STC spools.
