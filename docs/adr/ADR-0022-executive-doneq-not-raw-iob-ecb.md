# ADR-0022 — The executive WAITs only on ECBs it owns (doneq via an I/O subtask), not the raw CTCI IOB ECB

**Status:** Accepted (2026-07-12), from the first live STC integration of the
CTCI driver on real MVS (issue #18). **Supersedes the WAIT premise of ADR-0019**
— "the §5.3 executive loop already WAITs on an ECBLIST of per-device ECBs; it can
wait on the IOB ECB directly" — which the live run disproved. ADR-0019's EXCP
recipe (plain EXCP, no appendage, `SLI`, post `X'7F'`, residual arithmetic,
ping-pong) and ADR-0020's framing are unaffected and remain in force.
**Relates to:** §5.3 (executive loop / ECBLIST), §9.2 (DEVOPS / NETDEV / doneq),
§9.3 (CTCI driver), ADR-0019, ADR-0017, M1-2 (doneq model), CLAUDE.md §3.
**Blocks:** M1-4 (its STC exit gate). Implementation is issue **#18** ("M1-4b").

## Context

M1-4 built the CTCI bottom half and wired it into the STC. Following ADR-0019,
the driver contributed its read/write IOB ECBs (`recb`/`wecb`) into the §5.3
executive `WAIT ECBLIST` (the "DEVIO" seam), so IOS would post the IOB ECB and
the executive loop would wake on it directly — no appendage, no subtask.

**Host coverage was green and, in isolation, the real channel worked.** TSTCTCM
on MVSCE (CTCI pair 0500/0501 on `tun0`) proved **receive** (`ctr_in` rose from
live host pings, decoded into PBUFs and posted `EV_PACKET_RECEIVED`) and **send**
(`ctr_out=1`; the crafted ICMP echo, id `0xABCD`, reached the host TUN in
`tcpdump`). **But TSTCTCM is loop-free — a single `ecb_wait(&d->recb)` probe —
which is precisely the safe path**, and therefore is *not* a production-integration
proof.

Wiring the same driver into the STC (which runs the real §5.3 multi-ECB loop)
surfaced a hang, bisected cleanly on the live system (issue #18):

- device **inactive** (undefined CUU fails to start → no device ECBs in the
  ECBLIST) → the operator works;
- device **UP, no traffic** (a READ is outstanding but has not completed) →
  `F NSF,DISPLAY` **replies** — the *static presence* of `recb`/`wecb` in the
  ECBLIST is fine;
- device **UP, one completed READ** (host ping → IOS posts `recb`) → `F NSF,STATS`
  and `P NSF` get **no reply**; the loop is blocked in `WAIT` and only
  `FORCE`/`C NSF` (→ ESTAE `NSF900E`, S222) takes it down.

So the trigger is **an asynchronous IOS POST of `recb`, out of phase with the
executive's multi-ECB `WAIT ECBLIST`**: once a foreign task posts an ECB that the
executive `WAIT` is also holding, the wait state is left such that a later
operator/stop POST no longer readies the executive. We state the mechanism
*modestly* — the exact RTM/RB wait-chain interaction is below our ability to
verify from here — but the empirical evidence is unambiguous, and it is
corroborated by a working precedent that deliberately avoids the pattern.

## Decision

**The executive WAITs only on ECBs it owns.** A device's I/O completion reaches
the executive through the **M1-2 `doneq` model** (already in `NETDEV`, already
used by NSFHOST, already validated host + across a thread boundary), never by the
executive waiting on the raw IOB ECB:

- A CTCI **I/O subtask** owns the channel: it issues `EXCP` and does a
  **single-ECB `ecb_wait(&recb)`** (the exact path TSTCTCM proved safe), and on
  completion pushes the received block to the device `doneq` (`xq_push`) and
  **POSTs `dev->ecb`**.
- The §5.3 executive keeps its M1-2 shape: it WAITs on `dev->ecb` (an ECB *it*
  owns), **resets it before each WAIT**, drains the `doneq`, and decodes each
  block into PBUFs **on the executive task** (allocation stays single-task, §3).
- This **retires the DEVIO `recb`/`wecb`-in-WAIT seam** added at M1-4. `recb`/`wecb`
  are waited on only by the subtask, never by the executive.

### Precedent — adopt UFSD's loop shape (source: `mvslovers/ufsd`)

`src/ufsd.c` (the working UFSD STC) WAITs on `ecblist = {comecbpt, server_ecb}`
only, with the **reset-before-WAIT + double-check drain** loop
(`ufsd_server_ecb_reset` then re-check `req_head`); async producers POST
`server_ecb`. UFSD **never waits on a raw I/O ECB**. NSF adopts this loop shape
(reset `dev->ecb` before WAIT; a double-check drain of the `doneq`) verbatim in
spirit. Sources: `ufsd/src/ufsd.c` (the `WAIT ECBLIST` at AP-1c) and
`ufsd/src/ufsd#que.c` (`ufsd_server_ecb_reset`).

### Key distinction — NSF is SAME-address-space, so it stays unauthorized

UFSD's producers are **cross-address-space**, which forces machinery NSF must
**not** copy:
- `ecb_post` (SVC 2) causes **S102** for a cross-AS POST, so UFSD uses `__xmpost`
  (CVT0PT01 branch entry, supervisor state) — `ufsd#que.c` header, lines 8-10;
- its `server_ecb` lives in **CSA (key 0)**, so UFSD's WAIT and the ECB reset run
  in **supervisor state** (`__super(PSWKEY0)`) — a problem-state WAIT on a key-0
  CSA ECB would abend `X'201'` (`ufsd.c` lines 428-436).

**None of that applies to NSF.** The CTCI I/O subtask is **same address space** as
the executive: a **normal POST** (`cthread_post`), **problem state, key 8**, no CSA,
no `__super`. NSF stays a plain **unauthorized problem-state** started task — do
not import UFSD's CSA / key-0 / `__xmpost` machinery.

### Seam — libc370 `cthread` (no hand-rolled ATTACH/POST asm)

Per the ADR-0018 discipline (reuse proven libc370 seams; no raw asm bridge for a
service libc370 already wraps), the subtask uses **`cthread`**
(`libc370/include/clibthrd.h`):
- `cthread_create(func, arg1, arg2)` — ATTACH the I/O subtask (`@@CTCRTE`);
- `cthread_post(ecb, code)` — same-AS MVS POST of `dev->ecb`;
- `cthread_detach` + `CTHDTASK.termecb` (offset `X'10'`, "posted by MVS when task
  ends") — join/quiesce at shutdown;
- `cthread_pop(task, CTHDPOP_ESTAE, ...)` — the subtask's own recovery, so a
  channel fault on the subtask does not take the executive down.

**Honesty note:** unlike `__estae`/`__svc99` (proven in ecosystem code and by our
own runs), `cthread` is **not** exercised anywhere in the code we can see. Its
first use is therefore **itself an MVS-validation item** in M1-4b — not assumed
safe.

## Scope of this decision

The same-address-space simplification above (plain POST, problem state, key 8, no
CSA, no `__xmpost`) applies **only** to the CTCI **I/O subtask → executive
completion** path. That path is intra-AS **forever**: a `cthread` subtask lives in
the executive's *own* address space, and that does not change when NSF becomes a
subsystem (`NSFS`, M3+). So intra-AS I/O completion stays a plain POST. This must
**not** be read as a blanket property of NSF.

When NSF becomes a subsystem (M3+), an application issuing an EZASOKET call runs in
a **foreign** address space; waking that application is a genuine **cross-AS**
POST and **will** require UFSD's machinery — the client ECB in **CSA (key 0)** and
`__xmpost` (CVT0PT01 branch entry, supervisor state), because a cross-AS SVC-2 POST
causes **S102**. That boundary is deliberately **out of scope here**: it is decided
at the socket / NSFRQE layer, not by this ADR (which governs only device→executive
completion).

In short: **intra-AS I/O completion = plain POST (this ADR); cross-AS consumer
wakeup = UFSD's cross-AS pattern (later, at the socket/NSFRQE layer).** This
section only *scopes* the decision; ADR-0022 stays **Accepted**.

## Rejected for v1 — the CHE appendage (ADR-0019 Option B)

An appendage could POST `dev->ecb` by branch entry directly from the I/O
interrupt, with **no subtask**. Rejected here for the same reasons ADR-0019
rejected it, now reinforced: it costs a `SYS1.SVCLIB` install, `IEAAPP00`
authorisation and restricted (supervisor, key 0) state, and would make NSF an
**authorised** program. The subtask keeps NSF **problem-state and installs
nothing**, so it is preferred. Option B stays fully documented in ADR-0019 should
throughput ever demand it.

## Consequences

- **M1-4 stays open.** Its exit gate ("`ping` → hexdump/EV_PACKET_RECEIVED in the
  running STC") is blocked on issue #18. What M1-4 *did* deliver stands: the codec
  (`nsfctcif`), the portable bottom half (`nsfctcib`: ping-pong, completion demux,
  decode→PBUF, sendq→WRITE, single-owner ownership), the host suites (TSTCTCIF,
  TSTCTCI), the cross-build/alias discipline, and the **isolated** TSTCTCM live
  receive+send proof.
- The `NSFRQE`/socket layers (M3+) also become async-producer consumers of the
  same `doneq` model, so fixing this now pays forward.
- A latent WAIT-seam bug was fixed alongside (not the cause of this hang):
  `nsfevt_plat_wait` sized its local ECB copy `list[8]` while the loop's list is
  `EVT_ECBLIST_MAX = 16`; a 9th ECB (a second interface, or M3's requestECB) would
  have silently truncated the cib/stop ECBs out of the WAIT — the same class of
  hang. Bumped to 16.
- **M1-4b validation gate:** `S NSF` → host ping → `EV_PACKET_RECEIVED` via the
  `doneq`; `F NSF,STATS` **responds after a READ completes** (today's failing
  case); MIH tolerated across idle; `P NSF` clean shutdown with the subtask
  detached (`termecb`).

## Sources

- Live bisection on MVSCE (`mvsdev`), issue #18 — device inactive / UP-idle /
  UP-after-one-READ.
- `mvslovers/ufsd`, `src/ufsd.c` (`WAIT ECBLIST={comecb, server_ecb}`,
  reset-before-WAIT + double-check drain, the CSA key-0 supervisor-state WAIT) and
  `src/ufsd#que.c` (`ufsd_server_ecb_reset`; the `__xmpost` / SVC-2-S102 cross-AS
  note) — the working same-shape precedent and the cross-AS machinery NSF avoids.
- `libc370/include/clibthrd.h` — `cthread_create` / `cthread_post` /
  `cthread_detach` / `CTHDTASK.termecb` / `cthread_pop(CTHDPOP_ESTAE)`.
- ADR-0019 (superseded WAIT premise; EXCP recipe retained), ADR-0018 (seam-reuse
  discipline), M1-2 / NSFHOST (the `doneq` model this restores).

---

## Annotation (2026-08-27, issue #64 step 64-1) — Phase 2 diverged from the reset half, and what it cost

**Append-only. The decision above is unchanged.** What follows records a place
where this ADR's discipline was not honoured, the measured cost, and the
correction. It is not a fix for issue #64, and it must not be read as one — see
"What this annotation does not claim" below.

### The divergence

This ADR's loop shape has two halves: **reset the ECB before taking the work**,
and **double-check after**. Three drains in the tree implement it.

| drain | resets its ECB | double-check |
|---|---|---|
| `nsfreq_drain` (Phase 1, `src/nsfreq.c`) | `g_reqecb = 0u` before `xq_drain` | its own `do { … } while (g_reqxq.head != NULL)` |
| `nsfv.c`'s loop (the probe STC) | `nsfv_server_ecb_reset` after servicing | `while (nsfv_any_pending(anchor))` |
| `nsfsx_drain` (Phase 2, `src/nsfsx.c`) | **never** — until this step | the WAIT gate (`nsfsx_pending`) |

`g_wake_ecb` was assigned zero exactly once, in `nsfsx_start`, and never again.
`src/nsfevt.c`'s step 2b states the contract Phase 2 did not meet in so many
words: *"The drain resets its own request ECB before taking the queue and
double-checks (ADR-0022), so it owns the reset-before-WAIT discipline."*

### The cost, measured rather than reasoned about

The first cross-address-space POST latched the POSTED bit for the life of the
STC, and a posted ECB in the ECBLIST makes `WAIT` return immediately — so
`evt_mainloop` could never block again. One controlled pair, one instance, the
first request as the only variable (`docs/nsf-64-0-measurements.md` §2):

| state | POSTED | pass rate | host CPU (user) |
|---|---|---|---|
| fresh, before any request | N | **9.95 /s** (the heartbeat, blocking correctly) | **0.9 %** |
| same STC, after one request | Y | **8 492 /s** | **26.0 %** |

Control with NSFS stopped: 0.5–0.7 % user. The 26 % is a full quarter of a host
core, burnt permanently, by every NSFS instance that has ever served a request.

### The correction, and why it is the reset alone

`nsfsx_drain` now resets `g_wake_ecb` immediately after the `wakeposts`
observation and **ahead of both of its scans**. A client publishes its request
(`req_state = PENDING`) and only then POSTs, so relative to that reset there are
exactly two orderings and both are safe: a POST *before* it implies the publish
was before it too, and both scans run after it, so the slot is seen on that very
pass; a POST *after* it leaves the bit standing, and the WAIT returns on it.
There is no third ordering, so no wake is lost.

**Phase 1's `do/while` recheck loop is deliberately NOT replicated**, and the
reason is that Phase 2 already has the recheck — one level up, in the WAIT gate,
which asks its question through the same `nsfsx_next_actionable` the drain does
(ADR-0025 defect (2)). Only the reset half was missing. Wrapping `nsfsx_drain`
in a loop would have been three regressions:

- its `__super` failures are **no-progress returns**, so the loop would spin on
  a condition its own failure path cannot clear;
- a client may republish the instant it is replied to, so the loop has **no
  finiteness argument** in Phase 2 — Phase 1's rests on its bounded fan-in
  (each blocking app subtask has ≤ 1 outstanding request) and does not transfer
  — and an unbounded drain is what §3's run-to-completion rule forbids, because
  it starves the timers and devices sharing the pass;
- it would quietly turn ADR-0042 §10's **one unit of work per pass** into "serve
  every inline-completing request per pass".

The change is **one statement**. A comment-stripped diff of `src/nsfsx.c`
against `main` is exactly `+ g_wake_ecb = 0u;` and nothing else.

### `wakeposts` changed meaning, and prior measurements are not comparable

Before this step the counter was **monotone and latching by construction**:
nothing cleared the ECB, so once the first POST landed it tracked `evtpasses` at
a constant offset (measured at exactly 3 361 across four readings of one
instance). From this step it counts **wake events** — each observation consumes
the bit. That is the more useful counter, but every `WAKEPOSTS` figure in
`docs/nsf-64-0*.md`, `-64-0b-`, `-64-0c-` was taken under the old semantics, so
a reader comparing across the change is comparing two different things. It is
still not a tally of POSTs: two posts between one observation and the next
coalesce into one, as they do for any ECB. The note lives at the declaration in
`src/nsfsx.c`, which is where such a reader will look.

### What this annotation does not claim

**It does not claim issue #64 is fixed, and #64 is not closed by it.** Four
measurement rounds establish that the stall is not in the WAIT/POST path at all:
the executive is not waiting during a stall (`WCF=0`, `RBXWAIT` and `RBECBWT`
clear, an ordinary PRB — `docs/nsf-64-0c-measurements.md` §6), and its address
space is stuck part-way through an MVS swap-out (`OUCBQFL = X'80'`, `OUCBGOO`
— `docs/nsf-64-0d-measurements.md` §1), which is a state NSF does not write and
cannot see. The **suspension is outside nsf370**.

What the reset *is*, besides an overdue correction, is the one **single-variable
experiment** available: every stall on record occurred on a spinning instance,
and 64-0d names the spin as one of two candidate **provocations** for SRM's
choice of this address space. Removing it changes nothing else, and it becomes
untestable the moment this lands — which is why the accompanying round attempts
a reproduction *after* the reset rather than reporting the CPU drop alone.

Whether NSF should mitigate an MVS condition at all, and in what form, is a
contract-level decision for the maintainer and is not taken here. **No floor was
added; whether one is needed is deliberately left open.**

---

## Annotation (2026-08-27) — pointer: the wake contract is now ADR-0043

**Append-only, and it changes nothing above.** The 64-1 annotation ends *"No floor was added;
whether one is needed is deliberately left open."* That sentence is the one **ADR-0043** closes,
with the measurement that was not available when it was written: on a post-TCP-workload instance
the executive makes **one pass in 259 s** and still serves eight cross-address-space requests
inside a single console second, so no floor is needed for the healthy case
(`docs/nsf-64-1-measurements.md` §4). ADR-0043 records the contract in full — who POSTs, what
resets, that the POST wakes a committed WAIT, and the gaps it does **not** guarantee — and
retires step 64-2 as unnecessary rather than deferred.

The **reset itself remains this ADR's subject**, and the annotation above remains its record.
Issue #64 is unaffected by either and stays open.
