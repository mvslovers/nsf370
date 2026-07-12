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
