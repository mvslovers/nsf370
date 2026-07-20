/*
 * tstloss.c -- NSFTCP loss-injection harness (spec ch. 13, M4-6 exit gate).
 *
 * HOST-ONLY (mvs = false). Drives TWO real NSF TCP sockets on ONE stack instance
 * (same HOME_IP; the 4-tuple demux distinguishes them because the port pairs are
 * swapped) through a lossy loopback, so the M4-3 copy-on-transmit data path and
 * the M4-4 RTO/persist machinery are exercised end to end against synthetic
 * drop / duplicate / reorder faults -- the systematic loss matrix the live CTCI
 * link (lossless without root) cannot produce.
 *
 * ARCHITECTURE -- a SYNCHRONOUS single-threaded pump (no event loop, no threads,
 * the M4 test contract). The pieces:
 *
 *   - the lossy WIRE: a capture DEVOPS (wire_send) whose send copies the outbound
 *     frame's raw bytes onto a bounded FIFO and frees the PBUF (exactly cap_send
 *     in tsttcp.c, but queued instead of overwritten). The wire holds COPIED
 *     BYTES, never PBUFs -- so it never fights the pool or the single-owner rule;
 *     a re-inject re-allocates a fresh RX PBUF.
 *
 *   - the FAULT stage: applied when the pump DRAINS the wire (not inside
 *     wire_send -- draining re-enters output, and faulting there would recurse).
 *     A seeded PRNG governs drop / dup / reorder; the seed is printed on every
 *     path so any failure reproduces. "Reorder" holds one frame and releases it
 *     after the next -- so the held frame arrives out of order (costs an oooseg
 *     drop + a retransmit, the documented v1 cost with no reassembly).
 *
 *   - the PUMP: each round (1) feeds the sender / receiver apps through the real
 *     M4-5 verbs (nsfreq_dispatch), (2) drains a SNAPSHOT of the wire -- the
 *     frames captured BEFORE this round; frames that injection appends belong to
 *     the NEXT round, which is what stops the drain spinning as ACKs/data
 *     regenerate -- applying faults and re-injecting survivors, and (3) if nothing
 *     moved, flushes a reorder-held frame, else JUMPS simulated time to the next
 *     armed timer expiry (firing rexmit / persist), else declares a real deadlock
 *     and FAILs with the seed. Simulated time (nsftmr_run) means an RTO backoff
 *     costs CPU, not wall-clock.
 *
 * THE ONE RULE (kickoff): the harness VALIDATES the stack; it must not paper over
 * a stall. If the pump stalls and rexmit/persist does not recover it, that is a
 * TCP bug to STOP and report -- never a harness nudge. The concrete trap is a
 * dropped window-update ACK: the sender sits at window 0 with data queued and
 * nothing in flight, and PERSIST (ADR-0033) must probe it open. The pump simply
 * advances time to the persist expiry; if persist works, the transfer completes;
 * if it does not, the deadlock path FAILs -- it does not invent progress.
 *
 * COVERAGE: 5 % drop / drop-bursts / 5 % dup / reorder / combined (3+ seeds, ×20
 * rotating) / SYN+FIN loss, each a 1 MB (or scaled) seeded pseudo-random transfer
 * verified BYTE-EXACT with an orderly FIN teardown and a per-scenario leak gate;
 * plus TIME_WAIT reclaim under pool pressure driven through the real verbs.
 */
#include "nsftcp.h"
#include "nsfip.h"
#include "nsficmp.h"
#include "nsfsoc.h"
#include "nsfreq.h"
#include "nsfdev.h"
#include "nsfbuf.h"
#include "nsfevt.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define ctr_get(comp, name)  sts_value((comp), (name))

#define HOME_IP   0x0A010102u       /* 10.1.1.2 -- this stack (both endpoints)   */

/* A TCPTCB pool big enough for the two live TCBs of a transfer plus a listener,
 * with slack; the TIME_WAIT-reclaim scenario uses a deliberately small window
 * (TWPOOL) inside this. */
#define TSTLOSS_POOL   40

/* Pump safety cap -- far above any real transfer's round count; hitting it means
 * a genuine deadlock (reported with the seed), never normal slowness. */
#define PUMP_MAX_ROUNDS  4000000u

/* Livelock watchdog: a real transfer advances app-level state (rcvlen / socket
 * states) constantly, so this many rounds with NO app progress -- even while
 * frames keep flowing -- is a livelock (e.g. a persist/window-update stall). Far
 * above any real transfer's total round count (~4k for 1 MB), so no false fire.
 * This is what makes the harness FAIL FAST + informatively on a regression the
 * timer-jump stall path cannot see (frames flowing, no progress). */
#define LIVELOCK_LIMIT   200000u

/* retcode sentinel for a parked (not-yet-completed) request (as in tsttcp.c). */
#define REQ_PENDING  0x7F7F7F7F

/* ============================================================================
 * Seeded PRNG (xorshift32) -- deterministic; the seed is printed so any failure
 * is reproducible by re-running with it.
 * ========================================================================== */
static UINT g_rng;

static void rng_seed(UINT s)
{
    g_rng = (s != 0u) ? s : 0xA5A5A5A5u;    /* xorshift never seeds with 0 */
}
static UINT rng_next(void)
{
    UINT x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}
/* True with probability pct/100. */
static int rng_pct(UINT pct)
{
    return (UINT)(rng_next() % 100u) < pct;
}

/* ============================================================================
 * The lossy WIRE: a bounded FIFO of captured outbound frames (bytes, not PBUFs).
 * ========================================================================== */
#define WIRE_MAX    512
#define FRAME_MAX   2048

typedef struct {
    UCHAR  buf[WIRE_MAX][FRAME_MAX];
    USHORT len[WIRE_MAX];
    int    head, tail, count;
} WIRE;

static WIRE g_wire;

static void wire_reset(void)
{
    g_wire.head = g_wire.tail = g_wire.count = 0;
}
static int wire_push(const UCHAR *bytes, USHORT n)
{
    if (g_wire.count >= WIRE_MAX || n > FRAME_MAX) {
        return -1;                          /* wire overflow -- a harness bug */
    }
    memcpy(g_wire.buf[g_wire.tail], bytes, n);
    g_wire.len[g_wire.tail] = n;
    g_wire.tail = (g_wire.tail + 1) % WIRE_MAX;
    g_wire.count++;
    return 0;
}
/* Pop the oldest frame into `out` (>= FRAME_MAX). Returns its length, or 0 when
 * the wire is empty. */
static USHORT wire_pop(UCHAR *out)
{
    USHORT n;
    if (g_wire.count == 0) {
        return 0u;
    }
    n = g_wire.len[g_wire.head];
    memcpy(out, g_wire.buf[g_wire.head], n);
    g_wire.head = (g_wire.head + 1) % WIRE_MAX;
    g_wire.count--;
    return n;
}

/* Capture DEVOPS: copy the outbound frame onto the wire, free the PBUF (send
 * takes ownership, spec 9.2). Loss/dup/reorder happen later, at drain time. */
static int wire_dev_init(NETDEV *d, const DEVCFG *c) { (void)d; (void)c; return 0; }
static int wire_dev_start(NETDEV *d)                 { (void)d; return 0; }
static int wire_dev_shutdown(NETDEV *d)              { (void)d; return 0; }
static int wire_dev_send(NETDEV *d, PBUF *b)
{
    UCHAR  frame[FRAME_MAX];
    USHORT n;
    (void)d;
    n = buf_copyout(b, frame, (USHORT)sizeof(frame));
    (void)wire_push(frame, n);
    buf_free(b);
    return 0;
}
static DEVOPS g_wire_ops = { wire_dev_init, wire_dev_start, wire_dev_send,
                             wire_dev_shutdown };

/* ============================================================================
 * The FAULT stage.
 * ========================================================================== */
typedef struct {
    UINT drop_pct;
    UINT dup_pct;
    UINT reorder_pct;
    /* burst: drop the next `burst_left` frames unconditionally, armed once when
     * the transfer has moved `burst_at` bytes (0 => never). */
    UINT burst_at;
    UINT burst_len;
    /* Deterministic, targeted losses: drop the first `drop_syn` SYN-bearing and
     * first `drop_fin` FIN-bearing frames unconditionally. This forces the M4-4
     * SYN-rexmit / FIN-rexmit paths with ZERO give-up risk (< MAXTRIES), so the
     * SYN/FIN-loss scenario is pointed and not seed-flaky. */
    UINT drop_syn;
    UINT drop_fin;
} FAULTCFG;

typedef struct {
    const FAULTCFG *cfg;
    int    held_valid;                      /* a reorder-held frame is stashed   */
    UCHAR  held[FRAME_MAX];
    USHORT held_len;
    UINT   burst_left;
    int    burst_armed;
    UINT   syn_left, fin_left;              /* remaining targeted SYN/FIN drops  */
    /* counts (for the harness's own reporting, not asserted against the stack) */
    UINT   n_dropped, n_dup, n_reordered, n_delivered;
} FAULT;

static void fault_init(FAULT *f, const FAULTCFG *cfg)
{
    memset(f, 0, sizeof(*f));
    f->cfg      = cfg;
    f->syn_left = cfg->drop_syn;
    f->fin_left = cfg->drop_fin;
}

/* Deliver `bytes[0..n)` inbound through the real IP path (dst is local) and flush
 * any reply into the wire. Declared here; defined once the device is in scope. */
static void inject_frame(NETDEV *dev, const UCHAR *bytes, USHORT n);

/* Arm the mid-transfer drop burst once `moved` bytes have been transferred. */
static void fault_maybe_arm_burst(FAULT *f, UINT moved)
{
    if (!f->burst_armed && f->cfg->burst_at != 0u && moved >= f->cfg->burst_at) {
        f->burst_left = f->cfg->burst_len;
        f->burst_armed = 1;
    }
}

/* Run one captured frame through the fault stage, injecting whatever survives.
 * Returns the number of frames actually injected (0 if dropped/held). */
static int fault_process(FAULT *f, NETDEV *dev, const UCHAR *bytes, USHORT n)
{
    int injected = 0;

    /* Deterministic targeted SYN/FIN drops (before any random roll). */
    if (f->syn_left > 0u || f->fin_left > 0u) {
        const UCHAR *t  = bytes + ((bytes[0] & 0x0Fu) * 4u);
        UCHAR        fl = t[13];
        if ((fl & (UCHAR)TCP_FL_SYN) && f->syn_left > 0u) {
            f->syn_left--; f->n_dropped++; return 0;
        }
        if ((fl & (UCHAR)TCP_FL_FIN) && f->fin_left > 0u) {
            f->fin_left--; f->n_dropped++; return 0;
        }
    }
    /* An armed drop burst swallows consecutive frames unconditionally. */
    if (f->burst_left > 0u) {
        f->burst_left--;
        f->n_dropped++;
        return 0;
    }
    /* Random drop. */
    if (rng_pct(f->cfg->drop_pct)) {
        f->n_dropped++;
        return 0;
    }
    /* Reorder: hold this frame if none is held yet; it is released AFTER the next
     * frame passes (below), so it arrives out of order. */
    if (!f->held_valid && rng_pct(f->cfg->reorder_pct)) {
        memcpy(f->held, bytes, n);
        f->held_len = n;
        f->held_valid = 1;
        f->n_reordered++;
        return 0;                           /* emitted later, out of order */
    }
    /* Deliver this frame (once, or twice on a duplication roll). */
    inject_frame(dev, bytes, n);
    injected++;
    f->n_delivered++;
    if (rng_pct(f->cfg->dup_pct)) {
        inject_frame(dev, bytes, n);
        injected++;
        f->n_dup++;
    }
    /* Now release a previously-held frame -- it lands after the current one. */
    if (f->held_valid) {
        inject_frame(dev, f->held, f->held_len);
        f->held_valid = 0;
        injected++;
    }
    return injected;
}

/* Flush a reorder-held frame unconditionally (the pump calls this on a stall so a
 * frame held as the last thing in flight is not stranded -- it still arrives out
 * of order, preserving the reorder semantics, only guaranteeing progress). */
static int fault_flush_held(FAULT *f, NETDEV *dev)
{
    if (f->held_valid) {
        inject_frame(dev, f->held, f->held_len);
        f->held_valid = 0;
        return 1;
    }
    return 0;
}

/* ============================================================================
 * Injection + timer plumbing (mirrors tsttcp.c inject/tick, but the reply goes
 * onto the wire, not a single capture slot).
 * ========================================================================== */
static PBUF *rx_pbuf(const UCHAR *pkt, USHORT len)
{
    PBUF *b = buf_alloc(len);
    if (b == NULL) {
        return NULL;
    }
    buf_reset_rx(b);
    (void)buf_copyin(b, pkt, len);
    return b;
}

static void inject_frame(NETDEV *dev, const UCHAR *bytes, USHORT n)
{
    PBUF *b = rx_pbuf(bytes, n);
    if (b == NULL) {
        CHECK(0, "inject_frame: buf_alloc (pool exhausted -- harness bug)");
        return;
    }
    nsfip_input(dev, b);                     /* validate, demux proto 6 -> TCP  */
    nsfdev_kick_output();                    /* flush replies onto the wire      */
}

/* ============================================================================
 * TCB / socket introspection (re-declared locally; NSF_DEBUG only for the pools).
 * ========================================================================== */
static TCB *tcb_of(UINT desc)
{
    SOCKCB *s = sock_lookup(desc);
    return (s != NULL) ? (TCB *)s->pcb : NULL;
}
static int tcb_state(UINT desc)
{
    TCB *t = tcb_of(desc);
    return (t != NULL) ? (int)t->state : -1;
}
/* Ticks until the NEXT armed timer (across the whole delta queue) fires, or 0 if
 * nothing is armed. The head timer's `delta` is exactly that interval (the
 * delta queue measures the head from "now"); reading an individual TCB's timer
 * would be wrong for a non-head timer (its delta is relative to its
 * predecessor). This is how the pump jumps to the next rexmit / persist expiry. */
static UINT next_expiry(void)
{
    if (nsftmr_count() == 0u) {
        return 0u;
    }
    {
        const TMR *h = nsftmr_peek(0u);
        UINT d = (h != NULL) ? h->delta : 0u;
        return (d != 0u) ? d : 1u;      /* a due-now head still needs one run(1) */
    }
}

#if NSF_DEBUG
static UINT small_inuse(void)
{
    MMSTATS s;
    mm_stats(buf_debug_pool(NSFBUF_CLASS_SMALL), &s);
    return s.inuse;
}
static UINT large_inuse(void)
{
    MMSTATS s;
    mm_stats(buf_debug_pool(NSFBUF_CLASS_LARGE), &s);
    return s.inuse;
}
static UINT tcb_inuse(void)  { return nsftcp_debug_inuse(); }
static UINT sock_inuse(void) { return soc_debug_inuse(); }
#else
static UINT small_inuse(void)  { return 0u; }
static UINT large_inuse(void)  { return 0u; }
static UINT tcb_inuse(void)    { return 0u; }
static UINT sock_inuse(void)   { return 0u; }
#endif

/* ============================================================================
 * App request plumbing (the real M4-5 verbs, one app / apptok, many sockets).
 * ========================================================================== */
static UINT g_apptok;

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
    r->retcode  = REQ_PENDING;
}
static void app_init(void)
{
    NSFRQE r;
    rqe_init(&r, RQ_INITAPI, 0u);
    nsfreq_dispatch(&r);
    CHECK(r.retcode == NSF_RETOK, "INITAPI");
    g_apptok = r.apptok;
}
static void app_term(void)
{
    NSFRQE r;
    rqe_init(&r, RQ_TERMAPI, 0u);
    r.apptok = g_apptok;
    nsfreq_dispatch(&r);
}
/* Dispatch a request and flush any outbound segment onto the wire. */
static void dispatch(NSFRQE *r)
{
    nsfreq_dispatch(r);
    nsfdev_kick_output();
}
static UINT tcp_socket(void)
{
    NSFRQE r;
    rqe_init(&r, RQ_SOCKET, 0u);
    r.apptok = g_apptok; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_STREAM; r.p3 = 6u;
    nsfreq_dispatch(&r);
    return (r.retcode >= 0) ? (UINT)r.retcode : 0u;
}
static int tcp_bind(UINT desc, UINT addr, USHORT port)
{
    NSFRQE r;
    rqe_init(&r, RQ_BIND, desc);
    r.p1 = addr; r.p2 = (UINT)port;
    dispatch(&r);
    return (r.retcode == NSF_RETOK) ? 0 : -1;
}
static int tcp_listen(UINT desc, UINT backlog)
{
    NSFRQE r;
    rqe_init(&r, RQ_LISTEN, desc);
    r.p1 = backlog;
    dispatch(&r);
    return (r.retcode == NSF_RETOK) ? 0 : -1;
}
static void tcp_close(UINT desc)
{
    NSFRQE r;
    rqe_init(&r, RQ_CLOSE, desc);
    dispatch(&r);
}

/* ============================================================================
 * The transfer harness.
 * ========================================================================== */

/* Fill n bytes with a seeded pseudo-random pattern (independent of the fault
 * PRNG so the payload is stable while faults vary). */
static void fill_payload(UCHAR *buf, UINT n, UINT seed)
{
    UINT i, x = (seed != 0u) ? seed : 0x1234567u;
    for (i = 0u; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (UCHAR)(x & 0xFFu);
    }
}

/* Static transfer buffers (host: 1 MB each is fine; keeps the stack small). */
#define XFER_MAX  (1024u * 1024u)
static UCHAR g_src[XFER_MAX];
static UCHAR g_dst[XFER_MAX];

/* Request posters (defined after run_transfer). */
static void req_send_post(NSFRQE *r, UINT desc, const void *buf, UINT len);
static void req_recv_post(NSFRQE *r, UINT desc, void *buf, UINT len);

/* Drive one full connection: A (active) connects to B (listener), A streams
 * `len` bytes of g_src through the lossy wire, B receives into g_dst, both close
 * cleanly. Verifies byte-exact arrival, that the asserted counters ticked, and a
 * per-scenario leak gate. Every CHECK message carries the tag+seed. */
static void run_transfer(NETDEV *dev, const FAULTCFG *cfg, UINT seed,
                         UINT len, const char *tag)
{
    FAULT   flt;
    NSFRQE  rc_conn, rq_acc, rs, rr;
    UINT    ldesc, adesc, cdesc = 0u;
    USHORT  lport = 20000u;
    UINT    rcvlen = 0u;
    int     a_connected = 0, accepted = 0, send_done = 0;
    int     a_closed = 0, got_eof = 0, c_closed = 0;
    UINT    rounds = 0u;
    UINT    base_rexmit, base_dupack, base_oooseg, base_wndprobe;
    UINT    last_sig = 0xFFFFFFFFu, stall = 0u;
    char    msg[160];

    fill_payload(g_src, len, seed ^ 0x9E3779B9u);
    memset(g_dst, 0, len);
    rng_seed(seed);
    fault_init(&flt, cfg);
    wire_reset();

    base_rexmit   = ctr_get("NSFTCP", "rexmit");
    base_dupack   = ctr_get("NSFTCP", "dupack");
    base_oooseg   = ctr_get("NSFTCP", "oooseg");
    base_wndprobe = ctr_get("NSFTCP", "wndprobe");

    printf("  [%s] seed=0x%08X len=%u drop=%u%% dup=%u%% reorder=%u%% burst=%u@%u\n",
           tag, seed, len, cfg->drop_pct, cfg->dup_pct, cfg->reorder_pct,
           cfg->burst_len, cfg->burst_at);

    /* --- set up B (listener) + A (active connect) --- */
    ldesc = tcp_socket();
    CHECK(ldesc != 0u, "listener socket");
    CHECK(tcp_bind(ldesc, HOME_IP, lport) == 0, "listener bind");
    CHECK(tcp_listen(ldesc, 4u) == 0, "listener listen");

    /* A blocking ACCEPT parked up front; it completes when the child graduates. */
    rqe_init(&rq_acc, RQ_ACCEPT, ldesc);
    dispatch(&rq_acc);

    adesc = tcp_socket();
    CHECK(adesc != 0u, "active socket");
    rqe_init(&rc_conn, RQ_CONNECT, adesc);
    rc_conn.p1 = HOME_IP; rc_conn.p2 = (UINT)lport;
    dispatch(&rc_conn);                      /* SYN onto the wire (or dropped) */

    rqe_init(&rs, RQ_SEND, adesc);           /* posted once A is ESTABLISHED */
    rqe_init(&rr, RQ_RECV, 0u);              /* posted once the child exists */

    /* --- the pump --- */
    for (rounds = 0u; rounds < PUMP_MAX_ROUNDS; rounds++) {
        int    moved = 0;
        int    i, snapshot;
        UCHAR  frame[FRAME_MAX];

        /* (1a) A reached ESTABLISHED -> its parked CONNECT completed. */
        if (!a_connected && rc_conn.retcode != REQ_PENDING) {
            CHECK(rc_conn.retcode == NSF_RETOK, "active connect completed OK");
            a_connected = 1;
            req_send_post(&rs, adesc, g_src, len);
            dispatch(&rs);                   /* buffers what fits, parks the rest */
            moved = 1;
        }
        /* (1b) the child graduated -> the parked ACCEPT completed. */
        if (!accepted && rq_acc.retcode != REQ_PENDING) {
            CHECK(rq_acc.retcode > 0, "accept returned a child descriptor");
            cdesc = (UINT)rq_acc.retcode;
            accepted = 1;
            req_recv_post(&rr, cdesc, g_dst, len);
            dispatch(&rr);
            moved = 1;
        }
        /* (1c) the SEND finished buffering all bytes -> close A. */
        if (a_connected && !send_done && rs.retcode != REQ_PENDING) {
            CHECK(rs.retcode == (INT)len, "send buffered the full payload");
            send_done = 1;
        }
        if (send_done && !a_closed) {
            tcp_close(adesc);                /* FIN drains behind the sndq data */
            a_closed = 1;
            moved = 1;
        }
        /* (1d) the RECV completed -> accumulate / EOF, then re-post. */
        if (accepted && !got_eof && rr.retcode != REQ_PENDING) {
            if (rr.retcode > 0) {
                rcvlen += (UINT)rr.retcode;
                req_recv_post(&rr, cdesc, g_dst + rcvlen, len - rcvlen);
                dispatch(&rr);
            } else if (rr.retcode == 0) {
                got_eof = 1;                 /* peer FIN consumed */
            } else {
                CHECK(0, "unexpected RECV error");
                got_eof = 1;
            }
            moved = 1;
        }
        /* (1e) B saw EOF -> close the child. */
        if (got_eof && !c_closed) {
            tcp_close(cdesc);
            c_closed = 1;
            moved = 1;
        }

        fault_maybe_arm_burst(&flt, rcvlen);

        /* (2) drain a SNAPSHOT of the wire through the fault stage. */
        snapshot = g_wire.count;
        for (i = 0; i < snapshot; i++) {
            USHORT n = wire_pop(frame);
            if (n == 0u) {
                break;
            }
            if (fault_process(&flt, dev, frame, n) > 0) {
                moved = 1;
            } else {
                moved = 1;                   /* a drop/hold IS progress (consumed) */
            }
        }

        /* (5) done? all bytes received + EOF + both closed + both TCBs terminal
         * + the wire drained. */
        if (got_eof && a_closed && c_closed && rcvlen == len &&
            g_wire.count == 0 &&
            (tcb_state(adesc) == TCP_TIME_WAIT || tcb_state(adesc) < 0) &&
            tcb_state(cdesc) < 0) {
            break;
        }

        /* (3) livelock watchdog: frames may keep flowing (moved stays 1), so the
         * stall path below never fires -- catch a NO-APP-PROGRESS spin here. */
        {
            UINT sig = rcvlen
                     ^ ((UINT)(a_connected | (accepted << 1) | (send_done << 2) |
                               (a_closed << 3) | (got_eof << 4) | (c_closed << 5)) << 24)
                     ^ ((UINT)(tcb_state(adesc) + 2) << 8)
                     ^ ((UINT)(tcb_state(cdesc) + 2) << 16);
            if (sig == last_sig) {
                if (++stall > LIVELOCK_LIMIT) {
                    snprintf(msg, sizeof(msg),
                             "%s seed=0x%08X: LIVELOCK (no app progress for %u rounds) "
                             "rcvlen=%u/%u eof=%d astate=%d cstate=%d",
                             tag, seed, stall, rcvlen, len, got_eof,
                             tcb_state(adesc), tcb_state(cdesc));
                    CHECK(0, msg);
                    break;
                }
            } else {
                stall = 0u;
                last_sig = sig;
            }
        }

        if (moved) {
            continue;
        }

        /* (4) STALLED: no app move, no frame injected, wire empty. */
        /* (4a) flush a reorder-held frame first. */
        if (fault_flush_held(&flt, dev)) {
            continue;
        }
        /* (4b) advance simulated time to the next armed timer expiry (rexmit /
         * persist). One jump per stall -- an RTO backoff of 64 s = 640 ticks is a
         * single nsftmr_run, not 640 no-op rounds. */
        {
            UINT jump = next_expiry();
            if (jump != 0u) {
                nsftmr_run(jump);
                nsfdev_kick_output();
                continue;
            }
        }
        /* (4c) nothing armed, still not done -> a REAL deadlock. This is the
         * kickoff's stop-and-report line: the harness FAILs, it does not nudge. */
        snprintf(msg, sizeof(msg),
                 "%s seed=0x%08X: DEADLOCK (no progress, no timer) "
                 "rcvlen=%u/%u eof=%d aclosed=%d cclosed=%d astate=%d cstate=%d",
                 tag, seed, rcvlen, len, got_eof, a_closed, c_closed,
                 tcb_state(adesc), tcb_state(cdesc));
        CHECK(0, msg);
        break;
    }

    CHECK(rounds < PUMP_MAX_ROUNDS, "pump completed within the round cap");

    /* --- verify byte-exact arrival + orderly teardown --- */
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: received all %u bytes", tag, seed, len);
    CHECK_EQ((long)rcvlen, (long)len, msg);
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: payload byte-exact", tag, seed);
    CHECK(memcmp(g_src, g_dst, len) == 0, msg);

    /* Reclaim A's TIME_WAIT, then close the listener. */
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    nsfdev_kick_output();
    tcp_close(ldesc);
    /* Any final teardown frames (unlikely) shed through one drain. */
    {
        int i, snap = g_wire.count;
        UCHAR frame[FRAME_MAX];
        for (i = 0; i < snap; i++) {
            USHORT n = wire_pop(frame);
            if (n == 0u) break;
            inject_frame(dev, frame, n);
        }
    }
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    nsfdev_kick_output();

    /* --- counters that MUST have ticked for this scenario --- */
    if (cfg->drop_pct != 0u || cfg->burst_len != 0u ||
        cfg->drop_syn != 0u || cfg->drop_fin != 0u) {
        snprintf(msg, sizeof(msg), "%s seed=0x%08X: retransmissions occurred", tag, seed);
        CHECK(ctr_get("NSFTCP", "rexmit") > base_rexmit, msg);
    }
    if (cfg->dup_pct != 0u) {
        snprintf(msg, sizeof(msg), "%s seed=0x%08X: duplicate ACKs seen", tag, seed);
        CHECK(ctr_get("NSFTCP", "dupack") > base_dupack, msg);
    }
    if (cfg->reorder_pct != 0u) {
        snprintf(msg, sizeof(msg), "%s seed=0x%08X: out-of-order drops seen", tag, seed);
        CHECK(ctr_get("NSFTCP", "oooseg") > base_oooseg, msg);
    }
    (void)base_wndprobe;

    /* --- per-scenario leak gate --- */
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: no sockets left open", tag, seed);
    CHECK_EQ((long)soc_count(), 0L, msg);
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: TCB pool baseline", tag, seed);
    CHECK_EQ((long)tcb_inuse(), 0L, msg);
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: SOCKET pool baseline", tag, seed);
    CHECK_EQ((long)sock_inuse(), 0L, msg);
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: BUFSMALL baseline", tag, seed);
    CHECK_EQ((long)small_inuse(), 0L, msg);
    snprintf(msg, sizeof(msg), "%s seed=0x%08X: BUFLARGE baseline", tag, seed);
    CHECK_EQ((long)large_inuse(), 0L, msg);

    printf("    -> ok: delivered=%u dropped=%u dup=%u reordered=%u rounds=%u\n",
           flt.n_delivered, flt.n_dropped, flt.n_dup, flt.n_reordered, rounds);

    /* Defensive teardown so a FAILED transfer does not cascade into the next
     * scenario's (cumulative-pool) leak gate. On success everything is already
     * closed + reclaimed, so these are no-ops. */
    if (tcb_state(adesc) >= 0) { tcp_close(adesc); }
    if (cdesc != 0u && tcb_state(cdesc) >= 0) { tcp_close(cdesc); }
    if (tcb_state(ldesc) >= 0) { tcp_close(ldesc); }
    {
        int pass, i, snap;
        UCHAR frame[FRAME_MAX];
        for (pass = 0; pass < 4; pass++) {
            snap = g_wire.count;
            for (i = 0; i < snap; i++) {
                USHORT n = wire_pop(frame);
                if (n == 0u) { break; }
                inject_frame(dev, frame, n);
            }
            nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
            nsfdev_kick_output();
        }
        wire_reset();
    }
}

/* Build a SEND / RECV request (retcode = REQ_PENDING so a parked request is
 * detectable). */
static void req_send_post(NSFRQE *r, UINT desc, const void *buf, UINT len)
{
    rqe_init(r, RQ_SEND, desc);
    r->ubuf = (void *)buf; r->ulen = len; r->flags = 0u;
}
static void req_recv_post(NSFRQE *r, UINT desc, void *buf, UINT len)
{
    rqe_init(r, RQ_RECV, desc);
    r->ubuf = buf; r->ulen = len; r->flags = 0u;
}

/* ============================================================================
 * TIME_WAIT reclaim under pool pressure (part C).
 *
 * The guest is the ACTIVE closer, so its side enters TIME_WAIT each cycle. We do
 * NOT advance the 2MSL clock between cycles, so those TIME_WAIT TCBs accumulate
 * until the pool is full; a further connection then reclaims the oldest TIME_WAIT
 * (twreclaim ticks) and keeps succeeding. This is the M4-2 unit test grown into
 * an end-to-end scenario through the real verbs -- and it needs the active-path
 * reclaim (tcp_attach), the M4-6 fold-in fix, or the guest's own accumulated
 * TIME_WAITs wall its next active socket() at EMFILE.
 * ========================================================================== */
static void run_reclaim_test(NETDEV *dev)
{
    UINT   ldesc;
    USHORT lport = 21000u;
    UINT   base_recl;
    UINT   established = 0u;
    int    cyc, ok = 1;
    const int CYCLES = TSTLOSS_POOL + 12;       /* well past the pool -> reclaim  */

    wire_reset();
    printf("  [reclaim] %d active connect->close cycles, pool=%d TCBs\n",
           CYCLES, TSTLOSS_POOL);

    ldesc = tcp_socket();
    CHECK(ldesc != 0u, "reclaim: listener socket");
    CHECK(tcp_bind(ldesc, HOME_IP, lport) == 0, "reclaim: bind");
    CHECK(tcp_listen(ldesc, 8u) == 0, "reclaim: listen");
    base_recl = ctr_get("NSFTCP", "twreclaim");

    for (cyc = 0; cyc < CYCLES && ok; cyc++) {
        NSFRQE rc_conn, rq_acc;
        UINT   adesc, cdesc = 0u, rounds;
        int    est = 0, accepted = 0, c_closed = 0, done = 0;

        rqe_init(&rq_acc, RQ_ACCEPT, ldesc);    /* a fresh accept for this child   */
        dispatch(&rq_acc);

        adesc = tcp_socket();                   /* reclaims a TIME_WAIT if full    */
        if (adesc == 0u) { ok = 0; break; }     /* active alloc walled at EMFILE   */
        rqe_init(&rc_conn, RQ_CONNECT, adesc);
        rc_conn.p1 = HOME_IP; rc_conn.p2 = (UINT)lport;
        dispatch(&rc_conn);

        for (rounds = 0u; rounds < 200000u && !done; rounds++) {
            int i, snap;
            UCHAR frame[FRAME_MAX];

            if (!est && rc_conn.retcode != REQ_PENDING) {
                if (rc_conn.retcode != NSF_RETOK) { ok = 0; break; }
                est = 1; established++;
                tcp_close(adesc);               /* ACTIVE close -> A -> TIME_WAIT  */
            }
            if (!accepted && rq_acc.retcode != REQ_PENDING) {
                accepted = 1;
                if (rq_acc.retcode > 0) { cdesc = (UINT)rq_acc.retcode; }
            }
            if (accepted && cdesc != 0u && !c_closed && tcb_state(cdesc) >= 0) {
                tcp_close(cdesc);               /* child passive-closes -> freed   */
                c_closed = 1;
            }
            snap = g_wire.count;
            for (i = 0; i < snap; i++) {
                USHORT n = wire_pop(frame);
                if (n == 0u) { break; }
                inject_frame(dev, frame, n);
            }
            if (est && accepted &&
                (tcb_state(adesc) == TCP_TIME_WAIT || tcb_state(adesc) < 0) &&
                (cdesc == 0u || tcb_state(cdesc) < 0) && g_wire.count == 0) {
                done = 1;
                break;
            }
            if (g_wire.count == 0) {
                UINT j = next_expiry();
                if (j != 0u) { nsftmr_run(j); nsfdev_kick_output(); }
            }
        }
        if (!done) { ok = 0; }
        /* deliberately NO 2MSL advance: A's TIME_WAIT lingers + accumulates */
    }

    CHECK(ok, "reclaim: every active connect->close cycle completed (no EMFILE wall)");
    CHECK_EQ((long)established, (long)CYCLES, "reclaim: all connections established");
    CHECK(ctr_get("NSFTCP", "twreclaim") > base_recl,
          "reclaim: TIME_WAIT reclaim fired under pool pressure");
    printf("    -> reclaim: %u/%d cycles, twreclaim delta=%u\n",
           established, CYCLES, ctr_get("NSFTCP", "twreclaim") - base_recl);

    /* Drain every lingering TIME_WAIT + close the listener -> pools to baseline. */
    tcp_close(ldesc);
    {
        int pass, i, snap;
        UCHAR frame[FRAME_MAX];
        for (pass = 0; pass < 4; pass++) {
            snap = g_wire.count;
            for (i = 0; i < snap; i++) {
                USHORT n = wire_pop(frame);
                if (n == 0u) { break; }
                inject_frame(dev, frame, n);
            }
            nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
            nsfdev_kick_output();
        }
        wire_reset();
    }
    CHECK_EQ((long)tcb_inuse(), 0L, "reclaim: TCB pool baseline");
    CHECK_EQ((long)sock_inuse(), 0L, "reclaim: SOCKET pool baseline");
}

/* ============================================================================
 * main.
 * ========================================================================== */
int main(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("=== nsf370 NSFTCP loss-injection harness (M4-6) ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init");
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve");
    CHECK(nsftcp_reserve(TSTLOSS_POOL) == 0, "nsftcp_reserve");
    mm_init_complete();

    dev_init();
    nsfip_init();
    nsficmp_init();
    soc_init();
    nsfreq_init();
    nsftcp_init();
    CHECK_EQ((long)nsfreq_register_proto(6u, nsftcp_protops()), 0L, "register TCP proto");

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "WIRE", 4);
    cfg.cuu = 0x0500; cfg.type = NSFDEV_T_HOST; cfg.ipaddr = HOME_IP; cfg.mtu = 1500;
    dev = dev_register(&cfg, &g_wire_ops);
    CHECK(dev != NULL, "dev_register (lossy wire)");
    CHECK(dev_start(dev) == 0, "dev_start (lossy wire)");
    nsfip_local_add(HOME_IP);
    CHECK(nsfip_route_add(0u, 0u, dev, 0u) == 0, "default route -> wire");

    app_init();

    /* Scenario 1 -- 5 % drop, 1 MB (the gate number): completes, byte-exact,
     * rexmit > 0. */
    {
        FAULTCFG c = { 5u, 0u, 0u, 0u, 0u, 0u, 0u };
        run_transfer(dev, &c, 0xC0FFEE01u, XFER_MAX, "drop5");
    }

    /* Scenario 2 -- a mid-transfer drop BURST: 3 consecutive segments dropped once,
     * halfway through a 256 KB transfer. The go-back-N single-segment retransmit
     * grinds it out. */
    {
        FAULTCFG c = { 0u, 0u, 0u, 128u * 1024u, 3u, 0u, 0u };
        run_transfer(dev, &c, 0xB0F5E701u, 256u * 1024u, "burst3");
    }

    /* Scenario 3 -- 5 % duplication, 1 MB: completes; duplicates draw dupacks /
     * re-ACKs, no corruption (the head-trim path). */
    {
        FAULTCFG c = { 0u, 5u, 0u, 0u, 0u, 0u, 0u };
        run_transfer(dev, &c, 0xD00D1E01u, XFER_MAX, "dup5");
    }

    /* Scenario 4 -- 5 % reorder (hold-one), 1 MB: completes; without an oooq (M5)
     * reordered segments cost oooseg drops + retransmits -- the documented v1 cost.
     * We assert completion + counters, not speed. */
    {
        FAULTCFG c = { 0u, 0u, 5u, 0u, 0u, 0u, 0u };
        run_transfer(dev, &c, 0x5EED0401u, XFER_MAX, "reorder5");
    }

    /* Scenario 5 -- COMBINED 5 % drop + 2 % dup + 2 % reorder, 1 MB (the gate
     * scenario): 3 distinct seeds. */
    {
        FAULTCFG c = { 5u, 2u, 2u, 0u, 0u, 0u, 0u };
        run_transfer(dev, &c, 0x11111111u, XFER_MAX, "combined");
        run_transfer(dev, &c, 0x22222222u, XFER_MAX, "combined");
        run_transfer(dev, &c, 0x33333333u, XFER_MAX, "combined");
    }

    /* Scenario 5b -- the combined scenario x20 with rotating seeds (breadth), a
     * smaller 128 KB transfer to keep the whole run fast + deterministic. */
    {
        FAULTCFG c = { 5u, 2u, 2u, 0u, 0u, 0u, 0u };
        UINT     k;
        for (k = 0u; k < 20u; k++) {
            run_transfer(dev, &c, 0xA5000000u + k * 0x01010101u, 128u * 1024u,
                         "combined20");
        }
    }

    /* Scenario 6 -- SYN + FIN loss under the harness: drop the first 2 SYN-bearing
     * and first 2 FIN-bearing frames (deterministic, < MAXTRIES) plus 3 % random
     * drop, on a small transfer -- handshake and teardown complete through the
     * M4-4 retransmit paths end-to-end. */
    {
        FAULTCFG c = { 3u, 0u, 0u, 0u, 0u, 2u, 2u };
        run_transfer(dev, &c, 0x59F19601u, 4096u, "synfin");
    }

    /* Part C -- TIME_WAIT reclaim under pool pressure (active-close cycles). */
    run_reclaim_test(dev);

    app_term();

    mm_shutdown();
    return mbt_test_summary("TSTLOSS");
}
