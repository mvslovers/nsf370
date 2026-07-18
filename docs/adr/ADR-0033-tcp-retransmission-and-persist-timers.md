# ADR-0033 — M4-4 TCP retransmission (fixed RTO + exponential backoff) and zero-window persist probes

**Status:** Accepted (2026-07-17). Turns the M4-3 copy-on-transmit data path
(`src/nsftcp.c`, ADR-0032) into a stack that survives loss and a zero-window
peer: a retransmission timer re-sends the oldest unacked segment on a fixed RTO
with exponential backoff, and a persist timer probes a closed send window.
**Fixed RTO only** — Karn's algorithm and adaptive RTT (`srtt`/`rttvar`) are M5
(spec §13.1). **No fast retransmit** — the `dupacks` field counts duplicate ACKs
but nothing acts on it until M5.
**Relates to:** spec §13 (TCB timers/counters), §6 (NSFTMR: 100 ms tick, embedded
TMRs, arming cannot fail, `tmr_cancel` idempotent), ADR-0032 (the copy-on-transmit
seam this drives with a timer), ADR-0031 (TIME_WAIT keeps only `t_2msl` armed; the
teardown checklist that already cancels all four timers), RFC 793 §3.7, RFC 1122
§4.2.2.17 (zero-window probing) and §4.2.3.1 (one segment per RTO).

## Context

M4-3 built the sndq specifically so a retransmission is "emit the SND.UNA slice
again" — data lives on the sndq until ACKed, so no new buffer machinery is
needed, only timers and policy. Four decisions had no obvious answer from the
existing code.

## Decision

### 1. One timer choke-point; the rexmit / persist mutual exclusion

A single reconcile function `tcp_timers_update(tcb)` runs after every event that
moves `SND.UNA` / `SND.NXT` / `SND.WND` / the sndq (the `tcp_output` tail, the SYN
emitters, and `tcp_process_ack`). It arms **exactly one** of the two timers:

- **`SND.UNA < SND.NXT` (sequence space in flight)** → the retransmit timer
  governs; persist is cancelled. This covers data, a SYN (SYN_SENT / SYN_RCVD get
  rexmit for free), and a FIN uniformly.
- **nothing in flight, but unsent data behind a zero window
  (`sndq_bytes > 0 && snd_wnd == 0`)** → the persist timer governs.
- **otherwise** → both off.

The function is idempotent ("arm only if idle"), so a doubled call in one event
never restarts a running timer; a progress ACK forces a fresh RTO by cancelling
`t_rexmit` first (in `tcp_process_ack`) so the reconcile re-arms at the base
interval (RFC restart-on-ACK).

The two timers are **never armed together**. This is not just tidiness: it is what
makes the give-up teardown safe to free the TCB from inside a timer callback.
`nsftmr_run` detaches + IDLEs the firing timer before calling it, and the mutual
exclusion guarantees no *sibling* timer of this TCB is queued (persist is off when
rexmit fires and vice-versa; `t_keep` is M5; `t_2msl` is TIME_WAIT-only and a
TIME_WAIT TCB carries no unacked data). So `tcp_conn_abort` → `soc_destroy` →
`tcp_destroy` (which frees the TCB holding the timers) is safe, exactly as
`tcp_2msl_expire` already relies on (ADR-0031).

### 2. Retransmit EXACTLY ONE segment (go-back-N restraint)

On a rexmit expiry `tcp_rexmit_one` re-sends the single oldest outstanding
sequence starting at `SND.UNA` (RFC 1122 §4.2.3.1 — never re-blast the whole
flight): a SYN / SYN|ACK re-emitted through the control path (`tcp_emit`, seq =
ISS = SND.UNA), or `min(mss, sndq_bytes)` data bytes copied from the sndq at
offset 0 (`tcp_data_emit`), or — data all acked — the FIN. `SND.NXT` is never
moved by a retransmit (the sequence is already counted). A retransmitted segment
is byte-identical in payload and sequence to the original; only the ack/window
fields reflect newer state (asserted in `test/tsttcp.c`).

**FIN retransmission** re-emits at the FIN's own sequence (`SND.UNA`, ==
`SND.NXT-1` once data drained) via a new `tcp_emit_seq(…, seq)`, and
`TCB_F_FINSENT` **stays set** — the flag means "the FIN occupies sequence space",
not "sent once", so the re-emit must not re-increment `SND.NXT`. (`tcp_emit`
becomes a thin wrapper over `tcp_emit_seq` computing the natural seq; every
existing caller is unchanged.)

### 3. Fixed RTO with backoff; give-up; the shared `backoff` field

The base RTO is `NSFTCP_RTO_TICKS` (10 ticks = 1 s at the 100 ms tick), doubled on
each no-progress expiry (`rto = base << backoff`), capped at `NSFTCP_RTO_MAX_TICKS`
(640 = 64 s). The shift is clamped at 6 (`10 << 6 = 640`) so a large backoff never
triggers shift-UB. `srtt`/`rttvar` stay 0 (M5). Because the two timers are
mutually exclusive, they **share** the `backoff` field (consecutive-expiry count =
interval shift) and the `rto` field (current interval).

After `NSFTCP_RTO_MAXTRIES` (8) no-progress rexmit expiries the connection is
declared dead: `tcp_conn_abort(tcb, NSF_ETIMEDOUT)` completes every parked request
with `NSF_ETIMEDOUT` (the pend slot cleared first, so `soc_destroy` does not
re-complete it with the generic `ECONNABORTED` — the `tcp_do_reset` pattern,
refactored to share `tcp_conn_abort`), then drives the ADR-0031 end-of-life path.
A SYN_SENT give-up is the classic connect timeout; a SYN_RCVD give-up reclaims the
embryonic child. A progress ACK resets `backoff`/`rto` to base and `dupacks` to 0.

### 3a. Window update must precede the send (an over-send fix found live)

The live persist gate surfaced a latent **M4-3** flow-control bug in
`tcp_process_ack`: the progress branch advanced `SND.UNA` and immediately
re-clocked the sender (`tcp_send_resume` → `tcp_output`) **before**
`tcp_update_window` ran. With the window still stale, advancing `SND.UNA` past a
peer's **shrunk** window slid the right edge (`SND.UNA + SND.WND`) rightward, and
the sender transmitted beyond it. On the wire: the guest sent a segment past the
peer's advertised window, the peer dropped it and advertised `win 0`, and the
guest retransmitted — surfacing as **rexmit instead of persist**. The bug hid on
the host until now because it only fires on the **parked-send** path
(`send_resume` early-returns when `pend_send == NULL`), which M4-3's tests never
combined with a shrinking window, and M4-3's live gate had the guest *receiving*,
never sending under flow control. Fix: `tcp_update_window` moves **before** the
`SND.UNA` advance / any transmit (RFC 793 p.72 step 5 order). Reproduced +
guarded by `test_flowctl_no_oversend` (fails pre-fix: over-sends 1024 bytes).

### 4. Persist: never gives up while the peer answers (the documented choice)

On a persist expiry `tcp_persist_expire` sends ONE byte of sndq data beyond the
closed window at `SND.NXT`, counts `wndprobe`, backs off, and re-arms. The probe
does **not** advance `SND.NXT` (the byte stays on the sndq and is sent for real
once the window reopens), so the send bookkeeping keeps "nothing in flight" and
persist stays the governing timer — the invariant in decision 1 holds.

Persist and rexmit disagree on give-up, and the reconciliation is the one place
M4-4 deviates from a naïve "give up at MAXTRIES":

- RFC: a zero-window connection is alive as long as the peer ACKs probes, so
  persist must never give up on it. But an all-silent peer must not be probed
  forever.
- **The distinguishing signal is whether *any* segment arrived** since persist
  armed, tracked in a new flag bit `TCB_F_PROBEACK` (set on every
  sequence-acceptable inbound segment — a zero-window ACK proves the peer is
  alive). `backoff` still grows on every probe (so the intervals **visibly** back
  off — the live tcpdump shows 1 s, 2 s, 4 s … even while the peer keeps ACKing),
  but the give-up (`backoff >= MAXTRIES`) only fires when `TCB_F_PROBEACK` is
  clear — i.e. the peer sent nothing at all through the whole escalation, at which
  point persist falls back to the rexmit give-up discipline (`NSF_ETIMEDOUT`).

This is the choice the task calls out: it keeps a genuinely-alive zero-window
connection open indefinitely (probing at the 64 s cap) while still reaping a dead
one, at the cost of one flag bit and *not* resetting `backoff` on a zero-window
ACK (which would hide the backoff the live test is meant to show).

## Consequences

- **Fixed RTO, no Karn / adaptive RTT (M5):** the RTO is a constant 1 s base, not
  measured. On the lossless CTCI link (the M2 gate) the RTO never fires, so this
  is invisible in production but host-proven.
- **No fast retransmit (M5):** `dupacks` counts a duplicate-ACK run but nothing
  triggers an early retransmit; the timer is the only retransmit driver in M4.
- **No keepalive (`t_keep`) / delayed ACK / Nagle / oooq (M5).**
- **TCB unchanged (200 B):** the RTO-state fields (`rto`, `backoff`, `dupacks`)
  already existed for M4-4; the only new state is one flag bit (`TCB_F_PROBEACK`).
  No new external symbols — every M4-4 helper is `static`.
- **Live coverage (honest state).** The M4-3 lossless regression re-runs green
  live with `rexmit == 0` (`test/mvs/tsttcpd.c`, batch CC 0). The persist path was
  driven live (`test/mvs/tsttcpr.c`: a guest stream against a host receiver with a
  tiny `SO_RCVBUF` that stops reading → `win 0`): the guest **respects the window**
  (no over-send, the §3a fix), a genuine one-byte zero-window probe fires
  (`wndprobe > 0`), the connection survives, and the full 4 MB transfer completes
  on window reopen. **But the multi-tick backoff cadence is NOT faithfully visible
  live** — probes are sparse and the intervals distorted by a foundational
  executive tick-advance bug (**issue #40**: `nsftmr_run(1u)` per wake while the
  STIMER is armed for the head delta → a delta-N timer fires after N(N+1)/2 ticks;
  this also mis-times the production 2MSL). So the persist **policy + backoff** and
  the rexmit **RTO/backoff/give-up** are **host-proven** (`test/tsttcp.c`,
  deterministic `nsftmr_run`); the live cadence follows issue #40. Genuine-loss
  RTO firing on a truly lossless link needs root — the systematic
  drop/dup/reorder matrix is M4-6.
  **RESOLVED (ADR-0034, issue #40 fixed):** the executive now advances the timer
  queue by the ARMED tick count via `nsftmr_wake` (not `nsftmr_run(1u)`), so a
  delta-N timer fires after N ticks. The persist/RTO backoff intervals are now
  faithfully visible live (delta-20 at ~2.0 s, not 21 s — `test/mvs/tsttmcad.c`),
  and the production 2MSL is 60 s. The host-proven policy/backoff is unchanged.
- `NSF_ETIMEDOUT` (60) is now actively returned (connect and rexmit give-up) —
  noted in `docs/ezasoket-conformance.md`.
- NSFTCP stays OUT of the `NSF` production load module (unreachable until the
  EZASOKET M4 verb set, M4-5); `S NSF` is byte-for-byte unchanged.
