# ADR-0031 — M4-2 TCP connection machine: acceptq linkage, background-close ownership, and the synchronous-verb completion seams

**Status:** Accepted (2026-07-16). Turns the M4-1 RST-only skeleton
(`src/nsftcp.c`, ADR from M4-1 milestone note) into a full connection state
machine: three-way handshake (both directions), orderly FIN teardown, and
TIME_WAIT with 2MSL + pool-pressure reclaim. **No data path** (M4-3) and **no
retransmission** (M4-4).
**Relates to:** spec §13 (TCB / teardown checklist / counters), §10.2/10.3
(SOCKCB, acceptq, parked requests), ADR-0028 (checksum seed), ADR-0022 (the
same-AS POST seam soc_complete rides), the M4-1 skeleton.

## Context

M4-1 left every non-CLOSED per-state handler as an RFC-ordered stub and the only
live emitter a RST. M4-2 fills the handlers. Four design questions had no obvious
answer from the existing code, and each is a correctness (not style) decision:

1. **Where does an established, un-ACCEPTed child hang?** spec §10.2 gives the
   listener an `acceptq` but does not say what its elements are. The TCB already
   uses its one `QELEM q` for the demux list, and an established child MUST stay
   demuxable (its 4-tuple carries subsequent segments), so `q` is unavailable.
   And M3-1 left `soc_destroy` flushing the acceptq **as PBUFs**
   (`buf_free(Q_ENTRY(e, PBUF, q))`) — a placeholder that becomes a heap
   corruptor the moment the queue holds anything else.

2. **Who owns a connection after the app closes it?** BSD `close()` returns
   immediately; the connection then finishes FIN/TIME_WAIT in the background. But
   the NSF object model ties a TCB's life to its SOCKCB (`soc_destroy` →
   `tcp_detach` → `tcp_destroy`), i.e. teardown is driven *top-down from the
   socket*. A background-closing connection has no app waiting on the socket, yet
   must keep the TCB alive for seconds and then free BOTH.

3. **How does ACCEPT reach TCP?** `PROTOPS` had no `accept` member and
   `soc_dispatch` no `RQ_ACCEPT` case (RQ_ACCEPT fell through to `NSF_EINVAL`).

4. **RQ_LISTEN completion.** `do_delegate` completes `r` only when the op returns
   non-zero; a real (rc==0) listen op would leave the request **uncompleted** —
   latent since M3 because TCP's listen was NULL (→ EOPNOTSUPP → completed).

## Decision

### Acceptq linkage — a second QELEM in the TCB, not a bigger SOCKCB

The TCB gains `struct tcb *listener` (a child → its listening TCB, NULL
otherwise) and a **second** `QELEM acceptlink`. `q` always links the demux list;
`acceptlink` links the listener's `acceptq` for an established-but-un-ACCEPTed
child. SOCKCB is untouched (72 B, UDP unaffected) — the TCP-specific topology
lives in the TCB where it belongs. The 256-byte target pool slot already covers
the growth (188 → 200 B).

`soc_destroy` step 3 is corrected: the acceptq is **protocol-owned**; it is
drained by *unlinking only*, never `buf_free`. TCP empties it first — a
listener's `tcp_destroy` walks the demux list and `soc_destroy`s every child
(embryonic SYN_RCVD via the `listener` back-pointer, established via the
acceptq), dequeuing each `acceptlink` before freeing, so the queue is already
empty when step 3 runs and stays consistent throughout.

### Background close — the ownership inversion

`close()` completes the request immediately (RETOK), sets `TCB_F_APPCLOSED`,
sends FIN, and transitions (ESTABLISHED → FIN_WAIT_1, CLOSE_WAIT → LAST_ACK); the
TCB and its SOCKCB live on. When a connection reaches true end of life
(LAST_ACK → CLOSED, TIME_WAIT 2MSL expiry, RST in a synchronized state, or a
LISTEN/SYN_SENT close with nothing on the wire), TCP calls **`soc_destroy(tcb->
sock)`** — the ONE top-level teardown — which cascades to `tcp_detach →
tcp_destroy`. **`tcp_destroy` never calls `soc_destroy`**, so the recursion
terminates. This keeps a single teardown checklist (spec §10.5/§13.4) while
supporting BSD-immediate close.

A RST in a synchronized state completes any parked request with the **specific**
errno (`NSF_ECONNRESET`) *before* `soc_destroy`, so the pend slot is cleared and
the request is not re-completed with the generic `NSF_ECONNABORTED`.

### ACCEPT and LISTEN seams

`accept` is added as the **trailing** `PROTOPS` member: C zero-fills omitted
trailing initializers, so UDP's `g_udp_ops` and the M3 dummy PROTOPS keep
compiling untouched, and a NULL accept maps to `NSF_EOPNOTSUPP` like any unset
op. `soc_dispatch` gains an `RQ_ACCEPT` case. RQ_LISTEN is routed to a new
`do_listen` (mirroring `do_bind`) that completes `r` from the op's rc — LISTEN is
synchronous and r-less, so it cannot ride the `do_delegate` "op completes/parks
r itself" contract.

### TIME_WAIT, reclaim, ISS, MSS, errnos, counters

- **2MSL** = 60 s (`NSFTCP_2MSL_TICKS`, a named constant; TCPCONFIG-tunable
  later). TIME_WAIT entry cancels the other three timers so **only `t_2msl` is
  armed** — the invariant that makes the 2MSL callback's synchronous
  `tcp_destroy` (which frees the TCB holding `t_2msl`) safe against `nsftmr_run`
  (it detaches the fired timer and re-reads the head; no sibling of the freed TCB
  remains queued).
- **Reclaim:** on TCPTCB pool exhaustion at SYN time, scavenge the **oldest**
  TIME_WAIT TCB (first in demux-list order = insertion order), count it, retry
  once; else drop the SYN silently (RFC-conformant). Never half-allocate.
- **ISS** from the platform clock (`nsf_now`), weighting the fast half toward the
  RFC 793 §3.3 ~4 µs tick spirit — only "advances, hard to guess" matters.
- **MSS:** SYN / SYN|ACK carry a 4-byte MSS option (24-byte header, data-offset
  6) announcing our receive MSS (route MTU − 40); a peer's MSS is parsed and
  clamped to `[NSFTCP_MSS_MIN, NSFTCP_MSS_MAX]`. A malformed option list (length
  byte < 2, or an option running past the header) **drops** the segment and
  counts `hdrerr` — never an overrun.
- **New errnos** (Table 67 values, `docs/ezasoket-conformance.md`):
  `NSF_ECONNRESET 54`, `NSF_EISCONN 56`, `NSF_ENOTCONN 57`, `NSF_ETIMEDOUT 60`,
  `NSF_ECONNREFUSED 61`.
- **Counter name cap.** `STSCTR.name` is a 12-char field (`nsfsts.h`), so the
  spec-§13.5 concept "timewaitreclaim" (15 chars) is registered — and read by
  `sts_value` — as **`twreclaim`**. A 15-char name is silently truncated to
  "timewaitrecl" and then unreadable by its full name (the exact-fit rule in
  `sts_fieldeq`); M4-2 is the first milestone to tick + read it, which surfaced
  the latent trap. A private `datadrop` counter records data-bearing segments
  dropped in M4-2 (no data path yet).

## Consequences

- The lossless CTCI link (M2 gate) means a lost segment on a real lossy path
  simply fails the connection in M4-2 — there is no RTO/retransmit until M4-4.
  Documented in the spec §13 status note.
- Simultaneous open and simultaneous close fall out of the RFC step order and are
  covered by host tests; neither is special-cased.
- ESTABLISHED carries no payload: a data-bearing segment is processed for its
  SYN/ACK/FIN/RST content and its data bytes are dropped + counted; RCV.NXT is
  NOT advanced over undelivered data (that would falsely ACK it).
- NSFTCP stays out of the `NSF` production load module (unreachable until the
  EZASOKET M4 set, M4-5); inbound TCP in production still draws ICMP
  protocol-unreachable. `S NSF` is byte-for-byte unchanged.
