# ADR-0019 — CTCI I/O completion via EXCP + IOB ECB in the ECBLIST (no CHE appendage)

**Status:** Accepted (2026-07-09), ratified with the maintainer. Corrects the
top-half description in §9.3 and supersedes one forward-looking clause of
ADR-0017 (see "Relation to ADR-0017").
**Relates to:** §9.2 (DEVOPS / NETDEV), §9.3 (CTCI driver), §5.3 (executive
loop / ECBLIST), ADR-0017 (timer wakeup), CLAUDE.md §3.

## Context

§9.3 originally specified the CTCI top half as "EXCP … I/O completion exit
pushes the IOELEM onto `doneq` (xq_push) and POSTs the device ECB". Before
writing that exit we went looking for the proven convention, as the project does
for every MVS seam (issue #8 → libc370 `@@getclk`; the STIMER exit → GC28-0683).

Two findings changed the design:

1. **There is no user-written "I/O completion exit" in EXCP.** When the channel
   program terminates, EXCP/IOS posts the ECB addressed by `IOBECBPB` in the IOB.
   That post *is* the completion notification. The only user code IOS will call
   are the appendages (SIO, PCI, EOE, CHE, ABE), and the CHE appendage's **normal
   (+0) return** is precisely "channel program posted complete, RQE made
   available". The appendage exists to *override* that default, not to produce it.

2. **A working 3.8j precedent confirms it.** `mvslovers/mvs38j-ip`
   (`src/arch/s370/mvsasm/igg019x8.asm`) does install a CHE appendage — but its
   own closing comment explains why: it hand-POSTs a *separate* ECB because the
   Xinu scheduler (`nulluser`/`iohook`) waits on one global ECB and needs a
   footprint flag. It notes the IOB ECB is not waited on at all; IOS merely fills
   in a descriptive post code via the +0 return.

NSF has no such need. The §5.3 executive loop already WAITs on an ECBLIST of
per-device ECBs. It can wait on the IOB ECB directly.

## Decision

**Option A.** The CTCI driver issues plain `EXCP` and lets IOS post the IOB ECB,
which is the device ECB already carried in `NETDEV` and already in the loop's
ECBLIST. No appendage is written, installed, or authorized.

Recipe (verified against `mvs38j-ip` `mvsctc.asm` / `ctcpostread.c`):

- Two DCBs, `DSORG=PS,MACRF=E`, one per subchannel (read CUU / write CUU),
  `IOBAD=` pointing at the IOB. **`CENDA=` is deliberately omitted** — naming an
  appendage there is what would activate one.
- OPEN the read DCB `INPUT`, the write DCB `OUTPUT`.
- Channel programs are single, unchained CCWs: READ `X'02'` with the **`SLI`
  flag** (an inbound block is shorter than the buffer; without SLI it raises
  incorrect-length / unit check), WRITE `X'01'`.
- Per request: clear the IOB, `IOBFLAG1 = IOBUNREL` (unrelated request), then
  store ECB / channel-program / DCB addresses into `IOBECBPB`, `IOBSTRTB`,
  `IOBDCBPB` (mapping `IEZIOB`), and issue `EXCP`.
- Completion: the loop wakes on the device ECB. Post code `X'7F'` means the
  channel program completed normally. **Bytes transferred = requested length −
  IOB residual count** (from the CSW).
- Buffer sizing must respect the MVS 3.8j maximum I/O length as well as the
  Hercules buffer bounds from §9.3.

The HLASM top half therefore runs entirely in the mainline: `EXCP` starts an I/O
and returns; nothing executes in exit state. `doneq` is not used by the CTCI
driver.

## Rejected for v1: Option B — a CHE (channel-end) appendage

Rejected because it buys latency/throughput, not correctness, and costs a system
install. Recorded here in full so it need not be re-derived if we later measure a
need (e.g. READ re-drive latency, or loss under load).

An appendage would let us re-drive the next READ from inside IOS at channel end
(`R14+8` re-EXCP), instead of after a task switch back to the executive.

**Entry state** (source: OS/VS2 SPL: Data Management, "Entry Points, Returns, and
Available Work Registers for Appendages"; transcribed in `igg019x8.asm`):

| Register | On entry |
|---|---|
| R1  | → RQE |
| R2  | → IOB |
| R3  | → DEB |
| R4  | → DCB |
| R7  | → UCB |
| R13 | → a 16-word work area for our use |
| R14 | return address (see below) |
| R15 | appendage entry point |

Return by branching to a displacement off R14: **+0** normal (channel program
posted complete, RQE made available) · **+4** skip (not posted, RQE made
available) · **+8** re-EXCP (not posted, RQE requeued for retry; the IOB must be
reinitialised first) · **+12** bypass (not posted, RQE not made available).

**Restrictions:** load module name `IGG019xx`, `xx` in `WA`–`Z9` (installation
range); runs in supervisor state; **no SVCs** or state-changing instructions
(WTO, LPSW); no loops testing for I/O completion; no alteration of supervisor or
IOS storage; if R9 is used it must be zeroed before returning. R10–R13 are work
registers needing no save/restore.

**Posting from an appendage** must therefore use **branch-entry POST**, not the
SVC: load the entry point from `CVT0PT01` off the CVT, set R10 = post code,
R11 = ECB address, R12 = TCB address (from `DEBTCBAD`), then `BALR R14,R15`.

**Install cost:** the module must be link-edited `RENT,REUS,REFR` into
`SYS1.SVCLIB` and authorised via `IEAAPP00` in PARMLIB. Every appendage change is
a system-library install, not a rebuild-and-rerun. A program check inside an
appendage abends **SA00**; `TCBEXCPD` (TCB offset `X'C0'`) locates the EXCP
diagnostic area whose flag byte identifies the failing appendage.

### What Option B would actually buy — and an honest risk calibration

Under Hercules the WRITE completes essentially at host memory speed, while the
READ blocks inside the emulator until a frame arrives. An appendage would
therefore save no channel latency; the *only* thing it closes is the gap between
an I/O completion and the next `EXCP`. And that gap costs nothing: Hercules
buffers arriving frames whether or not a READ is outstanding, back-pressures its
own reader thread when full (~100 µs retry) instead of discarding, and raises
neither attention nor unit check. Ping-pong buffers plus re-drive-before-parse
(§9.3) shrink the gap to a few instructions regardless.

A common argument against appendages is that a failure there takes MVS down. That
is overstated for 3.8j: the `igg019x8.asm` author records that MVS coped with an
appendage ABEND (SA00) better than he expected. The risk is real in kind —
supervisor state, key 0, free to alter IOS storage — but the case against Option B
should rest on install cost, dev-loop friction and the absence of any correctness
benefit, not on a catastrophe claim we cannot substantiate.

### Why mvs38j-ip needed an appendage, and why NSF does not

Worth recording, because the obvious objection to Option A is "the mvs38j-ip
author paid the SVCLIB cost, so there must be a reason." There was — but it is a
**Xinu reason, not an EXCP reason**.

`igg019x8.asm` does two things a plain ECB post cannot: it sets a footprint bit
(`CTIOCOMPLETE`) in the device's `ctdev`, and it passes the `ctdev` address as
the POST code. `iohook`, called from Xinu's `resched`, then walks the device
chain, finds the bit, and `resume`s the owning Xinu process. That is necessary
because **Xinu's dispatcher is not ECB-driven**: an MVS POST wakes the MVS *task*,
but Xinu must still determine which of its *internal* processes became runnable,
and only a memory flag can tell it that. Compounding this, `nulluser` waits on a
fixed, global two-ECB list (`nullecb`, `ctcecb`); per-IOB device ECBs do not fit
a static list that would have to be rebuilt as devices open and close.

Note also that this wiring is visibly unfinished — the project was abandoned in
2002/03. The appendage POSTs the per-device `ctdev.ecb`, which is not in
`nulluser`'s wait list; the global `ctcecb` that *is* waited on is never posted
anywhere in the tree; the wake actually comes via `nullecb`, and the footprint
bit is the real signal. The author hedges in his own comment about what the +0
return guarantees.

NSF's executive is **already ECB-driven**: the §5.3 loop WAITs on an ECBLIST that
contains the per-device ECBs and, on waking, identifies the completed device by
scanning that list. No footprint, no POST-code trick, no global ECB. The
appendage's entire purpose was to bridge EXCP's ECB model to a cooperative
scheduler that we deliberately did not adopt.

**Therefore mvs38j-ip is authoritative for us on MVS *convention*, not on
*design*.** We take from it the appendage entry/return table, branch-entry POST,
the DCB/IOB/EXCP recipe, `SLI`, post code `X'7F'` and the residual-count
arithmetic. We do not take its ECB topology or its process model.

One detail we *do* adopt: **the ECB must be cleared before every `EXCP`**
(`mvsctc.asm` clears the whole IOB per request). Otherwise the loop observes a
stale posted bit and processes a phantom completion.

## Relation to ADR-0017

ADR-0017 chose the async STIMER-REAL exit over a timer subtask, and part of its
rationale was that the M1 device I/O exits would be "the same async class", so
mastering the exit-entry convention would pay forward. **That clause does not
hold** — EXCP has no such exit, as established above. ADR-0017's decision itself
stands unchanged: it is implemented, runtime-validated on MVS (10 heartbeats,
mean 100.2 ms), and NSFTMEXP remains the project's only OS-invoked async exit.

## Consequences

- No `SYS1.SVCLIB` install, no `IEAAPP00` authorisation, no restricted-state
  code, no APF authorisation. NSF stays a plain problem-state started task, and
  plain `EXCP` (not `EXCPVR`, which would require authorisation and a PGFX
  appendage) handles page-fixing and translation for us.
- M1-3 loses its highest-risk element. The top half is mainline HLASM: build the
  channel program, fill the IOB, `EXCP`, return.
- `doneq` (the `XQ` in `NETDEV`) is **no longer required by the CTCI driver**. It
  remains the right mechanism for `NSFHOST`, whose reader thread is a genuine
  concurrent producer, and stays in `NETDEV` as a per-driver facility. Drivers
  that complete via an ECB simply leave it empty.
- The device CUUs still have to be allocated. Either DD statements in the STC
  PROC (as `mvs38j-ip` does with `CTCREAD`/`CTCWRITE`), or dynamic allocation
  from the `DEVICE` statement in PROFILE.TCPIP via the libc370 SVC 99 seam. To be
  settled in M1-3; DD statements are the lower-risk starting point.
- The driver must clear the device ECB before each `EXCP` (see above); this is a
  correctness requirement of Option A, not a style choice.
- The read path uses **ping-pong device I/O buffers** and re-drives the READ
  *before* parsing the block just received (§9.3). One IOB and one ECB suffice —
  only one READ is outstanding; the driver alternates the CCW data address.
- Each inbound `CTCISEG` payload is **copied into a PBUF**. Zero-copy would have
  to hand the 20 KB I/O buffer up as a PBUF, breaking both the size classes and
  ping-pong; it is out of scope for v1.
- The configured MTU must satisfy `MTU ≤ MAX_CTCI_FRAME_SIZE` and `MTU ≤ 9000`,
  or Hercules silently discards the frame.
- If throughput later demands it, Option B is a contained, well-documented change
  behind the same `DEVOPS` contract: add `CENDA=` to the DCB and install the
  appendage. Nothing above the driver changes.

## Sources

- OS/VS2 MVS SPL: Data Management, GC26-3830 (Rel. 3.8) — appendages, the
  authorized appendage list, EXCP control blocks.
- `mvslovers/mvs38j-ip`, `src/arch/s370/mvsasm/{igg019x8.asm,mvsctc.asm}`,
  `src/arch/s370/ctc/ctcpostread.c`, `src/arch/s370/jcl/linkche.jcl` — a working
  MVS 3.8j EXCP CTC driver; the MVS convention is taken, the Xinu architecture
  around it is not.
- Hercules `ctc_ctci.c` / `ctcadpt.h` — the wire format, already normative in
  §9.3 (M1-1).
