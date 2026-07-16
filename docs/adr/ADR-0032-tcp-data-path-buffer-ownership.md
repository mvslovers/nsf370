# ADR-0032 — M4-3 TCP data path: buffer ownership (copy-on-transmit / trim-in-place), sliding-window flow control, and FIN-after-data

**Status:** Accepted (2026-07-16). Turns the M4-2 connection machine
(`src/nsftcp.c`, ADR-0031) into a data-carrying stack: ESTABLISHED now
segmentizes application sends under the peer's window, delivers in-order receive
data into the socket rxq, and drains queued data before FIN at close.
**Still no retransmission** (M4-4: the CTCI link is lossless, so a lost segment
fails the connection) and **no out-of-order reassembly** (M5: the `oooq` stays
empty — a gap segment is dropped and dup-ACKed).
**Relates to:** spec §13 (TCB sndq/rxq, §13.5 counters), §10.2/10.3 (SOCKCB,
parked requests), §3.4 (single-owner PBUFs), ADR-0028 (checksum seed), ADR-0031
(the connection machine + background close this extends), `src/nsfudp.c`
RQ_RECVFROM (the recv copy-out precedent).

## Context

M4-2 established connections but carried no payload: a data-bearing segment was
processed for its control bits and its data dropped + counted (`datadrop`), and
`nsftcp_input` unconditionally `buf_free`d the inbound PBUF because every handler
read the decoded `TCPSEG`, never the buffer. M4-3 must (a) copy application sends
into a bounded send buffer and clock them onto the wire under the peer's window,
(b) accept in-order receive data and hand it to the app, and (c) do both without
violating the single-owner-PBUF invariant (spec §3.4) on a 24-bit, memory-scarce
machine. Five decisions had no obvious answer from the existing code.

## Decision

### 1. Send = copy-on-transmit (the direction pinned in M4-1, now normative)

`TCB.sndq` holds the application payload as a FIFO of PBUFs (`tcp_sndq_append`),
front byte == `SND.UNA`, total bytes == `sndq_bytes`, bounded by a named send
budget `NSFTCP_SNDBUF` (default 4096). Every (re)transmission builds a **fresh
wire PBUF** and **copies** the slice being sent out of the sndq
(`tcp_sndq_slice` walks the queue and `buf_copyin`s each run into the new PBUF);
the wire PBUF gets its TCP header prepended into the headroom and is handed to
`nsfip_output`, which then owns it. The sndq keeps the data until it is ACKed. No
reference counting: the sndq PBUFs and the wire PBUF are distinct single-owner
objects, so spec §3.4 holds. An sndq PBUF is NEVER handed straight to `dev_send`.

### 2. The recoverability dividend (why M4-3 needs no RTO)

Because a transmission is a *copy*, a transmit failure (`nsfip_output` /
`dev_send` reject, bounded device queue) loses only the wire copy. The data is
still on the sndq, `SND.NXT` is not advanced past the failed slice
(`tcp_output` advances `snd_nxt` only after a successful emit), and the next
event that runs `tcp_output` — an ACK arrival, a window update, or the next
RQ_SEND — retries the same bytes. On the lossless CTCI link (the M2 gate) this is
sufficient for correctness with **no retransmission timer**. It is also the exact
seam M4-4's RTO plugs into: the rexmit timer will call `tcp_output` with
`snd_nxt` rewound to `snd_una`, and copy-on-transmit rebuilds the wire segment
from the retained sndq bytes. This is why the ownership direction is part of the
deliverable, not an afterthought.

### 3. Receive = trim-in-place; the conditional free in `nsftcp_input` (the ownership gate)

An accepted in-order segment is **trimmed in place** — `buf_trim_head` past the
IP+TCP headers (and past any overlap already received, item 5), `buf_trim_tail`
past the advertised window — and queued on the socket `rxq` AS-IS. Ownership
transfers from `nsftcp_input` to the rxq; no copy. RQ_RECV (`tcp_recv`) copies
STREAM bytes out of the rxq PBUFs into the caller's buffer, freeing a
fully-consumed PBUF and `buf_trim_head`-ing a partially-consumed head.

This breaks M4-2's unconditional `buf_free(b)` at the end of `nsftcp_input`: the
accept path now *keeps* `b`. The free is made **conditional on an explicit
ownership flag** threaded down the handler chain (`tcp_state_input` /
`tcp_synchronized_input` take a `PBUF *b` and an `int *kept` out-param). `*kept`
is set to 1 **only** on a successful `q_enq` onto the rxq — so the rxq-full drop,
the pure-duplicate drop, the gap drop, and the header-overlap-then-queue cases
are each unambiguous, and `nsftcp_input` frees `b` iff `!kept`. This is the
double-free/leak class CLAUDE.md §3 calls review-blocking; it is exercised under
ASan/UBSan in `test/tsttcp.c`.

A coherence dividend falls out: if data is dropped (rxq full, or a gap),
`RCV.NXT` is not advanced, so a same-segment FIN's `seq + dlen == rcv_nxt` check
fails and the peer retransmits data + FIN together — correct with no special
case.

### 4. The parked send lives in `SOCKCB.pend_send`, progress in `r->p3`

A blocking RQ_SEND larger than the free budget PARKS and completes (RETCODE =
full byte count) once every byte is copied into the sndq, as ACKs free space. The
deciding constraint for *where* the parked request lives is not aesthetics but
the "no application task WAITs forever" guarantee: `soc_destroy` enforces that
uniformly for the existing pend slots (spec §10.5), so a parked send must be a
peer of `pend_recv`, not TCP-private topology like the acceptq linkage (which
went to the TCB in ADR-0031 because a SOCKCB structurally cannot hold a
queue-of-TCBs — a single request pointer is exactly what SOCKCB already models).

So SOCKCB gains a fourth pend slot `pend_send` (72 → 76 B, still ≤ 128; SOCKCB is
not frozen, only NSFRQE is) and `SOC_PEND_SEND = 3`; `soc_park` / `soc_complete`
/ `soc_destroy` get one additive edit each. The partial-copy cursor (bytes
already copied into the sndq) lives in the request's own `r->p3` (RQ_SEND does
not use it — SENDTO's dest is p1/p2, a stream SEND has no address), which
neutralizes the only real pull toward the TCB. `tcp_do_reset` completes a parked
send with the specific `NSF_ECONNRESET` before `soc_destroy` (mirroring
pend_connect/recv/accept), and `soc_destroy` step 4 completes any residual parked
send with `NSF_ECONNABORTED`. A non-blocking SEND accepts what fits (partial
count) or `NSF_EWOULDBLOCK` if nothing fits — never parks.

### 5. Sliding window, dynamic receive window, window-update-on-drain

- **Usable send window is signed.** `tcp_output` computes
  `usable = (INT)(snd_una + snd_wnd - snd_nxt)` and stops at `usable <= 0`. Raw
  UINT subtraction wraps to a huge value when the peer shrinks its window below
  `snd_nxt`; the signed cast makes a closed/over-shrunk window a clean zero. A
  segment is `min(mss, usable, unsent-bytes)`; a zero window simply pauses the
  sender until a window update arrives (persist probes are M4-4 — a
  shrinking-to-zero peer window is documented as a pause, not a stall we break).
- **`rcv_wnd` is the live advertised window**, not the constant it was in M4-2.
  Queueing `n` bytes does `rcv_nxt += n; rcv_wnd -= n`; draining `n` on recv does
  `rcv_wnd += n` (clamped to `NSFTCP_RCVWND_DEFAULT`). The invariant is that the
  right edge `rcv_nxt + rcv_wnd` NEVER moves left (RFC 793 "do not shrink the
  window"): queue moves `rcv_nxt` up and `rcv_wnd` down by the same amount, so
  the edge is fixed; drain moves the edge right. `tcp_emit` already advertises
  `rcv_wnd`, so this needs no new field — `rcv_wnd` becomes dynamic.
- **Window update on drain (the deadlock rule).** When `tcp_recv` drains the rxq
  and the advertised window grows **from 0** or **by ≥ 1 MSS**, it sends a pure
  window-update ACK. Without it, a peer that filled our window stalls forever (it
  has no persist obligation toward us). This is the rule the live >1-window
  transfer proves; it is an explicit, separately-tested condition.

### 6. FIN-after-data and EOF

- **FIN drains behind the data (RFC CLOSE).** `tcp_close` on ESTABLISHED /
  CLOSE_WAIT no longer emits the FIN immediately (ADR-0031 did, correct only with
  an empty sndq). It sets `TCB_F_FINQ`, transitions
  (ESTABLISHED → FIN_WAIT_1, CLOSE_WAIT → LAST_ACK), completes the request
  (BSD-immediate, unchanged), and calls `tcp_output`. Two invariants make this
  safe: (a) the FIN sequence is frozen at close = `snd_una + sndq_bytes` — no
  send is possible after close, and `snd_una + sndq_bytes` is invariant as ACKs
  trim the sndq (una advances, sndq_bytes shrinks by the same amount), so
  `tcp_output` emits the FIN exactly when `snd_nxt` reaches that value (all data
  sent), and the condition self-clears once `snd_nxt` passes it (no FINSENT flag
  needed); (b) when the sndq is empty at close, `tcp_output` emits the FIN on the
  **same pass**, byte-identical to M4-2 — which is what keeps the M4-2 teardown
  tests (all 758 M4-1/M4-2 assertions) green.
- **EOF is a sticky flag.** Consuming the peer's FIN sets `TCB_F_RCVFIN`
  (`tcp_process_fin`), which completes a parked recv with rc=0 if the rxq is
  empty, and makes every later recv on an empty rxq return rc=0 — data queued
  ahead of the FIN is delivered first (recv drains the rxq, THEN returns 0), and
  EOF never becomes an errno. `tcp_recv` outside a receiving state is
  `NSF_ENOTCONN`; SEND outside ESTABLISHED / CLOSE_WAIT is `NSF_ENOTCONN`.
- **PSH** is set on the last data segment of a send burst (cosmetic; we deliver
  immediately regardless). **URG** is not supported: the urgent pointer is
  skipped and any "urgent" data is delivered inline (documented no).

### 7. Counters (spec §13.5 + documented private)

`datadrop` now counts **only** data-bearing text dropped in a state that
legitimately drops it (SYN_RCVD before data, or a synchronized closing state past
the peer's FIN); in-window ESTABLISHED / FIN_WAIT data is delivered. `oooseg`
ticks for a real gap segment (beyond RCV.NXT, dropped because `oooq` is M5) and
`dupack` ticks for a received **duplicate ACK** (SEG.ACK ≤ SND.UNA with
outstanding data, no payload) — deliberately NOT the dup-ACK we *emit* on a gap
(that is observable as the ACK carrying the old RCV.NXT). A new **private**
counter `rxfull` records an in-order segment dropped because the rxq PBUF bound
(`NSFSOC_RXQ_MAX`) was hit before the byte window closed (mirrors UDP's
`rxfull`); this is the one counter added beyond §13.5 + the existing private ones
(`hdrerr`, `datadrop`), documented here.

## Consequences

- **No retransmission (M4-4).** A segment lost on a real lossy path fails the
  connection; the copy-on-transmit sndq is the seam the RTO will use. No RTO,
  persist, or keepalive timer is armed in M4-3.
- **No out-of-order reassembly (M5).** A gap segment is dropped + dup-ACKed
  (`oooseg`); `oooq` stays empty. On the lossless link no gaps occur, so this is
  invisible live but exercised host-side.
- **rxq PBUF cap vs byte budget.** With trim-in-place, one inbound segment = one
  rxq PBUF, so a peer sending many tiny segments can hit `NSFSOC_RXQ_MAX` (32)
  before the 4096-byte window closes; that in-order segment is dropped + counted
  `rxfull` and the old RCV.NXT is re-ACKed (the peer retransmits). Coalescing is
  an M5 memory optimization.
- **Nagle / delayed ACK** are M5: every accepted segment is ACKed immediately and
  every send transmits as soon as the window allows.
- NSFTCP stays OUT of the `NSF` production load module (unreachable until the
  EZASOKET M4 verb set, M4-5); inbound TCP in production still draws ICMP
  protocol-unreachable. `S NSF` is byte-for-byte unchanged.
- No new errno VALUES (the NSFRQE freeze holds); `NSF_ENOTCONN` (57) and
  `NSF_EWOULDBLOCK` (35) gain TCP send/recv uses — noted in
  `docs/ezasoket-conformance.md`.
