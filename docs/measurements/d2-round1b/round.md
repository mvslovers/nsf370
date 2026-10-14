# d2 round 1b: the lower-case SSCT name, live on dev1

**Date:** 2026-10-14, about 16.48–16.53 MVS time. **Stand:** dev1 (MVSCE on
mvsdev, mvsMF `mvsdev.lan:8080`), slot cleared by the MBT coordinator.
**Build:** branch `d2-round1` at 5a44e51, mbt 3, cc370 1.6.3, libc370 2.11.0.
**Predictions:** `predictions.md`, written and committed before the deploy,
not edited.

## Preparation, and the end state

| Step | Result |
|---|---|
| No NSF STC running | `D A,L` |
| Backup | `NSFBKP2 JOB02282`: IEBCOPY of `NSF.LINKLIB` (the merged 2.x build) into new `NSF.V2X.LINKLIB`, CC 0000; member list read back, 6 of 6 |
| Deploy | `mbt deploy` into `NSF.LINKLIB`: **members replaced, library not deleted**, 53% of 396 tracks, no STC holding it |
| Restore | `NSFREST JOB02290`: IEBCOPY `NSF.V2X.LINKLIB` → `NSF.LINKLIB` with replace, `IEB154I` for all 6 |
| Restore verified by content | NSFS, NSF, NSFV and NSFVSVC downloaded from `NSF.LINKLIB` are **byte-identical to the backup**, and NSFS no longer contains `NSF060I` (which only a d2 build can emit, and which it did emit in Q3) |
| Clean-up | `NSF.V2X.LINKLIB` and the test library deleted, `JOB02282`–`JOB02290` and `STC00703`/`STC00704` purged after their spools were saved here |

## Did the deploy take effect?

**`NSF060I RENDEZVOUS REGISTERED`** at Q3's start is the positive check: no
build from `main` contains that message. **`TSTSCTM: HOLD nsfs`** shows the
lower-case derivation came from the new test build: the round-1 build derived
`NSFS`.

## The predictions

| | Prediction | Result |
|---|---|---|
| Q1 | two-name gate with lower-case names, 25 / 0 per step | **held**: `TSTSCTM` CC 0 batch and TSO, **50 PASS, 0 FAIL** (`JOB02285`). `'NSZ1' (upper case) is NOT found while 'nsz1' is chained`; `jobname 'MBTTEST ' -> key 'mbtt'`, so `tolower` folds EBCDIC correctly on the target |
| Q2 | with `nsfs` chained, `S NSFS` **starts** and says `NSF059W`; a clean stop removes `nsfs` | **held**, below |
| Q3 | an ordinary start registers under `nsfs`; a clean stop removes it | **held**, below |

### Q2: the round-1 failure, reversed

| Time | Event |
|---|---|
| 16.50.10 | `TSTSCTH JOB02286`: `TSTSCTM: HOLD nsfs CHAINED SSCTSUSE=00098464 -- NOW S NSFS`; the job ends and the SSCT stays |
| 16.50.17 | `S NSFS` (**STC00703**) **starts**: `NSF040I`, `NSF210I CTCI 0500/0501 UP`, `NSF042I SVC 239 STOLEN`, **`NSF059W RENDEZVOUS NOT REGISTERED ... (ALREADY CHAINED?)`**, `NSF041I`, `NSF001I` |
| 16.50.35 | `P NSFS`: `NSF043I SVC 239 RESTORED`, `NSF011I`, `IEF404I`; no `NSF054W`, so the free path |
| 16.50.53 | `TSTSCTP JOB02287`: **`PROBE nsfs ABSENT`** |

Round 1, in the same position with the key `NSFS`, got `IEF612I PROCEDURE
NOT FOUND` and the STC could not start at all.

### Q3: the ordinary start

| Time | Event |
|---|---|
| 16.51.02 | `S NSFS` (**STC00704**) on a clean chain: **`NSF060I RENDEZVOUS REGISTERED -- ANCHOR=00A9C7C8`**, `NSF041I ... ANCHOR=00A9C7C8` |
| 16.51.12 | `TSTSCTP JOB02288`: **`PROBE nsfs CHAINED 00A9C7C8`**, the same anchor |
| 16.51.22 | `P NSFS`: `NSF043I SVC 239 RESTORED`, `NSF011I`, `IEF404I`; no `NSF054W` |
| 16.51.39 | `TSTSCTP JOB02289`: **`PROBE nsfs ABSENT`** |

**No `IEA995I`, no ABEND** in the console log over the round. `D A,L` shows
no NSF STC at the end.

## Does NOT establish

- **The retain path on the new name.** It is not re-run; its code is
  unchanged, and round 1 measured the asymmetry under the old name.
  Re-inducing it would cost 139264 bytes of CSA until IPL.
- **A jobname with no letter in its first four characters**, which is
  refused. Host-pinned only.
- **Adoption.** Round 2.
