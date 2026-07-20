# ADR-0035 — SELECT: one request over N sockets, readiness by poke (M4-5)

**Status:** Proposed (2026-07-19). Pins how NSF implements the EZASOKET **SELECT**
verb and the socket **readiness** model it rests on, for the M4-5 EZASOKET M4 set.
SELECT is the one M4-5 verb with no precedent in the codebase: every other verb is
one request bound to one socket (`pend_recv`/`pend_accept`/`pend_connect`/
`pend_send`), while SELECT is **one NSFRQE waiting on N sockets** for a readiness
edge on **any** of them. **No behaviour change to TCP or the frozen NSFRQE** — this
adds a socket-layer readiness probe + a small parked-SELECT engine, reached through
registration seams so no protocol/request test gains a link dependency.
**Relates to:** spec §10.3 (the parked-request pattern; "SELECT is the same pattern
across multiple sockets with a shared ECB"), §15.2 (the M4 verb set), the M4-5
conformance addendum (`docs/ezasoket-conformance.md` §2/§3: right-to-left masks,
SOPT/GOPT, EINPROGRESS), ADR-0029 (the NSFEZA facades + core), ADR-0031/0032
(the TCP connection machine + data path whose readiness edges SELECT observes),
ADR-0022 (single-task, run-to-completion; the completion POST seam).

## Context

`select()` asks: of these sockets, which are ready to read, ready to write, or in
an exceptional state — and block (optionally with a timeout) until at least one is.
Three properties make it awkward against the M3-1 socket model:

1. **N sockets, one request.** The `pend_*` slots hold at most one request per kind
   per socket. SELECT cannot ride them: it is parked on a *set* of sockets and woken
   by an edge on any one. It needs its own parked-state object.
2. **Readiness is a level, tested without side effects.** A blocking RECV *consumes*
   data; SELECT must *peek* whether a RECV would succeed, on every socket in the set,
   without dequeuing anything or parking a per-socket request.
3. **A timeout that is not a per-socket timer.** SELECT's TIMEOUT needs an embedded
   `TMR`, but the NSFRQE is frozen (no room for a timer) and the timer is not owned
   by any one socket in the set.

The masks are also **socket NUMBERS** (the 0-based halfword the app sees), not the
internal `(gen<<16)|id` descriptors the executive works in — so numbering must be
translated somewhere.

## Decision

### 1. Readiness is a side-effect-free PROTOPS probe (`poll`), trailing member

Add a **last** member to `PROTOPS`:

```c
int (*poll)(SOCKCB *s, int want);   /* SELECT readiness, side-effect-free */
```

`want` is a mask of `SEL_READ`/`SEL_WRITE`; the op returns the subset that is ready
**now**, never blocking, never parking, never dequeuing. It is the LAST member on
purpose (the `accept` precedent, M4-2): every positional PROTOPS initializer that
omits it (UDP's `g_udp_ops`, the M3 dummy protocols) gets `poll == NULL`, and a NULL
`poll` falls back to the **generic rule** in the socket layer:

- **read-ready** ⟺ `rxq` non-empty **or** `acceptq` non-empty;
- **write-ready** ⟺ always (a datagram socket can always be handed a send).

That fallback is exactly right for UDP, so **NSFUDP needs no code change**. TCP
supplies a precise `tcp_poll` (static, reached through the vtable):

- **read-ready** ⟺ `rxq` non-empty, **or** `acceptq` non-empty (a listener with a
  pending connection — ACCEPT counts as a read op, per the addendum), **or**
  `TCB_F_RCVFIN` (EOF: a RECV would return 0 immediately — level-triggered readable
  for everyone);
- **write-ready** ⟺ state ∈ {ESTABLISHED, CLOSE_WAIT} **and** `sndq_bytes < SNDBUF`
  (send would accept ≥ 1 byte). A connect still in progress (SYN_SENT/SYN_RCVD) is
  **not** write-ready; a connect that *completes* becomes write-ready via the state
  transition (a nonblocking CONNECT selects for write on completion — the addendum
  rule). A connect that *fails* tears the socket down → see §4.
- **exception**: never. NSF v1 supports no TAKESOCKET, the only EZASOKET exception
  source, so `ERETMSK` is always returned zero (documented; addendum §2 SELECT).

### 2. Parked SELECT state: a fixed static pool of SELCBs with embedded TMRs

`src/nsfsel.c` owns a **static array of `NSFSEL_MAX` (4) `SELCB`s** — no
`mm_pool_create`, no runtime allocation (a static object satisfies §3's "TMR
embedded in its owner CB, arming cannot fail" without a pool at all). Four is the
bound on concurrently parked SELECTs; a fifth is rejected with a clean errno
(`NSF_EMFILE`). One SELECT per app task is IBM's own effective model, and v1 has a
single INITAPI per address space, so four covers the realistic fan-out; the cap is a
named constant.

```c
typedef struct selcb {
    UCHAR       busy;           /* slot occupied                              */
    NSFRQE     *req;            /* the parked SELECT request (app owns it)     */
    NSFSELITEM *items;          /* app-side descriptor/interest array (ubuf)   */
    UINT        nitems;         /* r->ulen                                     */
    UINT        apptok;         /* owning app instance (teardown scoping)      */
    TMR         tmr;            /* TIMEOUT (embedded; tmr_cancel mandatory)    */
} SELCB;
```

The `TMR` is embedded (never allocated); `tmr_cancel` is idempotent and runs on
**every** exit path (ready, timeout, teardown) — the same discipline the TCP timers
follow.

### 3. Parameter marshalling: an item array in `ubuf`, numbering stays in the facade

The masks are socket numbers; the executive knows only internal descriptors; only
NSFEZA owns the number→descriptor mapping table. So **the facade translates**, and
the request carries a **descriptor/interest array** rather than a bitmask:

```c
typedef struct nsfselitem {
    UINT  desc;    /* internal (gen<<16)|id                                   */
    UCHAR want;    /* SEL_READ|SEL_WRITE requested for this socket             */
    UCHAR ready;   /* subset that is ready (the EXECUTIVE writes this back)    */
    UCHAR rsvd[2];
} NSFSELITEM;
```

`nsf_select` reads the right-to-left in-masks (§5), and for every socket number set
in any in-mask resolves its descriptor from the mapping table and appends an item
(`want` = the union of the read/write bits set for that number). It passes the array
by the frozen NSFRQE's designated buffer word:

- `r->fn = RQ_SELECT`, `r->sockdesc = 0` (a SELECT is not parked on any one socket's
  pend slot — 0 makes `soc_complete`'s pend-slot clear a clean `sock_lookup(0)→NULL`
  no-op);
- `r->ubuf = items`, `r->ulen = nitems`;
- `r->p1 = tv_sec`, `r->p2 = tv_usec`, `r->p3` = a flags word
  (`SEL_TV_PRESENT` / `SEL_TV_FOREVER`), so poll (0/0), timed, and block-forever
  (negative timeout) are distinguished without a fourth pointer.

The executive-side handler works **only in descriptors**, writing each item's
`ready`; NSFEZA maps `ready` back to the return masks and sets `RETCODE` = the count
of ready sockets. **Phase-2 note (not built now):** in Phase 2 `ubuf` becomes a keyed
cross-memory move — the SSI transport copies the item array in, the executive fills
`ready`, and the transport copies it back out, exactly as `ubuf` already models for
SENDTO/RECVFROM. The NSFRQE layout does not change; SELECT state lives outside it.

### 4. Readiness reaches a parked SELECT by a POKE — one function, at the edge

A parked SELECT is re-evaluated when a socket's readiness could have changed. The
coupling is one function, registered so `nsfsel` stays out of the protocol link
graph (the `nsfip_register_proto` / `evt_set_request` pattern):

- `nsfsoc.c` gains `soc_set_select_notify(fn)` and `soc_notify_ready(SOCKCB *s,
  UCHAR events)`. Protocol code calls `soc_notify_ready`; it forwards to the
  registered SELECT engine **iff linked** (NULL → no-op). Because `soc_notify_ready`
  lives in `nsfsoc.c` — already linked wherever TCP/UDP are — the protocols take no
  new dependency; a build without `nsfsel.c` (tsttcp/tstudp/tstreq) simply pokes into
  a NULL notify.
- `nsfreq.c` gains `nsfreq_register_select(fn)`. `RQ_SELECT` dispatches to the
  registered handler if present, else completes `NSF_EOPNOTSUPP` (the M3-2 behaviour
  — so tstreq's assertions hold unchanged).

**Poke placement — the load-bearing rule.** A readiness poke goes at the **queue or
state change**, unconditionally, **never gated on a `pend_*` slot**. The `pend_*`
slot is NULL precisely when a SELECT (not a per-socket blocking call) is the waiter,
so any poke behind `if (pend_recv)` / behind `tcp_send_resume`'s `pend_send == NULL`
early return is dead code for SELECT. Concretely:

| Edge | Poke site | Events |
|---|---|---|
| data queued / drained | **end** of `tcp_recv_data`, after the pend_recv drain | READ |
| datagram queued | `nsfudp_input`, after the rxq enqueue / pend_recv drain | READ |
| EOF | `tcp_handle_fin`, after `TCB_F_RCVFIN` is set | READ |
| connection accepted | `tcp_deliver_child`, after the acceptq enqueue / deliver | READ (on the **listener**) |
| connect completes | `tcp_enter_established` | WRITE |
| send budget freed | `tcp_process_ack`, after SND.UNA advances | WRITE |

The read poke is placed at the **end** of `tcp_recv_data` (after the pend_recv drain)
on purpose: it reflects the *final* rxq state, which resolves the concurrent case in
§6. Every poke fires synchronously inside the executive's event-dispatch pass, so a
parked SELECT completes on the **same pass** as the edge — no new executive hook, no
lost wakeup (the completion POSTs the app's ECB; the loop needs no wake for that).

On a poke, the engine re-scans **every** parked SELCB fully (≤ 4 × ≤ 64 items —
trivial): call `sock_lookup(item.desc)` and either `s->ops->poll(s, want)` or the
generic fallback; sum the ready count; if > 0, write each `ready`, cancel the TMR,
free the slot, and `soc_complete(req, count, 0)`.

### 5. Masks: right-to-left bit numbering in fullwords, byte-wise

Per the addendum (§5.4.24, the part everyone gets wrong): descriptor **N** is bit
`N mod 32` **from the least-significant end** of fullword `N div 32` — `0x00000001`
in the first fullword is descriptor 0, `0x80000000` is descriptor 31, the second
fullword covers 32–63. Mask length = `((maxdesc + 32) / 32) * 4` bytes. NSFEZA reads
and writes each fullword **byte by byte** (the M2 big-endian discipline), so the one
source is correct on the big-endian target and the little-endian host, and the
0/31/32 fullword-boundary vectors are asserted **literally** against the addendum,
never by round-trip. `MAXSOC` caps the scan (only descriptors 0..MAXSOC−1 examined);
zero send-masks with `MAXSOC ≤ 0` is "SELECT as a pure timer".

### 6. Interactions, pinned

- **Teardown while parked → `NSF_ECONNABORTED`.** When any socket in a parked
  SELECT's set is destroyed, that SELECT completes with `RETCODE −1`,
  `NSF_ECONNABORTED` — the same errno every parked NSFRQE gets on teardown (§10.5),
  not a "mark it ready" fiction. `soc_destroy` calls `soc_notify_ready(s, SEL_DEAD)`
  at the **top** of the checklist (before the generation bump, while `soc_desc(s)`
  still resolves), so the engine finds every parked SELECT whose item set contains
  that descriptor and aborts it. This is forget-proof: it is one line in the ONE
  teardown checklist, so no close/reset/shutdown path can skip it. A RST in a
  synchronized state is exactly this path (`tcp_do_reset → soc_destroy`), so "readable
  after RST" never arises — the socket has vanished.
- **SELECT + a concurrent RECV on the same socket.** The stack is multi-subtask
  (nsfthr): subtask A may SELECT on `s` while subtask B blocks in RECV on `s`. Rule:
  data lands on `s->rxq`; `tcp_recv_data` drains it into B's `pend_recv`; **then** the
  end-of-function poke re-evaluates A's SELECT against the now-**empty** rxq, so the
  SELECT reports `s` **not** read-ready. B (the parked RECV) wins the datum; A is not
  spuriously woken. (Level-triggered readability is preserved for EOF, which is not
  consumed by a RECV.)
- **A stale/closed descriptor in the set.** `sock_lookup` returns NULL for a
  descriptor already closed before the scan; that item contributes no readiness (it
  is skipped). A socket closed *while* the SELECT is parked takes the teardown path
  above (ECONNABORTED) instead, so a set that goes entirely dead never hangs.
- **Poll form (0/0 timeout) and pure timer.** Count > 0 on first scan → complete
  immediately (`RETCODE` = count). Count 0 with a 0/0 timeout → complete immediately
  with `RETCODE` 0 (no park). Count 0 with a finite timeout → park + arm the TMR;
  the TMR callback completes `RETCODE` 0 with all masks zeroed. Count 0 with a
  negative (forever) timeout → park with no TMR.

### 7. Non-blocking (FIONBIO) selects for write on connect completion

FIONBIO's persistent non-blocking flag is a facade concern (a bit next to the
descriptor in NSFEZA's mapping table — SOCKCB stays spec-exact, no flags field), so
it is specified in the conformance doc, not here. The SELECT-relevant contract: a
**nonblocking CONNECT** returns `RETCODE −1` / `ERRNO 36` (`NSF_EINPROGRESS`, Table
67-verified) and the connection proceeds; when it completes, `tcp_enter_established`'s
WRITE poke reports the socket write-ready — the standard "wait for a nonblocking
connect with select-for-write" idiom.

## Consequences

- **New component `NSFSEL`** (`src/nsfsel.c` / `include/nsfsel.h`): the SELCB pool,
  the scan/park/re-eval engine, the `soc_set_select_notify` registration, and
  `nsfsel_dispatch` (the `RQ_SELECT` handler registered with NSFREQ). Unique 8-char
  aliases, scheme `NSFSL*`. Linked only where SELECT is reachable (the `NSF`/`NSFECHO`
  /`NSFTECHO` modules and the SELECT tests) — tcp/udp/req tests are unchanged.
- **`PROTOPS` grows one trailing member (`poll`)** — zero-fills for every non-TCP
  initializer, so no UDP/dummy edit; `SOCKCB` is untouched (still 76 B, no flags
  field). Two `nsfsoc.c` additions (`soc_set_select_notify`, `soc_notify_ready`) and
  one `soc_destroy` line (the SEL_DEAD poke). One `nsfreq.c` addition
  (`nsfreq_register_select`) and the `RQ_SELECT` case.
- **`NSFRQE` is not touched** — SELECT rides `ubuf`/`ulen`/`p1`/`p2`/`p3`; the freeze
  holds. `NSF_EINPROGRESS` (36) is added to the provisional errno set (a value, not a
  layout change).
- **Bounded, non-abending.** ≤ 4 parked SELECTs; a fifth → `NSF_EMFILE`. No allocation
  on any path; the TMR is embedded; teardown completes every parked SELECT. The
  readiness scan is `O(parked × maxsoc)` and only runs while a SELECT is parked.
- **Host-testable without the loop.** The readiness probe, mask translation, park,
  poke-completion, and timeout are all exercised by direct calls (`nsfreq_dispatch` +
  `nsftmr_run` for the timeout), because the host STIMER seam never posts the loop's
  timer ECB — the same reason the M0-5 timer tests drive `nsftmr_run` directly.
