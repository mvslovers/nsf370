# d2 round 2b: register stamps the tail; adoption live on dev1

**Date:** 2026-10-15, about 01.39–01.51 MVS time. **Stand:** dev1 (MVSCE on
mvsdev), handed back by the MBT coordinator after oftpd's OFTPD-only test. OFTPD
(STC00715), HTTPD, UFSD and FTPD ran throughout and were not touched.
**Build:** branch `d2-round2` (7e0edde) plus the 2b change, anchor version 4,
mbt 3, cc370 1.6.3, libc370 2.11.0.
**Predictions:** `predictions-2b.md`, written before the deploy and not
edited. Its sha256 was recorded before the deploy in `predictions-2b.sha256`.

## Why the round exists

Round 2's Q3 was falsified (`../d2-round2/round.md`): no production code wrote
`jobname_tail`, so a real anchor carried `00000000` and the next start read
its own predecessor as FOREIGN.

**Red first:** with the fixtures changed to a wrong tail and section 8 (job
`NSFS`, zero tail, which is the live shape), the host suite failed 8
assertions, with `find` returning 4 (FOREIGN) where 1 (OK) was expected
(`host-red-tstsct.txt`).

**Fix:** `nsfsct_plat_register` takes the tail and writes it into the
anchor inside its key-0 window, before `ssct_install`. The host shim does the
same. The classifier is unchanged. `NSF060I` prints the tail as read back
from the anchor.

Afterwards: host 3626/0, cross build clean.

## Preparation, and the end state

| Step | Result |
|---|---|
| No NSF STC running | `D A,L` at 01.39.15 |
| Backup | still `NSF.V2X.LINKLIB` from round 2 (`NSFBKP3 JOB02291`) |
| Deploy | `mbt deploy` into `NSF.LINKLIB`: members replaced, 53% of 400 tracks |
| Restore | `NSFREST JOB02323`: IEBCOPY `NSF.V2X.LINKLIB` → `NSF.LINKLIB` with replace, `IEB154I` for all 6 |
| Restore verified by content | **all 6 members** downloaded from both libraries are byte-identical. The restored NSFS contains none of ` TAIL=`, `NSF061I` or `NSF060I`. The member list is exactly the 6 |
| Clean-up | after the maintainer's approval: `NSF.V2X.LINKLIB` and `IBMUSER.NSF370.V0R1M0D.TESTLIB` deleted (the listing afterwards shows `NSF.LINKLIB` alone); JOB02291–JOB02323 (rounds 2 and 2b, mbt's own deploy jobs included, as listed on the stand) and STC00710/00711/00716–00722 purged after their spools were saved here. The first attempt at the delete was refused by this session's permission check |

## The predictions

| | Prediction | Result |
|---|---|---|
| Q0 | the deployed NSFS contains ` TAIL=`; TSTSCTM contains `TAIL ANCHOR=` | **held**. The member downloaded from `NSF.LINKLIB` after the deploy contains ` TAIL=` and the whole `NSF060I ... TAIL` format (EBCDIC search). For the TESTLIB's TSTSCTM the evidence is live: `JOB02306` printed `TSTSCTM: TAIL ANCHOR=00A9C788 ...`, which no earlier TSTSCTM contains |
| Q1 | TSTSCTM GATE 33 per step, 66 | **held**: `mbttest-q1-JOB02305.txt`, 66 PASS / 0 FAIL, including `register STAMPED our tail into key-0 CSA` |
| Q2 | TAIL repairs A | **held**: `tstsctt-q2-JOB02306.txt`, 5/5, `SERVER_ASCB=00FDD180 INFLIGHT=1 TAIL=00000000` → `TAIL=40404040` |
| Q3 | adopt A, REUSE | **held** |
| Q4 | adopt A again, REAP, free path | **held**, with two exceptions for Job A (below): it ran on a fresh anchor, and the prediction quoted stale totals |
| Q5 | fresh start, `NSF060I ... TAIL=40404040`, X3 within 8 KB of X2 | **held** |
| Q6 | the fresh cycle adopts C with no rig | **held** |
| Q7 | REPLACE | **held** |
| Q8 | NSFV half 438/0 | **held** |

### Q3 and Q4: adopting A, which the rig repaired

**The rig wrote A's tail.** These two adoptions are real, but they adopt by an
identity that the test supplied.

| Time | Event |
|---|---|
| 01.40.24 | `TSTSCTT JOB02306`: `TAIL ANCHOR=00A9C788 SERVER_ASCB=00FDD180 INFLIGHT=1 TAIL=00000000`, then `TAIL=40404040 -- NOW S NSFS` |
| 01.40.41 | `S NSFS` (**STC00716**): **`NSF061I RENDEZVOUS FOUND A RETAINED ANCHOR=00A9C788 -- ADOPTING`**, `NSF055I ... NOW 1003520` (X1), `NSF062I ... REAPED=0 POSTED=0 INFLIGHT 1 -> 1`, `NSF065W ... REUSING THE RETAINED SVC ROUTINE (EP 00A9C208)`, `NSF042I SVC 239 STOLEN (EP 00A9C208)`, `NSF041I ... ANCHOR=00A9C788`. No `NSF060I`, no `NSF059W` |
| 01.41.02 | `P NSFS`: `NSF054W 1 CLIENT(S) ... RETAINED (EXHAUSTED=0)` |
| 01.41.19 | `TSTSCTP JOB02307`: `PROBE nsfs CHAINED 00A9C788 TAIL=40404040` |
| 01.41.34 | `TSTSCTD JOB02308`: `DEAD SLOT 0 PENDING ASCB=00FFFFF8 ASID=1 INFLIGHT=1 ANCHOR=00A9C788`, 11/11. **Synthetic death**, as in round 2 |
| 01.41.40 | `S NSFS` (**STC00717**): `NSF061I`, X2 = 1003520 (= X1), **`NSF062I ... REAPED=1 POSTED=0 INFLIGHT 1 -> 0`**, **`NSF065I RETAINED SVC ROUTINE (EP 00A9C208) FREED`**, `NSF042I ... (EP 00A9C208)`: the same EP, reloaded into the freed block, which is why `NSF065I` and not the EP is the discriminator |
| 01.42 | `mbttest-q4-JOB02310.txt`: TSTRQXC 8 + TSTRQXF 57 per step, **130/0**. TSTRQXC's lowest-slot assertion shows that slot 0 came back FREE. **TSTRQXM 32/32 in both the batch and the TSO step**: the listener ran twice (`swl-2b.log`), so for the first time in d2 the TSO step is green as well. TSTSCTM 66/0. TSTRQX2 CC 20 (no PARM), as always |
| 01.43.17 | `P NSFS`: **the free path** (`NSF043I`, `NSF044I`, no `NSF054W`) |
| 01.43.34 | `TSTSCTP JOB02311`: **`PROBE nsfs ABSENT`** |

**The ALIVE falsifier was not met.** The start classified STC00710's server
ASCB (`00FDD180`) as ended: it adopted rather than going ASIDE. The round
shows that classification only, not what the ASVT held or why.

### Q5: the production build writes the tail

| Time | Event |
|---|---|
| 01.43.46 | `S NSFS` (**STC00718**): `NSF055I ... NOW 999424` (X3, **4096 below X2**: A's storage came back and C took as much), `NSF042I SVC 239 STOLEN (EP 00A83448)`, **`NSF060I RENDEZVOUS REGISTERED -- ANCHOR=00A9D788 TAIL=40404040`**, `NSF041I` |

C is `00A9D788`, 4096 above A. The router moved to a lower hole, `00A83448`.

### Job A, moved from Q4

Job A needs a freshly started NSFS with an untouched socket table. Q4's
regression set had already used STC00717, so Job A ran on STC00718 (anchor C,
fresh) instead of on the twice-adopted A. **This deviates from Q4.3.**

| Job | Result |
|---|---|
| `RQX2A JOB02312` | **19/19**, CC 0 |
| `RQX2B JOB02313` | **24/24**, CC 0 |

The predictions said 18/18 and 23/23. That was **a stale figure in the
prediction**: the wire arm (PR #119) moved both totals to 19 and 24
(`internals/milestone-history.md`, the Job A wire-arm entry). The result equals the
last recorded figure.

### Q6: adopting an anchor nobody repaired

| Time | Event |
|---|---|
| 01.46.14 | `TSTRQXCL JOB02314`: `LEAK slot=0 state=4 inflight 0->1 anchor=00A9D788` |
| 01.46.21 | `P NSFS`: `NSF054W 1 CLIENT(S) ... RETAINED (EXHAUSTED=0)` |
| 01.46.38 | `TSTSCTP JOB02315`: `PROBE nsfs CHAINED 00A9D788 TAIL=40404040` |
| 01.46.50 | `S NSFS` (**STC00719**): **`NSF061I RENDEZVOUS FOUND A RETAINED ANCHOR=00A9D788 -- ADOPTING`**, X4 = 999424 (= X3), `NSF062I ... REAPED=0 POSTED=0 INFLIGHT 1 -> 1`, `NSF065W ... (EP 00A83448)`, no `NSF060I` |
| 01.47.14 | `P NSFS`: `NSF054W` |
| 01.47.31 | `TSTSCTD JOB02316`: `DEAD SLOT 0 PENDING ... ANCHOR=00A9D788` |
| 01.47.36 | `S NSFS` (**STC00720**): `NSF061I`, X5 = 999424, `NSF062I ... REAPED=1 POSTED=0 INFLIGHT 1 -> 0`, `NSF065I ... (EP 00A83448) FREED` |
| 01.47.59 | `P NSFS`: free path |
| 01.48.16 | `TSTSCTP JOB02317`: `PROBE nsfs ABSENT` |

**This is round 2's Q3 as it was meant.** The anchor was registered by
NSFS's own build, nothing touched its identity, and the next start adopted it.

### Q7: REPLACE

| Time | Event |
|---|---|
| 01.48.30 | `TSTSCTH JOB02318`: `HOLD nsfs CHAINED SSCTSUSE=00099754` (private) |
| 01.48.35 | `S NSFS` (**STC00721**): **`NSF063W RENDEZVOUS BADEYE -- SSCT REMOVED`**, with no `ANCHOR=`. Then `NSF060I ... ANCHOR=00A9D788 TAIL=40404040`. No abend, so the private SSCTSUSE was never read |
| 01.48.58 | `TSTSCTP JOB02319`: `CHAINED 00A9D788 TAIL=40404040` |
| 01.49.03 | `P NSFS`: free path |
| 01.49.20 | `TSTSCTP JOB02320`: `ABSENT` |

### Q8: NSFV

| Time | Event |
|---|---|
| 01.49.32 | `S NSFV` (**STC00722**): `NSFV034I SVC 239 STOLEN (OLD EP 0000CCC8, NEW EP 00A83448, 40 FREE)`, `NSFV001I NSFV READY -- ANCHOR=00A9D788` |
| 01.50 | `mbttest-q8-JOB02322.txt`: TSTSVC, TSTMVCK, TSTUBUF, TSTDEATH and TSTXFW, **438 PASS / 0 FAIL**, CC 0 batch and TSO |
| 01.50.39 | `P NSFV`: `NSFV095I SVC 239 RESTORED`, `NSFV036I SVC ROUTINE UNLOADED`, `NSFV011I` |

At the end, **no IEA995I and no ABEND** in the console log from 01.39.15 on.
`D A,L` shows no NSF STC; `nsfs` is ABSENT; SVC 239 is restored.

## CSA, as deltas (#120)

| Reading | Value | Against |
|---|---|---|
| STC00710 (round 2, after allocating A) | 1003520 | |
| STC00711 (round 2, A held, B allocated) | 864256 | −139264 |
| X1, STC00716 (A held, nothing allocated) | 1003520 | = STC00710: B's storage came back at STC00711's stop |
| X2, STC00717 | 1003520 | = X1 |
| X3, STC00718 (A freed, C allocated) | 999424 | X2 − 4096 |
| X4, X5, and STC00721 | 999424 | = X3 |

No anchor of either round is left in CSA:
- A was freed by STC00717's free-path stop;
- B by STC00711's;
- C by STC00720's;
- D, the REPLACE start's fresh anchor (the same address, `00A9D788`), by
  STC00721's;
- NSFV's own anchor by STC00722's stop. `NSFV036I` without `NSFV098W` is the
  drained path, which ends in `nsfv_anchor_free` (`src/nsfv.c:631-641`).

The readings are consistent with that, but a largest-free-block figure does
not prove a free by itself. The free-path messages above are the evidence.

## Does NOT establish

- **A real client death in a retained anchor.** Both REAPs are of a
  fabricated identity (`DEAD`), as in round 2.
- **Why** `00FDD180` read as ended. The round shows only that the start
  classified it so.
- **S2** (post a LIVE owner), **ASIDE live**, **BADVER live** and **the ESTAE
  retain path**. All are host-pinned only, for the reasons in
  `predictions-2b.md`.
- **A real CL4 collision** (`NSFSTACK` against `NSFS`). It is host-pinned in
  TSTSCT section 4, where the foreign anchor is now registered under the
  other jobname rather than planted.
- **Job A on an adopted anchor.** It ran on a fresh one (above).

## Files

- `predictions-2b.md`, `predictions-2b.sha256`: the predictions and the hash
  taken before the deploy.
- `host-red-tstsct.txt`: the host red run before the fix.
- `mbttest-q1-JOB02305.txt`, `mbttest-q4-JOB02310.txt`,
  `mbttest-q8-JOB02322.txt`: the three matrix runs.
- `tstsctt-q2-JOB02306.txt`: TAIL.
- `tstsctd-q4-JOB02308.txt`, `tstsctd-q6-JOB02316.txt`: DEAD.
- `tstrqxcl-q6-JOB02314.txt`: LEAK.
- `tstscth-q7-JOB02318.txt`: HOLD.
- `tstsctp-*.txt`: the PROBEs.
- `joba-JOB02312.txt`, `joba-JOB02313.txt`: Job A.
- `nsfrest-JOB02323.txt`: the restore.
- `nsfs-STC00716.txt` … `nsfs-STC00721.txt`, `nsfv-STC00722.txt`: the STC
  spools.
- `swl-2b.log`: the host listener log for TSTRQXM.
