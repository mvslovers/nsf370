# ADR-0023 — CTCI I/O subtask implementation: raw-block doneq, single-block-sync, subtask-owned channel

**Status:** Accepted (2026-07-12), from the M1-4b implementation of ADR-0022 and
its live validation on MVSCE (issue #18). ADR-0022 decided *that* the CTCI driver
completes through the M1-2 `doneq` model behind an `cthread` I/O subtask; this ADR
records the sub-decisions that implementation settled — each of which ADR-0022
left open or sketched, and several of which the first live runs corrected.
**Corrected in part by ADR-0025 (issue #21):** §6's claim (1) — "the STIMER-exit
timeout does not fire on the CRT main task" — was a misdiagnosis (the timeout ECB
was outside the WAIT list, so its POST could not wake the wait on ANY task); §5's
"separate timeout ECB" rule now additionally requires that ECB IN the waitlist;
and §2's read handshake is sequenced behind the write pipeline (the pair shares
one channel). The operative decisions here otherwise stand.
**Relates to:** ADR-0022 (the decision), ADR-0019/0020 (EXCP recipe + framing,
unchanged), ADR-0021 (the superseded DEVIO seam), §9.2/§9.3 (device/CTCI),
§5.3 (executive loop), CLAUDE.md §3.

## 1. `doneq` payload = RAW BLOCKS decoded on the executive (Option B), not PBUFs

ADR-0022 was ambiguous ("pushes the block" vs "the M1-2 doneq model", which for
NSFHOST carries PBUFs). The M1-4b brief *recommended* Option A — the subtask
DECODES the received block and allocates PBUFs, pushing PBUFs onto `doneq` —
**conditioned on `buf_alloc` being TCB-safe.** It is not, and cannot be made so
without breaking a non-negotiable invariant:

- `nsfmm.c` `mm_alloc`/`mm_free` are plain, unserialised free-list pops/pushes.
  §3 states the storage model is single-task with **"no locking except the
  CS-based exit→mainline handoff (`xq_push`) and POST."** Making `buf_alloc`
  TCB-safe means a CS lock (or spinlock) on the hottest path — a review-blocking
  §3 violation.
- §9.2 is explicit: **"Inbound PBUFs are allocated by the driver (bottom half,
  executive task)."** Option A moves allocation onto the subtask, contradicting it.
- NSFHOST's loopback reader is copy-free — it relays a pre-existing PBUF and
  **never calls a storage service** (`nsfhost.c`). So "matching M1-2" actually
  favours the producer NOT allocating.

**Decision: Option B.** The read subtask hands the filled raw I/O buffer up; the
**executive** `service` decodes it into PBUFs (the existing `ctci_decode_block`,
unchanged from M1-4). NSFMM is touched only on the executive task — §3 intact, no
lock. The subtask does the absolute minimum (EXCP + wait + hand the block over +
POST), exactly the async-producer discipline §9.2 describes.

## 2. READ = single-block-synchronous (a `returnecb` handshake), not ping-pong

The subtask owns ONE read buffer. Its loop: `EXCP READ; single-ECB wait recb;
store len/post; POST dev->ecb; wait returnecb`. The executive decodes the buffer
and POSTs `returnecb`, releasing the subtask to read again. Only ONE block is
ever in flight, so **no queue and no free-buffer handshake are needed** — the
`returnecb` post is the whole synchronisation. This drops `rbuf1` (−20 KB/device).

The one cost — no READ is outstanding for the microseconds the executive spends
decoding — is **lossless**: Hercules buffers/back-pressures inbound frames with no
READ outstanding (§9.3, verified against `ctc_ctci.c`; the same property that made
ADR-0019 reject the CHE appendage). Ping-pong device buffers + a free-buffer queue
(keeping a READ always outstanding, honouring §9.3's "re-drive-before-parse")
remain the **documented throughput follow-on**, deferred exactly as the appendage
is — correctness first. §9.3's "re-drive-before-parse is normative" line was
written for the single-task M1-4 model; under the subtask model it is superseded
by single-block-sync + the Hercules-buffering justification.

## 3. Each subtask OWNS OPEN + EXCP + CLOSE of its subchannel

ADR-0022 says "the subtask owns the channel." Implementation: **the read subtask
OPENs the read subchannel and the write subtask OPENs the write subchannel, each
on its own TCB**, as their first act (reporting the OPEN rc back so `ctci_op_start`
stays synchronous / refuse-to-start). SVC 99 allocate/unallocate is
address-space-scoped, so it stays on the executive (`ctci_chan_open` →
`ctci_chan_alloc`/`ctci_chan_unalloc`).

The load-bearing reason is **shutdown, not a cross-TCB-EXCP theory** (untestable
from here): the outstanding READ must be purged at stop, and a CLOSE purging an
EXCP started on the **same** TCB is the clean path. Split OPEN/CLOSE (executive)
from EXCP (subtask) and teardown becomes a cross-TCB purge race; subtask-owns-
everything makes it a same-TCB CLOSE.

**Consequence — the HLASM top half needs a PER-SUBCHANNEL save area.** With two
subtasks calling `ctci_read`/`ctci_write`/`ctci_close_sub` concurrently, the one
shared static `CTCISAVE` (FUNHEAD SAVE=) that was safe under the single-task model
is corrupted by two tasks at once — a live **S238** on the write subtask. Fixed by
moving the save area INTO each `CTCISC` (the `SCSAVE` field): each entry uses the
save area in its own scb (read→rscb, write→wscb), never shared — concurrency-safe,
no lock. (`asm/nsfctcio.asm`.)

## 4. WRITE = one outstanding; the executive encodes, the write subtask EXCPs

`dev_send` queues the PBUF (bounded `sendq`, executive side). The executive `kick`
encodes the PBUF into the write buffer (`buf_copyout` stays on the executive — the
**PBUF never crosses to the subtask**), sets `txbusy`, and POSTs `txgoecb`. The
write subtask EXCPs the WRITE and single-ECB-waits `wecb`; on completion it POSTs
`dev->ecb`, and the executive reaps + `buf_free`s the PBUF exactly once (§3: only
the executive frees). No PBUF handback queue is needed.

## 5. Seam + two correctness rules the live runs forced

- **`nsfthr`** (`include/nsfthr.h`) wraps the subtask: libc370 `cthread` on MVS
  (`src/nsfthr.c`), a pthread + condvar on the host (`src/nsfthr_host.c`), so the
  SAME subtask logic runs both ways. NSF must call `nsfthr_setup()`
  (`clib_identify_cthread`, a bare IDENTIFY) itself — libc370 only IDENTIFYs
  CTHREAD inside the authorized `clib_apf_setup()`, and NSF stays unauthorized.
- **Timed waits use `ecb_(timed_)waitlist` with a SEPARATE timeout ECB**, NOT
  `cthread_wait`/`cthread_timed_wait` (which CLEAR the ECB) nor `ecb_timed_wait`
  (which POSTs it on timeout). Either would lose a completion or forge a phantom
  one in the `wait_or_stop` re-check loop, and turn a `nsfthr_join` timeout into an
  unbounded EOT hang (a detached-but-live subtask).
- **The executive clears `dev->ecb` before the CTCI `service`** (the default
  doneq path already does; the CTCI branch did not). A stale-posted `dev->ecb`
  lingering in the executive's multi-ECB WAIT is the SAME #18 hazard recb/wecb
  were removed for — it corrupts the WAIT so a later operator/stop POST no longer
  wakes it. This is the UFSD `ufsd_server_ecb_reset` (reset-before-WAIT) discipline.

## 6. Idle liveness = the ADR-0017 heartbeat; STIMER is a per-task singleton

With the driver present, the executive's WAIT is woken by real POSTs alone, and
live runs showed an operator MODIFY arriving while the stack is **idle** (the
normal state of a network stack) can sit undrained until the next device
completion. Two candidate fixes, one survivor:

- **Rejected: a timed executive WAIT** (`ecb_timed_waitlist`, the HTTPD
  `STIMER WAIT`-fallback shape). Two disqualifiers, both live-observed:
  (1) its STIMER-exit timeout **does not fire on the CRT main task** (it does on
  cthread subtasks — Stage 0's naps); (2) it opens with **`TTIMER CANCEL`**, and
  STIMER is a **per-task singleton** — so every WAIT entry silently kills any
  interval timer the executive owns, including the heartbeat below. The two
  mechanisms are mutually exclusive; the executive WAIT stays plain
  `ecb_waitlist`.
- **Adopted: arm the ADR-0017 self-re-arming async STIMER exit at STC start**
  (`nsftmr_plat_arm(1)`, nsfmain.c) — the TSTEVTM-validated 100 ms heartbeat.
  The exit POSTs timerECB, the loop iterates, the unconditional operator drain
  picks up any queued CIB within a tick. Costs 10 interrupts/s and consciously
  trades ADR-0011's "an idle stack takes zero timer interrupts" for operator
  liveness; keeps `nsftmr_run(1)` tick accounting correct for M2+ timers.
- **Corollary (documented in nsfthr.h):** `nsfthr_timed_wait`/`nsfthr_join` use
  `ecb_timed_waitlist` and therefore TTIMER-CANCEL the **calling** task's timer.
  Fine on the subtasks (they own theirs); on the executive they may run only
  outside the heartbeat window — device start runs before the arm, teardown
  after the disarm. Any future mid-run use (an M2+ VARY-style restart) must
  re-arm afterwards.

## Rejected / deferred

- **Option A (subtask decodes + allocates PBUFs):** rejected — needs a lock in
  `mm_alloc` (breaks §3) and contradicts §9.2. Technically implementable; the
  cost is the invariant, which is why it is an ADR, not a silent choice.
- **Ping-pong device buffers + free-buffer queue:** deferred throughput follow-on
  (see §2). Revisit only if a loss-under-load measurement demands it.

## Validation

- Stage 0 (TSTCTHR, MVS CC 0): `cthread_post` (SVC-2) into the executive's
  multi-ECB WAIT wakes it (20/20) alongside the STIMER heartbeat — the premise the
  whole fix rests on.
- Host: TSTCTCI (subtask model over the host shims, real pthreads) + TSTCTCIF +
  TSTDEV green.
- MVS isolated (TSTCTCM, live 0500/0501, batch): the subtask model end to end —
  crafted WRITE on the wire (tcpdump id 0xABCD), `ctr_in` rising through a MULTI-
  ECB WAIT, clean shutdown, no S238.
- STC gate (S NSF on the live pair, final build with the heartbeat): device up,
  both subtasks OPEN; `F NSF,STATS` answers promptly in **every** state — fresh
  idle (`in 0`), reads flowing (`in 59`, `in 86` — **after READs complete**, the
  exact operation #18 hung), and post-traffic idle (the regression that hung an
  interim build); MIH across idle tolerated (IGF991I/995I, device kept working);
  `P NSF` → NSF830I → NSF011I → IEF404I within one second, subtasks joined,
  SYSUDUMP **empty** (0 bytes).

## Sources

ADR-0022, ADR-0019/0020/0021, §9.2/§9.3/§5.3, CLAUDE.md §3; `libc370` `cthread` /
`ecb_timed_waitlist` / `clib_identify_cthread`; `mvslovers/ufsd`
(`ufsd_server_ecb_reset`), `mvslovers/httpd` (`httpd.c` main loop); live runs on
MVSCE (`mvsdev`), issue #18.
