# ADR-0030 — Close issue #28: gate the IOHALT read-park on a provably-armed READ (`rarmed`)

**Status:** Accepted (2026-07-15). Closes the stall corner left OPEN by
**ADR-0027** ("IOHALT with no outstanding READ"). Reproduced live, at scale, by
the M3-5 UDP echo sample (`samples/nsfecho.c`); fixed and re-validated on the
real 0500/0501 pair.
**Relates to:** ADR-0027 (active read-park via IOHALT — the mechanism this
refines), ADR-0025 (pair sequencing / `rhold`), ADR-0022/0023 (subtask model),
§9.3 (CTCI driver).

## Context

ADR-0027 parks the armed READ by IOHALT-ing it when a locally-originated WRITE
is queued, so the WRITE issues on the freed channel. It carried a known-open
corner (issue #28): the executive decided to halt on `!d->rhold`, which does
**not** prove a READ is actually armed. `kick` clears `rhold` and posts
`returnecb` when the sendq drains; `read_sub` re-issues its EXCP a moment later.
A locally-originated send that lands in **that window** — after the `returnecb`
POST, before `read_sub`'s `ctci_read` — re-enters `kick` with `rhold=0` and no
READ outstanding. The IOHALT then hits nothing: Hercules `ctc_halt_or_clear()`
acts only if `pCTCBLK->fReadWaiting`, so it is a pure no-op. No X'48' purge
completion arrives, so `service` (which sets `rhold` **only** on a read
completion) never re-parks, `read_sub` arms the read into an idle link, and the
WRITE **stalls until the next inbound frame** — the pre-#21 stall class,
reintroduced for that one send.

ADR-0027 assessed this as a narrow window ("a burst keeps the sendq full → no
drain → no race; a spaced send lets the read re-arm first") and deferred the fix.
**That assessment was wrong about impact.** The M3-5 UDP echo server is the first
workload to hammer the pattern — one locally-originated reply per inbound
datagram, thousands of times — and it hit the race reliably on an idle link:
a stop-and-wait host client saw the first ~20 replies, then one 2 s stall,
then a permanent one-off desync (tcpdump: request X answered only when X+1's
inbound frame arrived). `send_fail=0` and `ierr=0` throughout — the socket and
device paths were healthy; the reply was built and handed to the channel but
physically held. `echo` with a concurrent background `ping` running passed
1000/1000: any other inbound traffic released each reply, pinning the cause to
the idle-link write path, not the sample / NSFEZA / NSFREQ.

## Decision

**Gate the IOHALT on a flag that proves a READ EXCP is outstanding.** Add
`CTCIDEV.rarmed`, written by `read_sub` only: set immediately after `ctci_read`
issues the SIO, cleared immediately after the completion is in hand
(`wait_or_stop(recb)` returns) — one incarnation, sole writer, executive
read-only, the same discipline as `rready`/`wready` (§3). `kick` halts to park
the read only when `!rhold && !Q_EMPTY(sendq) && !halting && rarmed`. When the
read is not yet armed, `kick` does nothing this pass.

To close the window without leaning on the 100 ms heartbeat, `read_sub` POSTs
`dev->ecb` **immediately after arming** (setting `rarmed=1`). That re-runs the
executive's service/kick pass with `rarmed` now true, so a send that arrived
during the arming window parks the read and issues on the very next pass — a
scheduling delay, never a stall.

The two parts are separable: the `rarmed` guard is the **correctness** fix (the
heartbeat already backstops liveness — a race-loser reply would clear within
≤100 ms even without the POST); the post-arm POST is a **latency** optimisation
that keeps idle-link reply RTT near-unimodal instead of bimodal.

## Why this is race-free

`read_sub` is strictly ping-ponged with the executive through `returnecb`: it
cannot re-arm until the executive posts `returnecb`, and it only does so at
drain. So `rarmed` tracks exactly one read incarnation and can never run ahead
of the executive's view. The cases:

- **Send lands while the read is armed (`rarmed=1`)** — the common case once the
  app is running: IOHALT purges (X'48'), `service` sets `rhold`, the WRITE
  issues. Unchanged from ADR-0027.
- **Send lands in the arming window (`rarmed=0`)** — the #28 case: `kick` waits;
  the post-arm `dev->ecb` POST re-runs `kick` with `rarmed=1`; parks + issues.
- **IOHALT races a real inbound frame** — the read completes X'7F' with data
  instead of X'48'; `service` decodes it and sets `rhold`; the WRITE still
  issues. (ADR-0027's documented race; unaffected.)
- **`rarmed=1` but the read just completed** — `read_sub` clears `rarmed` right
  after the completion; a halt that slips in first is a Hercules no-op and the
  completion flows through `service`. No stall.

No path leaves a WRITE queued indefinitely.

## Evidence (live, MVSCE, real 0500/0501)

- **Before:** idle-link `echo_client.py echo --count 1000` → 21/1000, then a 2 s
  stall and permanent desync; `CTCA rpurge=39` for 300 echoes (the halt rarely
  purged — it was hitting an un-armed read). `echo` + background `ping` → 1000/1000.
- **After:** idle-link `echo --count 1000` → **1000/1000, 0 lost**, no ping
  needed; `rpurge` climbs ~1:1 with the echoed count (every idle-link reply now
  routes through a real purge). M2 live ping gate re-run: **1000/1000 unimodal**.

## Consequences

- `CTCIDEV` grows 124 → 128 bytes (`rarmed` + 3 pad); `NSF_SIZE_ASSERT` updated.
- One extra `dev->ecb` POST per read arm → one extra (no-op) executive pass per
  received frame. Cheap; the M2 ping gate confirms no receive-path regression.
- The ADR-0025 follow-on note ("a locally-originated WRITE while the READ is
  armed still queues; needs HIO or attention-driven read") is now **resolved**
  for the single-outstanding-WRITE model. The attention-driven upgrade path in
  ADR-0027 (removing the permanently-outstanding READ, unlocking ping-pong
  throughput) remains the longer-term option, not a correctness gap.
- The IOHALT/PURGE mechanism, UCB chase, and completion classes (X'7F' / X'48' /
  error) are unchanged from ADR-0027.

## Sources

- `samples/nsfecho.c`, `samples/host/echo_client.py` — the reproduction + the
  before/after live gate.
- ADR-0027 (the read-park mechanism), ADR-0025 (pair sequencing), §9.3.
- Hercules SDL-Hercules-390 `ctc_ctci.c` (`ctc_halt_or_clear`, `fReadWaiting`).
