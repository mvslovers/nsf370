# #83 live round: recovery and the startup failure stop freeing under I/O

**Date:** 2026-10-15, about 03.10–03.23 MVS time. **Stand:** dev1 (MVSCE on
mvsdev), slot cleared by the MBT coordinator. OFTPD (STC00715), mvsMF/HTTPD,
UFSD and FTPD ran throughout and were not touched.
**Predictions:** `predictions.md`, committed before any deploy (0c6f35b), not
edited. Its sha256 is in `predictions.sha256`.

**Builds:**
- **RED**: `main` d350bd0 plus the temporary injection.
- **GREEN**: branch 0c6f35b plus the injection.
- **FINAL**: branch 0c6f35b alone.

The injection was the first statement of `nsfsx_stats_extra`:
`if (!__issup()) *(volatile unsigned *)0 = 0;`, marker `NSF83INJ`. It went in
by script, into throwaway worktrees only. `git grep NSF83INJ -- src include`
on the branch returns nothing.

## Preparation, and the end state

| Step | Result |
|---|---|
| No NSF STC running | `D A,L` 03.10.36 |
| Backup | `NSFBKP4 JOB02324`: IEBCOPY `NSF.LINKLIB` → new `NSF.V2X.LINKLIB`, CC 0, 6 of 6 members |
| RED deploy | first refused for space (dead space from the earlier replaces). `mbt deploy --reallocate` then gave 400 trk, 26% |
| Each deploy verified by content | the member downloaded from `NSF.LINKLIB` is **byte-identical to that build's NSFS**: RED 347845 B, GREEN 348701 B, FINAL 348665 B. RED lacks `NSF068W`; GREEN and FINAL contain it |
| TESTLIB recreated | `mbt test --mvs --only TSTSCTM` (`JOB02329`, GATE 66/0) so `TSTSCTP` could run. The first PROBE, `JOB02327`, was a JCL ERROR because the TESTLIB had been cleaned up after round 2b |
| Restore | `NSFREST JOB02337`, `IEB154I` ×6 |
| Restore verified by content | all 6 members, downloaded into a fresh directory, **byte-identical to the backup**. NSFS contains none of `NSF068W`, `NSF061I` or `NSF060I` |
| Clean-up | `NSF.V2X.LINKLIB` and the TESTLIB deleted; JOB02324–JOB02337 and STC00724–00733 purged, the spools saved here first |

**A trap met on the way, in the instrument.** The first RED deploy check read
336249 bytes without `NSF061I`, which looked like a failed deploy.
`zowe zos-files download` **skips a file that already exists**
(`... skipped as it already exists.`), and the output had gone to `/dev/null`.
So the check compared an earlier download. A fresh file name showed the deploy
had worked. Every later check downloaded into a new file.

## The predictions

| | Prediction | Result |
|---|---|---|
| Q0 | each deploy took effect | **held**, by content as above |
| Q1 | RED, startup failure: `A0A- 2 ... 0000A040` | **FALSIFIED**: `ABEND SA03`, no `IEA700I` |
| Q2 | RED, recovery: `NSF903I`, then `A0A- 2 ... 0000A040`, no `NSF901I` | **held** |
| Q3 | GREEN, startup failure: no abend, no `NSF068W`, CC 8 | **held** |
| Q4 | GREEN, recovery: `NSF901I`, then the original `S0C4` | **held** |
| Q5 | FINAL: STATS answers, TSTRQXC+F 130/0, clean stop with no `NSF068W`, Phase 1 `S NSF`/`P NSF` clean | **held** |

### Q1: falsified, and what it shows

| Time | Event |
|---|---|
| 03.12.45 | `S NSFV` (STC00724): `NSFV034I SVC 239 STOLEN` |
| 03.12.56 | `S NSFS` (**STC00725**): `NSF210I`/`NSF211I` (CTCI up), `NSF049E SVC 239 IN USE (EP 00A83448) -- NOT STOLEN`, `NSF009E`, then **`IEF450I NSFS NSFS - ABEND SA03 U0000`**. No `IEA700I` anywhere in the round's log before Q2 |

`SA03` is a task ending while its subtasks are still attached: `return 8` from
`main` with the two CTCI subtasks alive. The `A0A` did **not** occur. The
start failed in the same second the devices started, so the reading that
fits is that the READ subtask had not yet parked its EXCP when `mm_shutdown()`
ran: nothing was fixed yet. *That is an inference.* The round shows only that
no `A0A` came.

So P3's defect on today's `main` is an `SA03`, not an `A0A`. The 40-CHK
`SA0A` (STC 1506) was measured on libc370 1.x. Both are fixed by C: the
devices are quiesced and the subtasks joined before `return 8` (Q3).

### Q2: the defect, reproduced on today's main

| Time | Event |
|---|---|
| 03.15.00 | `S NSFS` (**STC00726**): `NSF060I ... ANCHOR=00A9D788 TAIL=40404040`, `NSF001I` |
| 03.15.27 | `F NSFS,STATS`: `NSF811I` lines, then the injection: `NSF900E`, `NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y`, `NSF903I ... SVC RESTORED, CSA AND ROUTER RETAINED`, **`IEA700I A0A-  2 NSFS NSFS 00 0000A040 0014AFC0 009DC7B0`**, `IEF450I ... ABEND SA0A`. **No `NSF901I`** |

The length field is the predicted `0000A040`, the CTCIBUF region after
libc370's rounding. The TCB field `009DC7B0` is the one all five #79
occurrences carried. The address moved from `0015DFC0` to `0014AFC0`: a
different build, a different private-area layout.

Q1 and Q2 together are the discriminating pair the survey lacked. A start
that failed within the second of the device start gave no `A0A`. A recovery
27 s after the start, with the READ parked, did.

**Recorded, not the gate:** `S NSFS` (**STC00727**) adopted the
recovery-retained anchor: `NSF061I`, `NSF062I ... INFLIGHT 0 -> 0`,
`NSF065I ... FREED`. `P NSFS` took the free path, and `TSTSCTP JOB02331`
gave `ABSENT`.

### Q3: the startup failure, fixed

| Time | Event |
|---|---|
| 03.17.26 | `S NSFV` (STC00728) |
| 03.17.38 | `S NSFS` (**STC00729**): `NSF210I`/`NSF211I`, `NSF049E`, `NSF009E`, `IEF404I NSFS - ENDED`. **No abend, no `NSF068W`, no `NSF206W`.** The spool shows `IEF142I NSFS NSFS - STEP WAS EXECUTED - COND CODE 0008` |

C did it: the subtasks joined, so the guard saw no busy device and the
regions were released. D was present and not needed.

### Q4: recovery, fixed

| Time | Event |
|---|---|
| 03.18.44 | `S NSFS` (**STC00730**): `NSF060I`, `NSF001I` |
| 03.19.10 | `F NSFS,STATS`: `NSF900E`, `NSF902I`, `NSF903I ... SVC RESTORED, CSA AND ROUTER RETAINED`, **`NSF901I NSF EMERGENCY TEARDOWN COMPLETE`**, then **`IEF450I NSFS NSFS - ABEND S0C4 U0000`**. No `IEA700I` |

The original abend percolated, and the recovery path completed **with the
CTCI pair up**. Before this it had completed only twice: once in #79's arm
with the pair down, and once in 40-CHK (STC 1505).

**Recorded, not the gate:** `S NSFS` (**STC00731**) adopted the anchor
(`NSF062I ... INFLIGHT 0 -> 0`, `NSF065I ... FREED`). `P NSFS` took the free
path with **no `NSF068W`**, which is the clean stop on the fixed code. Then
`TSTSCTP JOB02333` gave `ABSENT`.

### Q5: FINAL, no regression

| Time | Event |
|---|---|
| 03.20.54 | `S NSFS` (**STC00732**): `NSF060I`, `NSF001I` |
| 03.21.16 | `F NSFS,STATS`: `NSF810I STATS 52 COUNTER(S)` and 52 `NSF811I`. No abend, so the injection is absent |
| 03.21 | `mbttest-q5-JOB02336.txt`: TSTRQXC + TSTRQXF **130 PASS / 0 FAIL**, CC 0 batch and TSO |
| 03.22.05 | `P NSFS`: `NSF043I`, `NSF044I`, `NSF011I`, `IEF404I`. **No `NSF068W`** |
| 03.22.22 | `S NSF` (**STC00733**, Phase 1): `NSF000I`, `NSF210I`, `NSF211I`, `NSF001I`, `NSF803I` |
| 03.22.46 | `P NSF`: `NSF830I`, `NSF011I`, `IEF404I`. **No `NSF068W`** |

**Over the round:** no IEA995I. The abends were exactly STC00725 `SA03` (Q1,
falsified), STC00726 `SA0A` (Q2) and STC00730 `S0C4` (Q4). **No IPL.** SVC 239
was restored at every stop and in both recoveries. No anchor is left in CSA.

## Does NOT establish

- **That the fixed pages are the READ buffer.** Length and TCB match; the
  address was not compared with the region's base, which no message prints.
- **Why Q1 gave `SA03` rather than `A0A`.** The not-yet-parked READ is an
  inference from the timing.
- **P2** (Phase-1 recovery): the same change in `nsfmain.c`, never provoked.
  Phase 1 was only started and stopped.
- **P4** (a subtask that will not join, `NSF206W`): no provocation; host-pinned
  in TSTDEV scenario 4. Whether `NSF068W` then appears on MVS is open.
- **TSTSTCM** (the on-MVS test of `nsfstc.c`, which links the changed
  `nsf_shutdown`) did not run; Q5 ran TSTRQXC and TSTRQXF only. Its change
  is a return value it ignores; host TSTSTC pins the behaviour.
- **The ESTAE retain round of ADR-0048.** Two recovery-retained anchors were
  adopted and freed (STC00727, STC00731), but as the way back to a clean
  stand, without that round's predictions. It stays a round of its own.

## Files

- `predictions.md`, `predictions.sha256`
- `nsfbkp4-JOB02324.txt`, `nsfrest-JOB02337.txt`
- `nsfs-STC00725.txt` (Q1), `stc-STC00726.txt` (Q2), `stc-STC00727.txt`,
  `nsfs-STC00729.txt` (Q3), `stc-STC00730.txt` (Q4), `stc-STC00731.txt`,
  `stc-STC00732.txt` (Q5), `stc-STC00733.txt` (Phase 1), and
  `stc-STC00724.txt`/`stc-STC00728.txt` (NSFV)
- `mbttest-tstsctm-JOB02329.txt`, `mbttest-q5-JOB02336.txt`
- `probe-*.txt`: the PROBEs; `probe-q1-JOB02327.txt` is the JCL ERROR
