# #83 live round: recovery and the startup failure stop freeing under I/O

Written 2026-10-15, **before any deploy**, and not edited afterwards. The
record goes in a separate file beside this one. Survey:
`../83-survey/survey.md`. Fix shape (maintainer): A + C, with D as the
backstop.

**Builds:**
- **RED** = `main` at d350bd0 plus the temporary injection;
- **GREEN** = branch `m5-83-recovery-free` plus the same injection;
- **FINAL** = the branch with no injection.

The injection is never committed. It is the first statement of
`nsfsx_stats_extra` (`F NSFS,STATS`):
`if (!__issup()) *(volatile unsigned *)0 = 0;` — a store into page 0 from
problem state, key 8, so `S0C4`, as in #79. Its marker in the source is
`NSF83INJ`, and the FINAL build is checked for its absence.

All runs need the CTCI pair up (`NSF210I`/`NSF211I`). A run with the pair down
measures nothing here, because the defect needs outstanding I/O.

## Q0. Each deploy took effect

- RED: `NSF.LINKLIB(NSFS)` contains `NSF061I` (main has d2) and **not**
  `NSF068W`.
- GREEN and FINAL contain `NSF068W`. That string exists only on the branch.
- A GREEN or RED `F NSFS,STATS` abends. A FINAL one does not.

## Q1 (RED). P3, the startup failure, reproduces

`S NSFV` first (SVC 239 taken), then `S NSFS`:
- `NSF210I`/`NSF211I` (devices up);
- `NSF049E SVC 239 IN USE ... NOT STOLEN`, then `NSF009E NSFS TRANSPORT
  INITIALIZATION FAILED`;
- **`IEA700I A0A-  2 NSFS NSFS 00 0000A040 ...`**, then `IEF450I NSFS NSFS -
  ABEND SA0A U0000`.

The length field `0000A040` is the survey's arithmetic for the CTCIBUF
region. The address and TCB fields are recorded and not predicted.

**Falsified if** NSFS ends RC 8 with no abend, or `A0A` comes with a
different length. Then the region identification of the survey is wrong.

Afterwards `P NSFV`. `nsfs` stays ABSENT: the failing start freed its anchor
before registering.

## Q2 (RED). P1, recovery, reproduces

`S NSFS`, then `F NSFS,STATS`:
- `NSF900E`;
- `NSF902I RECOVERY ENVIRONMENT: SUP=N AUTH=Y`;
- `NSF903I NSFS TRANSPORT QUIESCED BY RECOVERY -- SVC RESTORED, CSA AND
  ROUTER RETAINED`;
- **`IEA700I A0A-  2 ... 00 0000A040 ...`**, then `IEF450I ... ABEND SA0A`;
- **no `NSF901I`**.

The anchor is retained and chained under `nsfs`.

**Not the gate, recorded only:** the next `S NSFS` adopts that anchor (d2:
`NSF061I`, inflight 0, so the router is freed with `NSF065I`), and `P NSFS`
takes the free path. The ESTAE retain round of ADR-0048 is a round of its own
(maintainer, 2026-10-15). Here it is only the way back to a clean stand.

## Q3 (GREEN). P3 fixed (C)

As Q1:
- `NSF049E`, then `NSF009E`;
- **no `IEA700I`, no abend**;
- no `NSF068W`, because the devices were quiesced first, so no device is
  busy;
- `IEF404I NSFS - ENDED`, with step CC 8.

`NSF206W` would mean a subtask did not join. In that case `NSF068W` follows
and the regions are left to MVS, which is D working. That outcome is a pass
of D, not of C, and the record says which one occurred.

## Q4 (GREEN). P1 fixed (A)

As Q2:
- `NSF900E`, `NSF902I`, `NSF903I`;
- **`NSF901I NSF EMERGENCY TEARDOWN COMPLETE`**;
- **`IEF450I NSFS NSFS - ABEND S0C4 U0000`**: the original abend percolates;
- **no `IEA700I`, no `A0A`**.

Then, recorded only, as in Q2: the next `S NSFS` adopts the anchor, and
`P NSFS` takes the free path.

**Falsified if** `A0A` appears, or `NSF901I` does not.

## Q5 (FINAL). No regression

- `S NSFS`: `NSF001I`. `F NSFS,STATS` answers `NSF810I`/`NSF811I`, with no
  abend.
- `mbt test --mvs --only TSTRQXC --only TSTRQXF`: **130 PASS / 0 FAIL**.
- `P NSFS`: `NSF043I`, `NSF044I`, `NSF011I`, **no `NSF068W`**. The clean stop
  quiesces first, so no device is busy and the regions are released as
  before.
- **Phase 1:** `S NSF` / `P NSF`: `NSF001I`, then `NSF011I`, no `NSF068W`.
  NSF and NSFS share the CTCI pair, so they run one after the other.

## End state

- No NSF STC running; `nsfs` ABSENT; SVC 239 restored.
- No NSF anchor left in CSA. The two recovery anchors are adopted and freed.
- `NSF.LINKLIB` restored from the backup and verified by content.
- No IEA995I. ABENDs only where predicted: Q1 and Q2 `SA0A`, Q4 `S0C4`.

## Not run, and why

- **P2** (Phase-1 recovery): the same code change as P1, in `nsfmain.c`, but
  no NSF injection is built. Host- and source-pinned only.
- **P4** (a subtask that will not join): no cheap provocation exists.
  `dev_busy_count` is host-pinned in TSTDEV scenario 4.
- **The ESTAE retain round of ADR-0048**, with its own predictions. It is a
  separate round.
