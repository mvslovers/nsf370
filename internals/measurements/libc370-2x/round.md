# libc370 2.x port: live round on dev1

**Date:** 2026-10-14, about 15.55–16.02 MVS time. **Stand:** dev1 (MVSCE on
mvsdev, mvsMF `mvsdev.lan:8080`), slot cleared by the MBT coordinator.
**Build:** branch `libc370-2x-port`, cc370 1.6.3, libc370 2.11.0, mbt 3.
**Predictions:** `predictions.md`, written and committed before the deploy,
not edited.

## Preparation

| Step | Result |
|---|---|
| No NSF STC running | `D A,L`: JES2, NET, TSO, UFSD, FTPD, OFTPD, HTTPD only |
| Backup | `NSFBKUP JOB02265` IEBCOPY `NSF.LINKLIB` → new `NSF.V1X.LINKLIB` (WORK00), CC 0000, `IEB154I` for all 6 members, and the member list of the copy read back: NSF, NSFECHO, NSFS, NSFTECHO, NSFV, NSFVSVC |
| Deploy, first try | refused by mbt 3 **before copying anything**: `NSF.LINKLIB has about 5 of 103 tracks free (95% used, 3 extent(s)); the 6 module(s) need about 99` |
| Deploy, `--reallocate` | `NSF.LINKLIB` deleted and reallocated on WORK00 as 396 tracks, 6 modules copied, 26% used, 1 extent |

## Did the deploy take effect?

This port adds no message, so two positive checks:

- **The module stamp.** NSF, NSFS and NSFV, downloaded from `NSF.LINKLIB`,
  carry `DC C'CC370',AL1(1,6,3)`, i.e. cc370 1.6.3. The same members in the
  backup `NSF.V1X.LINKLIB` carry no stamp at all. `NSFVSVC` (pure assembler,
  source unchanged) differs from its backup in 7 bytes, all inside the
  linker's IDR record after `LD370`: the link date.
- **P1.** TSTCTHR printed `rc=4`, which a 1.x build cannot produce (see P2).
  That check covers the test library, not `NSF.LINKLIB`; the stamp covers
  `NSF.LINKLIB`.

## The predictions

| | Prediction | Result |
|---|---|---|
| P1 | deploy seen to take effect | **held** (above) |
| P2 | TSTCTHR prints `clib_identify_cthread rc=4` and passes | **held**: `rc=4` in batch and TSO, `TSTCTHR` 18 PASS / 0 FAIL CC 0 (`JOB02271`) |
| P3 | NSFV Stage-0 set 438 / 0 | **held**: `TSTSVC`/`TSTMVCK`/`TSTUBUF`/`TSTDEATH`/`TSTXFW` all CC 0 batch and TSO, **438 PASS, 0 FAIL** (`JOB02269`) |
| P4 | NSFS round 130 / 0 | **held**: `TSTRQXC`/`TSTRQXF` CC 0 batch and TSO, **130 PASS, 0 FAIL** (`JOB02273`) |
| P5 | production device path up, ping 20/20 | **held**: see below |
| P6 | mbt 3 deploy behaviour, recorded | see below |

P2's consequence, now **observed in a production module**: NSFS links the
CTCI bottom half, so `NSF210I CTCI 0500/0501 UP` at `S NSFS` shows
`nsfthr_setup()` → `dev_start` succeeding with IDENTIFY returning 4. The
unfixed branch (`nsfthr_setup` returning 4) was not run.

## P5: the production device path

- `S NSFS` (STC00700): `NSF851I ... NDS=1`, `NSF040I`, `NSF210I CTCI 0500/0501
  UP ... MTU 1500`, `NSF211I`, `NSF055I CSA POOL 137272 BYTES ... LARGEST FREE
  BLOCK NOW 1003520`, `NSF042I SVC 239 STOLEN`, `NSF041I`, `NSF001I`.
- `S NSF` (STC00701, Phase 1): `NSF000I`, `NSF210I`, `NSF211I`, `NSF001I`.
  A host ping `-c 20` gives **20/20, 0% loss**, rtt 0.640/0.891/1.175 ms.
  `F NSF,APPS` still answers `NSF808E UNKNOWN COMMAND APPS`, the Phase-1 red
  line.
- `tun0` was up: `192.168.200.2 peer 192.168.200.1`.

## Additionally: TSTRQXM (the cross-AS data path)

With `samples/host/shortwrite_listener.py` on the host (`192.168.200.2:3003`):

- **batch 32/32**;
- the peer logged `received 9353 bytes` and `PASS: 9353 bytes byte-exact`;
- the TSO re-run fails **by design**, exactly `CONNECT` (`errno=61`, because
  the one-shot listener was consumed by the batch run) and its dependent
  `CLOSE`: 18/20, `JOB02275`.

## P6: mbt 3 deploy against `NSF.LINKLIB`

- **Space check.** mbt 3 checks free space before copying and refuses with
  nothing changed. `NSF.LINKLIB` was 95% full, so a member-replace deploy would
  not have fitted: under mbt 2 the library was deleted and received fresh every
  time, so it never needed room for two copies.
- **`--reallocate`** deletes and reallocates, so it needs the library free,
  like mbt 2's deploy did.
- **Not measured: a member-replace deploy while an STC holds the library.**
  No STC held it here, by design. The CLAUDE.md *(mbt 2)* rule stays as written
  until that case is run.

## STCs and stops

| STC | Name | Stop |
|---|---|---|
| STC00699 | NSFV | `NSFV095I SVC 239 RESTORED`, `NSFV011I`, `IEF404I` |
| STC00700 | NSFS | `NSF043I SVC 239 RESTORED`, `NSF853I ... OKSWAP`, `NSF011I`, `IEF404I`, **no `NSF054W`** |
| STC00701 | NSF | `NSF830I`, `NSF011I`, `IEF404I` |

**No `IEA995I` (no dump).** The two console lines
`ABEND S0C4 detected for module CTHREAD` (15.57.31/32, `JOB02271`) are
TSTCTHR's TEST 3, which forces a fault in a subtask under its ESTAE, once per
batch and TSO step. It passed: "ESTAE subtask ended + joined (parent isolated
from the fault)".

The stand was left with no NSF STC running (`D A,L`) and the host peer
removed. `NSF.LINKLIB` now holds the 2.x build; `NSF.V1X.LINKLIB` holds the
1.x build it replaced.

## A finding outside the port: TSTVSNP is stale

TSTVSNP (`JOB02277`) **fails**: 12/14 in batch and TSO. The only failing
assertion is the one that pins the libc370 defect, "last byte of the target
region is DATA, not NUL" (`got 0, want 198`). Every other assertion passes,
including the hard write bound and the C99 return value.

libc370 fixed the defect in **1.0.3** (2026-08-23, #128, "vsnprintf reserves
the final byte for the terminator"). That is long before this port, so the
test has been stale since 1.0.3 and evidently not run since. The port does
not cause it. `nsf_vsnprintf`/`nsf_snprintf`, the wrappers ADR-0026 put in
front of it, stay correct on the fixed library: **TSTFMT 28 PASS / 0 FAIL**,
batch and TSO (`JOB02279`). Left unfixed here. Updating TSTVSNP, and the
ADR-0026 / CLAUDE.md §3 sentence that says libc370 does not terminate, is
its own change.

## Does NOT establish

- **The edited, device-gated tests:** `tsthio`, `tstctcm`, `tsttcp{d,h,r,w}`,
  `tstudpm`, `tstezam`, `tstezat`, and `tstd1r`. Cross-compile only, see
  `predictions.md`.
- **NSFECHO and NSFTECHO.** They call `nsfthr_setup()` too; they were
  deployed and not run.
- **mbt 3 deploy against a held library** (P6).
- **libc370 behaviour outside what these gates touch.** These include:
  - stdio locking with threads (2.5.0/2.6.1);
  - `cthread_detach()` now asking the TCB (#431);
