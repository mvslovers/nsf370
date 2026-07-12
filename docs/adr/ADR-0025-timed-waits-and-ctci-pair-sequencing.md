# ADR-0025 — Timed cross-task waits (timeout ECB in the waitlist) and CTCI pair sequencing (read re-arm behind the write pipeline)

**Status:** Accepted (2026-07-12), from the resolution of issue #21 (CTCI write
latency band + burst-tail stall), proven live on MVSCE.
**Relates to:** ADR-0022/0023 (CTCI subtask model), ADR-0019/0020 (EXCP recipe /
framing, unchanged), §5.3 (executive loop), §9.3 (CTCI), CLAUDE.md §3.
**Corrects:** ADR-0023 §6, claim (1) — see Decision 1 below.

Issue #21 presented as one symptom pair (bimodal reply latency + the last frame
of a burst stalling until later traffic) but decomposed into **three** distinct
defects, each fixed and proven separately. Decisions 1 and 2 are the lossy-wake
class the issue predicted; Decision 3 is the transport mechanism the live gate
uncovered underneath them.

## 1. A timed ECB wait must include its timeout ECB IN the WAIT list

libc370 `ecb_timed_waitlist(waitlist, timeecb, bintvl, postcode)` arms a
`STIMER REAL` whose exit **POSTs `*timeecb`** — and then issues
`WAIT ECBLIST=(waitlist)` **on the caller's list as given** (read from
`libc370/src/clib/@@ecbtwl.c`). A POST of an ECB that is not in the waitlist
never satisfies the WAIT. Therefore:

- `nsfthr_timed_wait` / `nsfthr_join` (through M2) passed a separate stack
  `tmo` **outside** the list — their timeout posted a dead ECB and the "timed"
  wait was a **pure infinite WAIT**. The CTCI subtask's 500 ms self-poll
  (`wait_or_stop`) was dead code, so any lost wake was permanent, and a
  `nsfthr_join` of a hung subtask would have hung shutdown forever.
- `ecb_timed_wait` / `cthread_timed_wait` "work" only because they pass the
  SAME ECB as list entry and timeecb — at the cost of a phantom POST of the
  target ECB on timeout (and `cthread_timed_wait` clears it), the exact hazards
  ADR-0023 §5 rejects for the re-check loop.

**Decision:** keep the separate timeout ECB, and put it **in** the waitlist:
`WAIT ECBLIST=({target, tmo|VL})`. On timeout the exit posts `tmo`, the WAIT
wakes, and the target ECB is left EXACTLY as the real poster left it — never
cleared, never phantom-posted. Applied to both `nsfthr_timed_wait` and
`nsfthr_join` (src/nsfthr.c).

**Proof (test/mvs/tstthrw.c, live batch + TSO, CC 0):** the old shape on a
cthread subtask did NOT return in 2 s of heartbeats and only returned on a real
release post 2003 ms later; the fixed shape returned in **500 ms** on a subtask
AND on the main task, target ECB untouched; a join of a live (parked) subtask
timed out to RETAIN instead of hanging. The old shape stays in the test as a
verbatim local copy so the seam contract stays pinned.

**Correction to ADR-0023 §6 (1):** "its STIMER-exit timeout does not fire on
the CRT main task (it does on cthread subtasks)" was a **misdiagnosis** of this
bug. The exit fires on any task (T3 above); the executive experiment used
`ecb_timed_waitlist` with the timeout ECB outside the list (no wake), while the
Stage-0 subtask naps used `cthread_timed_wait` (same-ECB trick — wakes). The
**operative part of ADR-0023 §6 stands unchanged**: the executive WAIT stays a
plain `ecb_waitlist`, and the TTIMER-CANCEL/STIMER-singleton constraint (timed
waits on the executive only outside the heartbeat window) remains in force.

**Incidental, worth pinning:** a satisfied multi-ECB WAIT leaves an RB-address
remnant (sans wait/post bits) in the un-posted ECBs of the list. Code must test
`ECB & POSTED`, never ECB non-zeroness. All NSF code already complies.

## 2. The executive rechecks device work before committing to WAIT

The §5.3 loop's WAIT-skip condition checked only the loop's own queues
(`evq`/`xq`). Device completion flags (`rready`/`wready`, the doneq) were
checked only inside the per-pass `poll_input` — so a completion handed up
between a pass's poll (which resets `dev->ecb`) and the WAIT commit relied
entirely on the surviving ECB POST. That is the same reset-window class
ADR-0022 closed for the doneq (UFSD's reset-then-recheck of `req_head`).

**Decision:** `DEVIO` gains a **side-effect-free `pending` probe** (CTCI:
`rready || (wready && txbusy)` — mirroring service's consume conditions
exactly, so reported work is always consumed); `nsfdev_work_pending()`
(alias `NSFDPEND`) covers DEVIO devices and the default doneq model;
`evt_set_devices` carries it as a fourth hook; `evt_mainloop` consults it in
the WAIT-skip condition. A completion whose wake is lost anywhere around the
reset window is serviced on the same pass. Host-proven with no timer running
(TSTCTCI: destroyed-wake reap; loop-consults-probe with a fake DEVIO device),
so the reap cannot be a heartbeat rescue.

## 3. CTCI pair sequencing: the read re-arm is held behind the write pipeline

With Decisions 1+2 deployed, the live gate still failed — and thereby isolated
the transport mechanism: **a WRITE SIO issued while the blocking READ is
outstanding does not execute until the next inbound frame completes that
READ.** The subchannel pair (e.g. 0500/0501) shares one channel; the queueing
is above the Hercules device handler (`ctc_ctci.c` `CTCI_Read` holds no lock
across its data wait and `CTCI_Write` takes none — read from the running
Hercules source on the target host), i.e. at the IOS/logical-channel level.

Live evidence (MVSCE, real pair): slow replies tracked the **sender's
interval** exactly (505 ms at `ping -i 0.5`, 2020 ms at `-i 2` — not the 100 ms
heartbeat, not the 500 ms self-poll); tcpdump showed each stuck reply hitting
the wire ~200 µs **after the next echo request**; the last reply of a run never
transmitted (the 90 s+ burst-tail stall of #21, `LNK1 out` = N−1). This also
re-explains the pre-fix M2 numbers: the "bimodal 200-311 ms band" was that
run's ping interval, not heartbeat multiples.

**Decision:** sequence the pair in the executive's DEVIO seam. `service` only
**marks** the read release (`CTCIDEV.rhold`, executive-only); `kick` POSTs
`returnecb` only when **nothing is queued and no WRITE is outstanding**. Every
WRITE is therefore issued with the READ parked and executes immediately; the
un-armed read window is lossless (Hercules buffers/back-pressures inbound with
no READ outstanding, §9.3 — the same property ADR-0023 §2 already relies on).
`kick` also walks past dropped frames instead of stranding the rest of the
sendq until the next wake. PBUF ownership and the kick-clocked write handoff
(ADR-0023 §4) are **unchanged** — this is a re-ordering of one executive-side
POST, not a completion-model change.

**Known limitation (M3+ follow-on):** a WRITE that originates while the READ is
already armed — locally-originated traffic (TCP retransmit, UDP send), as
opposed to the M2 receive→reply pattern — still queues behind the READ until
the next inbound frame. The follow-on options are HIO (halt the outstanding
READ, write, re-arm) or an attention-driven read protocol; the ADR-0023
ping-pong/free-buffer-queue throughput follow-on is now explicitly
**conditional on the same solution** (keeping a READ permanently outstanding
would reintroduce the stall for every write). To be designed before the first
locally-originated transmit path lands (M3 UDP).

## Rejected

- **cthread_timed_wait as the subtask timed-wait primitive:** clears the ECB
  and phantom-posts on timeout — loses/forges completions in `wait_or_stop`
  and turns a join timeout into a false "task ended" (ADR-0023 §5 stands).
- **Making the write path self-clocked** (issue #21's floated direction): would
  move PBUF handling onto the subtask against §3/§9.2 single-owner rules, and
  would not have fixed the transport mechanism anyway (the stall is in the
  channel, not the wake).
- **Re-genning the target's channel model:** NSF must run on stock
  TK4-/TK5/MVSCE systems; the driver adapts to the channel, not vice versa.

## Validation

- TSTTHRW (MVS, batch+TSO CC 0, 32/32): Decision 1 both ways, on both task types.
- TSTCTCI (host, 190→207): destroyed-wake reap + loop-probe (Decision 2),
  deterministic rhold/returnecb ordering (Decision 3); suite 804/804.
- Live gate (MVSCE, S NSF on the real 0500/0501 pair): see issue #21 close-out —
  1000/1000 replies including the last frame, unimodal sub-2 ms RTT,
  `LNK1 out` = 1000, clean `P NSF`.

## Sources

`libc370/src/clib/@@ecbtwl.c` / `@@ecbtw.c` / `@@cttwat.c` (the timed-wait
mechanism); `~/hercules/hyperion/ctc_ctci.c` on the target host (the running
Hercules source; read-wait lock discipline); live bisection and tcpdump/ping
measurements on MVSCE (`mvsdev`); ADR-0022/0023; issue #21.
