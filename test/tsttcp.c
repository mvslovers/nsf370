/*
 * tsttcp.c -- NSFTCP host unit tests (spec ch. 13, M4-1).
 *
 * M4-1 is STRUCTURE, not behavior: after this milestone every crafted segment
 * gets the RFC 793 REACTION SHAPE (RST, drop, count) but no connection can be
 * established. These tests pin exactly that, tstip-style -- literal crafted
 * segments driven through the real IP path over a CAPTURE device (a DEVOPS whose
 * send copies the outbound packet to a global and frees it), the loop NOT
 * running, so every byte and counter is asserted deterministically with no
 * threads (the task's M4-1 contract).
 *
 * Pinned FIRST (before anything uses them, spec discipline):
 *   - the sequence-arithmetic macros across the 2^32 wrap;
 *   - the TCP checksum against an INDEPENDENTLY hand-computed vector (0x22F4),
 *     two-sided (a segment carrying it verifies to 0; the zeroed-field segment
 *     computes to it) -- the UDP 0x9371 idiom.
 * Then the RFC 793 §3.4 RST generation, byte-exact:
 *   - a bare SYN to a closed port -> RST,ACK with ack = seg.seq + 1 (SEG.LEN
 *     counts the SYN -- the one value everyone gets subtly wrong);
 *   - an ACK to a closed port -> RST with seq = seg.ack (ACK flag off);
 *   - a SYN+ACK -> the ACK branch dominates (seq = seg.ack, RST only);
 *   - a RST to a closed port -> silence (never a RST in response to a RST),
 *     counted resetrcvd;
 *   - a bad checksum / a bad data-offset -> drop + count, no reply;
 *   - the socket/TCB lifecycle: attach allocates a TCB, close frees it, the pool
 *     exhausts cleanly (EMFILE-class, s->pcb left NULL so teardown no-ops), and
 *     attach+destroy N times returns the pool to baseline (the leak gate).
 *
 * Counters are read back through the real registry (sts_value), exercising
 * registration too.
 */
#include "nsftcp.h"
#include "nsfip.h"
#include "nsficmp.h"
#include "nsfcksum.h"
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

#define HOME_IP   0x0A010102u       /* 10.1.1.2 -- this stack               */
#define PEER_IP   0x0A010101u       /* 10.1.1.1 -- the point-to-point peer   */

/* The TCPTCB pool reserved for the socket-lifecycle tests (small so exhaustion
 * is quick; the RST/segment tests use no TCB at all). */
#define TSTTCP_POOL  8

/* ---- capture device ---------------------------------------------------------- */
/* g_cap holds the LAST captured segment (the M4-1/M4-2 single-reply tests read it
 * through cap_tcp). M4-3 sends produce several segments per dispatch, so cap_send
 * also records each into g_capsegs[] for per-segment assertions (cap_seg). */
#define CAP_MAX 32
static UCHAR  g_cap[2048];
static USHORT g_caplen;
static int    g_capcount;
static UCHAR  g_capsegs[CAP_MAX][2048];
static USHORT g_capseglen[CAP_MAX];

static int cap_init(NETDEV *d, const DEVCFG *c)     { (void)d; (void)c; return 0; }
static int cap_start(NETDEV *d)                     { (void)d; return 0; }
static int cap_shutdown(NETDEV *d)                  { (void)d; return 0; }
static int cap_send(NETDEV *d, PBUF *b)
{
    (void)d;
    g_caplen = buf_copyout(b, g_cap, (USHORT)sizeof(g_cap));
    if (g_capcount >= 0 && g_capcount < CAP_MAX) {
        memcpy(g_capsegs[g_capcount], g_cap, g_caplen);
        g_capseglen[g_capcount] = g_caplen;
    }
    g_capcount++;
    buf_free(b);                        /* capture owns + frees the sent PBUF */
    return 0;
}
static DEVOPS cap_ops = { cap_init, cap_start, cap_send, cap_shutdown };

/* ---- byte-wise helpers ------------------------------------------------------- */
static void put16(UCHAR *p, USHORT v) { p[0] = (UCHAR)(v >> 8); p[1] = (UCHAR)v; }
static void put32(UCHAR *p, UINT v)
{
    p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);  p[3] = (UCHAR)v;
}
static USHORT get16(const UCHAR *p) { return (USHORT)(((USHORT)p[0] << 8) | p[1]); }
static UINT   get32(const UCHAR *p)
{
    return ((UINT)p[0] << 24) | ((UINT)p[1] << 16) | ((UINT)p[2] << 8) | p[3];
}

/* Checksum of a raw byte range (wrap it in a one-segment PBUF). */
static USHORT raw_cksum(const UCHAR *p, USHORT len)
{
    PBUF tp;
    memset(&tp, 0, sizeof(tp));
    tp.data = (UCHAR *)p;
    tp.len  = len;
    return in_cksum(&tp, 0u, len);
}

/* The IPv4 TCP pseudo-header partial sum (src, dst, zero, proto=6, tcplen),
 * computed with the SHARED in_cksum primitive over a 12-byte scratch -- exactly
 * as nsftcp.c seeds the segment sum. The EXPECTED checksum constant the tests
 * assert against is hand-computed independently (see test_checksum_vector), so
 * this is not circular. */
static UINT tcp_seed(UINT src, UINT dst, USHORT tcplen)
{
    UCHAR psh[12];
    PBUF  p;
    put32(psh + 0, src);
    put32(psh + 4, dst);
    psh[8] = 0u; psh[9] = (UCHAR)NSFIP_PROTO_TCP; put16(psh + 10, tcplen);
    memset(&p, 0, sizeof(p));
    p.data = psh; p.len = 12u;
    return in_cksum_partial(&p, 0u, 12u, 0u);
}

/* Build a complete IPv4 + TCP segment (20-byte IP header, 20-byte TCP header, no
 * options) with a valid IP header checksum AND a valid TCP checksum, so it
 * survives nsftcp_input's mandatory verify and reaches the demux. Returns the
 * total IP length. */
static USHORT build_tcp(UCHAR *buf, UINT src, UINT dst, USHORT sport, USHORT dport,
                        UINT seq, UINT ack, UCHAR flags, USHORT window,
                        const UCHAR *pay, USHORT paylen)
{
    UCHAR *ip = buf;
    UCHAR *t  = buf + 20;
    USHORT tcplen = (USHORT)(20u + paylen);
    USHORT total  = (USHORT)(20u + tcplen);
    UINT   seed;
    PBUF   tp;

    /* IP header (v4, IHL 5). */
    ip[0] = 0x45u; ip[1] = 0u; put16(ip + 2, total);
    put16(ip + 4, 0x4321u); put16(ip + 6, 0u);          /* id, no fragment    */
    ip[8] = 64u; ip[9] = (UCHAR)NSFIP_PROTO_TCP;
    ip[10] = 0u; ip[11] = 0u; put32(ip + 12, src); put32(ip + 16, dst);
    put16(ip + 10, raw_cksum(ip, 20u));

    /* TCP header. */
    put16(t + 0, sport); put16(t + 2, dport);
    put32(t + 4, seq);   put32(t + 8, ack);
    t[12] = (UCHAR)((20u / 4u) << 4);                    /* data offset 5      */
    t[13] = flags;
    put16(t + 14, window); put16(t + 16, 0u); put16(t + 18, 0u);
    if (paylen != 0u) {
        memcpy(t + 20, pay, paylen);
    }
    seed = tcp_seed(src, dst, tcplen);
    memset(&tp, 0, sizeof(tp)); tp.data = t; tp.len = tcplen;
    put16(t + 16, in_cksum_fold(in_cksum_partial(&tp, 0u, tcplen, seed)));
    return total;
}

/* Wrap raw packet bytes into a fresh inbound PBUF (received at start, no
 * headroom -- exactly what a driver delivers via buf_reset_rx). */
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

static UINT small_inuse(void)
{
#if NSF_DEBUG
    MMSTATS s;
    mm_stats(buf_debug_pool(NSFBUF_CLASS_SMALL), &s);
    return s.inuse;
#else
    return 0u;
#endif
}
static UINT tcb_inuse(void)
{
#if NSF_DEBUG
    return nsftcp_debug_inuse();
#else
    return 0u;
#endif
}
static UINT sock_inuse(void)
{
#if NSF_DEBUG
    return soc_debug_inuse();
#else
    return 0u;
#endif
}

/* Inject a segment inbound through the real IP path (dst must be local) and
 * drain any reply the RST path emitted into the capture. */
static void inject(NETDEV *dev, const UCHAR *pkt, USHORT total)
{
    PBUF *b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "inject: rx_pbuf allocated");
    nsfip_input(dev, b);                /* validates, demuxes proto 6 -> TCP  */
    nsfdev_kick_output();               /* flush any RST into the capture      */
}

/* ==========================================================================
 * M4-2 helpers: connection setup + segment injection with sequence tracking.
 * The loop is still NOT running; each request is dispatched directly and each
 * segment injected through the real IP path over the capture device, so every
 * transition and byte is asserted deterministically with no threads.
 * ========================================================================== */

/* Forward decls: the request-building helpers below live with the M4-1 socket
 * lifecycle tests (section 4), after this block. */
static void rqe_init(NSFRQE *r, UINT fn, UINT desc);
static UINT tcp_socket(void);

/* Build IPv4+TCP carrying `optlen` option bytes (a multiple of 4, so the data
 * offset lands on a whole word). Valid IP + TCP checksums. */
static USHORT build_tcp_opt(UCHAR *buf, UINT src, UINT dst, USHORT sport, USHORT dport,
                            UINT seq, UINT ack, UCHAR flags, USHORT window,
                            const UCHAR *opt, USHORT optlen)
{
    UCHAR *ip = buf;
    UCHAR *t  = buf + 20;
    USHORT tcplen = (USHORT)(20u + optlen);
    USHORT total  = (USHORT)(20u + tcplen);
    UINT   seed;
    PBUF   tp;

    ip[0] = 0x45u; ip[1] = 0u; put16(ip + 2, total);
    put16(ip + 4, 0x4321u); put16(ip + 6, 0u);
    ip[8] = 64u; ip[9] = (UCHAR)NSFIP_PROTO_TCP;
    ip[10] = 0u; ip[11] = 0u; put32(ip + 12, src); put32(ip + 16, dst);
    put16(ip + 10, raw_cksum(ip, 20u));

    put16(t + 0, sport); put16(t + 2, dport);
    put32(t + 4, seq);   put32(t + 8, ack);
    t[12] = (UCHAR)(((20u + optlen) / 4u) << 4);        /* data offset in words */
    t[13] = flags;
    put16(t + 14, window); put16(t + 16, 0u); put16(t + 18, 0u);
    if (optlen != 0u) {
        memcpy(t + 20, opt, optlen);
    }
    seed = tcp_seed(src, dst, tcplen);
    memset(&tp, 0, sizeof(tp)); tp.data = t; tp.len = tcplen;
    put16(t + 16, in_cksum_fold(in_cksum_partial(&tp, 0u, tcplen, seed)));
    return total;
}

/* Decode the most-recently captured outbound TCP segment (a 20-byte IP header,
 * IHL 5, as nsfip_output emits). NULL out-params are skipped. */
static void cap_tcp(UINT *seq, UINT *ack, UCHAR *flags, USHORT *sport,
                    USHORT *dport, UCHAR *dataoff)
{
    const UCHAR *t = g_cap + 20;

    if (sport   != NULL) { *sport   = get16(t + 0); }
    if (dport   != NULL) { *dport   = get16(t + 2); }
    if (seq     != NULL) { *seq     = get32(t + 4); }
    if (ack     != NULL) { *ack     = get32(t + 8); }
    if (dataoff != NULL) { *dataoff = (UCHAR)((t[12] >> 4) & 0x0Fu); }
    if (flags   != NULL) { *flags   = t[13]; }
}

/* TCP state behind a descriptor, or -1 if the socket / TCB is gone. */
static int tcb_state(UINT desc)
{
    SOCKCB *s = sock_lookup(desc);

    if (s == NULL || s->pcb == NULL) {
        return -1;
    }
    return (int)((TCB *)s->pcb)->state;
}

/* Dispatch a request and flush any outbound segment it produced into the capture.*/
static void dispatch(NSFRQE *r)
{
    nsfreq_dispatch(r);
    nsfdev_kick_output();
}

/* A connection's sequence bookkeeping (test side). cli_next = the peer's next
 * send sequence; srv_next = our next send sequence (== the peer's rcv_nxt). */
typedef struct {
    UINT   desc;
    UINT   ldesc;               /* the listener descriptor (passive), else 0    */
    USHORT lport, cport;
    UINT   cli_next, srv_next;
} CONN;

/* Establish an ACTIVE connection (connect -> SYN -> SYN|ACK -> ACK), fill *c. */
static void establish_active(NETDEV *dev, USHORT peerport, CONN *c)
{
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total, sport;
    UINT   synseq, aseq, aack;
    UCHAR  flags, aflags;
    UINT   peerISS = 0x50000000u;

    memset(c, 0, sizeof(*c));
    c->desc = tcp_socket();
    CHECK(c->desc != 0u, "active: socket");

    rqe_init(&r, RQ_CONNECT, c->desc);
    r.p1 = PEER_IP; r.p2 = (UINT)peerport;
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)g_capcount, 1L, "active: connect sent one SYN");
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);
    CHECK_EQ((long)(flags & (TCP_FL_SYN | TCP_FL_ACK)), (long)TCP_FL_SYN, "active: SYN only");
    CHECK_EQ((long)tcb_state(c->desc), (long)TCP_SYN_SENT, "active: SYN_SENT");

    c->lport    = sport;                        /* our ephemeral source port      */
    c->cport    = peerport;
    c->srv_next = synseq + 1u;                  /* our snd_nxt after the SYN       */

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, peerport, sport,
                      peerISS, synseq + 1u, (UCHAR)(TCP_FL_SYN | TCP_FL_ACK),
                      0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "active: SYN|ACK -> one ACK");
    cap_tcp(&aseq, &aack, &aflags, NULL, NULL, NULL);
    CHECK_EQ((long)aflags, (long)TCP_FL_ACK, "active: final segment is a pure ACK");
    CHECK_EQ((long)aseq, (long)(synseq + 1u), "active: ACK seq = our snd_nxt");
    CHECK_EQ((long)aack, (long)(peerISS + 1u), "active: ACK ack = peerISS + 1");
    CHECK(r.retcode == NSF_RETOK, "active: connect completed RETOK");
    CHECK_EQ((long)tcb_state(c->desc), (long)TCP_ESTABLISHED, "active: ESTABLISHED");

    c->cli_next = peerISS + 1u;                 /* the peer's next send sequence   */
}

/* Establish a PASSIVE connection (listen -> SYN -> SYN|ACK -> ACK -> accept),
 * fill *c (including the listener descriptor for cleanup). */
static USHORT g_listen_port = 7000u;

static void establish_passive(NETDEV *dev, CONN *c)
{
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total, lport = g_listen_port++;
    USHORT cport = 41000u;
    UINT   cliISS = 0x60000000u, srvISS, sack;
    UCHAR  flags, doff;

    memset(c, 0, sizeof(*c));
    c->ldesc = tcp_socket();
    CHECK(c->ldesc != 0u, "passive: listen socket");
    rqe_init(&r, RQ_BIND, c->ldesc); r.p1 = HOME_IP; r.p2 = (UINT)lport; dispatch(&r);
    CHECK(r.retcode == NSF_RETOK, "passive: bind");
    rqe_init(&r, RQ_LISTEN, c->ldesc); r.p1 = 5u; dispatch(&r);
    CHECK(r.retcode == NSF_RETOK, "passive: listen");
    CHECK_EQ((long)tcb_state(c->ldesc), (long)TCP_LISTEN, "passive: LISTEN");

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, cport, lport, cliISS, 0u,
                      (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "passive: SYN -> one SYN|ACK");
    cap_tcp(&srvISS, &sack, &flags, NULL, NULL, &doff);
    CHECK_EQ((long)flags, (long)(TCP_FL_SYN | TCP_FL_ACK), "passive: SYN|ACK flags");
    CHECK_EQ((long)sack, (long)(cliISS + 1u), "passive: SYN|ACK ack = clientISS + 1");
    CHECK_EQ((long)doff, 6L, "passive: SYN|ACK carries an option (data offset 6)");
    CHECK_EQ((long)g_cap[40], 2L, "passive: option kind = MSS");
    CHECK_EQ((long)g_cap[41], 4L, "passive: option length = 4");

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, cport, lport, cliISS + 1u, srvISS + 1u,
                      (UCHAR)TCP_FL_ACK, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 0L, "passive: final ACK draws no reply");

    rqe_init(&r, RQ_ACCEPT, c->ldesc);
    dispatch(&r);
    CHECK(r.retcode > 0, "passive: accept returns a child descriptor");
    CHECK(r.retcode != (INT)c->ldesc, "passive: child descriptor != listener");
    c->desc     = (UINT)r.retcode;
    c->lport    = lport;
    c->cport    = cport;
    c->cli_next = cliISS + 1u;
    c->srv_next = srvISS + 1u;
    CHECK_EQ((long)tcb_state(c->desc), (long)TCP_ESTABLISHED, "passive: child ESTABLISHED");
}

/* Close a descriptor via the request path. */
static void conn_close(UINT desc)
{
    NSFRQE r;
    rqe_init(&r, RQ_CLOSE, desc);
    dispatch(&r);
}

/* ==========================================================================
 * 1. Sequence arithmetic -- pinned FIRST, across the 2^32 wrap.
 * ========================================================================== */
static void test_seq_macros(void)
{
    /* Ordinary, no wrap. */
    CHECK(TCP_SEQ_LT(100u, 200u),  "seq: 100 < 200");
    CHECK(TCP_SEQ_GT(200u, 100u),  "seq: 200 > 100");
    CHECK(!TCP_SEQ_LT(5u, 5u),     "seq: 5 not < 5");
    CHECK(TCP_SEQ_LEQ(5u, 5u),     "seq: 5 <= 5");
    CHECK(TCP_SEQ_GEQ(5u, 5u),     "seq: 5 >= 5");
    CHECK(!TCP_SEQ_GT(5u, 5u),     "seq: 5 not > 5");

    /* Across the signed midpoint: 0x7FFFFFF0 precedes 0x80000010. */
    CHECK(TCP_SEQ_LT(0x7FFFFFF0u, 0x80000010u), "seq: 0x7FFFFFF0 < 0x80000010 (midpoint)");
    CHECK(TCP_SEQ_GT(0x80000010u, 0x7FFFFFF0u), "seq: 0x80000010 > 0x7FFFFFF0 (midpoint)");

    /* Across the 0 wrap: 0xFFFFFFF0 precedes 0x00000010. */
    CHECK(TCP_SEQ_LT(0xFFFFFFF0u, 0x00000010u), "seq: 0xFFFFFFF0 < 0x10 (wrap past 0)");
    CHECK(TCP_SEQ_GT(0x00000010u, 0xFFFFFFF0u), "seq: 0x10 > 0xFFFFFFF0 (wrap past 0)");
    CHECK(TCP_SEQ_LEQ(0xFFFFFFF0u, 0x00000010u), "seq: 0xFFFFFFF0 <= 0x10 (wrap)");

    /* Half-window boundaries. */
    CHECK(TCP_SEQ_LT(0u, 0x7FFFFFFFu), "seq: 0 < 2^31-1 (just within the window)");
    CHECK(TCP_SEQ_GT(0u, 0x80000001u), "seq: 0 > 2^31+1 (nearer going backward)");
}

/* ==========================================================================
 * 2. Checksum -- an INDEPENDENTLY hand-computed vector (two-sided pin).
 *
 * pseudo-header src 10.1.1.1 dst 10.1.1.2 proto 6 tcplen 20; TCP sport 0x1234
 * dport 0x0050 seq 0x11223344 ack 0 dataoff 5 flags SYN win 0x2000 -> the
 * one's-complement checksum is 0x22F4 (verified by hand and by an independent
 * summation, NOT by the routine under test).
 * ========================================================================== */
static void test_checksum_vector(void)
{
    UCHAR seg[20];
    PBUF  tp;
    UINT  seed;
    USHORT computed;

    put16(seg + 0, 0x1234u);   put16(seg + 2, 0x0050u);
    put32(seg + 4, 0x11223344u); put32(seg + 8, 0x00000000u);
    seg[12] = (UCHAR)((20u / 4u) << 4);  seg[13] = (UCHAR)TCP_FL_SYN;
    put16(seg + 14, 0x2000u);  put16(seg + 16, 0u);  put16(seg + 18, 0u);

    seed = tcp_seed(PEER_IP, HOME_IP, 20u);
    memset(&tp, 0, sizeof(tp)); tp.data = seg; tp.len = 20u;

    /* (b) computing over the zeroed-checksum-field segment yields the vector. */
    computed = in_cksum_fold(in_cksum_partial(&tp, 0u, 20u, seed));
    CHECK_EQ((long)computed, 0x22F4L,
             "TCP checksum over the hand-computed vector == 0x22F4");

    /* (a) a segment carrying that checksum verifies to 0. */
    put16(seg + 16, 0x22F4u);
    CHECK_EQ((long)in_cksum_fold(in_cksum_partial(&tp, 0u, 20u, seed)), 0L,
             "a segment carrying 0x22F4 verifies to 0");
}

/* ==========================================================================
 * 3. RST generation (RFC 793 §3.4) -- byte-exact against the capture.
 * ========================================================================== */

/* Assert the captured packet is an IP+TCP RST from HOME to PEER carrying a valid
 * checksum, and hand back its TCP seq/ack/flags/ports for the per-case asserts. */
static void rst_fields(UINT *seq, UINT *ack, UCHAR *flags, USHORT *sport, USHORT *dport)
{
    PBUF   tp;
    UINT   seed;

    CHECK_EQ((long)g_caplen, 40L, "RST: header-only (IP20 + TCP20, no payload)");
    CHECK_EQ((long)g_cap[0], 0x45L, "RST: emitted IP version 4 IHL 5");
    CHECK_EQ((long)g_cap[9], (long)NSFIP_PROTO_TCP, "RST: emitted IP protocol TCP (6)");
    CHECK_EQ((long)raw_cksum(g_cap, 20u), 0L, "RST: emitted IP header verifies to 0");
    CHECK_EQ((long)get32(g_cap + 12), (long)HOME_IP, "RST: source is our address");
    CHECK_EQ((long)get32(g_cap + 16), (long)PEER_IP, "RST: dest is the offender");

    seed = tcp_seed(HOME_IP, PEER_IP, 20u);
    memset(&tp, 0, sizeof(tp)); tp.data = g_cap + 20; tp.len = 20u;
    CHECK_EQ((long)in_cksum_fold(in_cksum_partial(&tp, 0u, 20u, seed)), 0L,
             "RST: emitted TCP checksum verifies to 0");

    if (sport != NULL) { *sport = get16(g_cap + 20); }
    if (dport != NULL) { *dport = get16(g_cap + 22); }
    if (seq   != NULL) { *seq   = get32(g_cap + 24); }
    if (ack   != NULL) { *ack   = get32(g_cap + 28); }
    if (flags != NULL) { *flags = (UCHAR)(g_cap[33]); }
}

/* A bare SYN to a closed port -> <SEQ=0><ACK=SEG.SEQ+1><CTL=RST,ACK>. The +1 is
 * the SYN counting as one octet of SEG.LEN (RFC 793 §3.3) -- the crux. */
static void test_rst_syn(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   seq, ack, sent0;
    UCHAR  flags;
    USHORT sport, dport;

    sent0 = ctr_get("NSFTCP", "resetsent");
    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40000u, 80u,
                      0x11223344u, 0u, (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);

    CHECK_EQ((long)g_capcount, 1L, "SYN to closed port -> exactly one RST sent");
    rst_fields(&seq, &ack, &flags, &sport, &dport);
    CHECK_EQ((long)sport, 80L,    "RST source port = offending dest port");
    CHECK_EQ((long)dport, 40000L, "RST dest port = offending source port");
    CHECK_EQ((long)flags, (long)(TCP_FL_RST | TCP_FL_ACK), "SYN -> RST|ACK");
    CHECK_EQ((long)seq, 0L, "SYN -> RST seq = 0 (ACK bit was off)");
    CHECK_EQ((long)ack, (long)0x11223345u, "SYN -> RST ack = seg.seq + 1 (SEG.LEN counts SYN)");
    CHECK_EQ((long)ctr_get("NSFTCP", "resetsent"), (long)sent0 + 1, "resetsent counted");
    CHECK_EQ((long)small_inuse(), 0L, "SYN/RST path leaked no buffers");
}

/* An ACK to a closed port -> <SEQ=SEG.ACK><CTL=RST> (ACK flag OFF, ack field 0). */
static void test_rst_ack(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   seq, ack;
    UCHAR  flags;

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40001u, 81u,
                      0x22334455u, 0xAABBCCDDu, (UCHAR)TCP_FL_ACK, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);

    CHECK_EQ((long)g_capcount, 1L, "ACK to closed port -> one RST sent");
    rst_fields(&seq, &ack, &flags, NULL, NULL);
    CHECK_EQ((long)flags, (long)TCP_FL_RST, "ACK -> RST only (no ACK flag)");
    CHECK_EQ((long)seq, (long)0xAABBCCDDu, "ACK -> RST seq = seg.ack");
    CHECK_EQ((long)ack, 0L, "ACK -> RST ack field 0");
    CHECK_EQ((long)small_inuse(), 0L, "ACK/RST path leaked no buffers");
}

/* A SYN+ACK -> the ACK branch dominates: <SEQ=SEG.ACK><CTL=RST>, no ACK flag. */
static void test_rst_synack(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   seq, ack;
    UCHAR  flags;

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40002u, 82u,
                      0x01020304u, 0x0A0B0C0Du,
                      (UCHAR)(TCP_FL_SYN | TCP_FL_ACK), 0x2000u, NULL, 0u);
    inject(dev, pkt, total);

    CHECK_EQ((long)g_capcount, 1L, "SYN+ACK to closed port -> one RST sent");
    rst_fields(&seq, &ack, &flags, NULL, NULL);
    CHECK_EQ((long)flags, (long)TCP_FL_RST, "SYN+ACK -> RST only (ACK bit dominates)");
    CHECK_EQ((long)seq, (long)0x0A0B0C0Du, "SYN+ACK -> RST seq = seg.ack");
    CHECK_EQ((long)ack, 0L, "SYN+ACK -> RST ack field 0");
    CHECK_EQ((long)small_inuse(), 0L, "SYN+ACK/RST path leaked no buffers");
}

/* A RST to a closed port -> silence (never a RST in response to a RST), counted
 * resetrcvd. Both a bare RST and a RST+ACK must stay silent. */
static void test_rst_on_rst_silent(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   rcvd0;

    rcvd0 = ctr_get("NSFTCP", "resetrcvd");

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40003u, 83u,
                      0x33445566u, 0u, (UCHAR)TCP_FL_RST, 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 0L, "RST to closed port -> no reply (no RST-on-RST)");

    total = build_tcp(pkt, PEER_IP, HOME_IP, 40004u, 84u,
                      0x44556677u, 0x11111111u,
                      (UCHAR)(TCP_FL_RST | TCP_FL_ACK), 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 0L, "RST+ACK to closed port -> still no reply");

    CHECK_EQ((long)ctr_get("NSFTCP", "resetrcvd"), (long)rcvd0 + 2, "both RSTs counted resetrcvd");
    CHECK_EQ((long)small_inuse(), 0L, "RST-silence path leaked no buffers");
}

/* A segment from a non-unicast source (zero / broadcast / multicast) to a closed
 * port -> NO RST (RFC 1122 §3.2.1.3: never reflect toward a non-unicast source).
 * nsfip_input accepts it (it validates only the local destination), so it
 * reaches TCP; TCP must suppress the reset. inaddrerr must NOT move (proving IP
 * delivered it, so the suppression is TCP's, not a stray IP drop). */
static void test_rst_nonunicast_src(NETDEV *dev)
{
    static const UINT bad_src[3] = { 0x00000000u, 0xFFFFFFFFu, 0xE0000001u };
    UCHAR  pkt[64];
    USHORT total;
    UINT   sent0, iae0, i;

    sent0 = ctr_get("NSFTCP", "resetsent");
    iae0  = ctr_get("NSFIP", "inaddrerr");
    for (i = 0u; i < 3u; i++) {
        g_capcount = 0;
        total = build_tcp(pkt, bad_src[i], HOME_IP, 40010u, 88u,
                          0x10203040u, 0u, (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
        inject(dev, pkt, total);
        CHECK_EQ((long)g_capcount, 0L, "non-unicast source -> no RST reflected");
    }
    CHECK_EQ((long)ctr_get("NSFTCP", "resetsent"), (long)sent0, "no RST sent for a non-unicast source");
    CHECK_EQ((long)ctr_get("NSFIP", "inaddrerr"), (long)iae0,
             "IP delivered the segments (dest local) -- the suppression is TCP's");
    CHECK_EQ((long)small_inuse(), 0L, "non-unicast path leaked no buffers");
}

/* A bad TCP checksum -> drop + count badcksum, no reply (mandatory verify). */
static void test_badcksum_drop(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   bad0;

    bad0 = ctr_get("NSFTCP", "badcksum");
    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40005u, 85u,
                      0x55667788u, 0u, (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    pkt[20 + 17] ^= 0xFFu;              /* corrupt the low TCP checksum byte  */
    inject(dev, pkt, total);

    CHECK_EQ((long)ctr_get("NSFTCP", "badcksum"), (long)bad0 + 1, "corrupt checksum -> badcksum");
    CHECK_EQ((long)g_capcount, 0L, "bad checksum -> dropped, no RST");
    CHECK_EQ((long)small_inuse(), 0L, "bad-checksum path leaked no buffers");
}

/* A bad data offset (claims a header longer than the segment) and a runt
 * (segment shorter than a TCP header) each -> drop + count hdrerr, no reply, no
 * crash. The length/offset checks run BEFORE seq/ack are read. */
static void test_malformed_drop(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   he0;

    he0 = ctr_get("NSFTCP", "hdrerr");

    /* data offset 15 -> header claims 60 bytes but the segment is 20. */
    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40006u, 86u,
                      0x66778899u, 0u, (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    pkt[20 + 12] = (UCHAR)(0xF0u);     /* data offset nibble = 15            */
    inject(dev, pkt, total);
    CHECK_EQ((long)ctr_get("NSFTCP", "hdrerr"), (long)he0 + 1, "bad data offset -> hdrerr");
    CHECK_EQ((long)g_capcount, 0L, "bad data offset -> dropped, no RST");

    /* runt: IP total claims only 12 TCP bytes (< a 20-byte header). */
    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40007u, 87u,
                      0x778899AAu, 0u, (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    put16(pkt + 2, (USHORT)(20u + 12u));           /* IP total = 32 (tcpseg 12) */
    pkt[10] = 0u; pkt[11] = 0u; put16(pkt + 10, raw_cksum(pkt, 20u));
    {
        PBUF *b = rx_pbuf(pkt, (USHORT)(20u + 12u)); /* deliver only 32 bytes   */
        CHECK(b != NULL, "runt: rx_pbuf allocated");
        nsfip_input(dev, b);
        nsfdev_kick_output();
    }
    CHECK_EQ((long)ctr_get("NSFTCP", "hdrerr"), (long)he0 + 2, "runt segment -> hdrerr");
    CHECK_EQ((long)g_capcount, 0L, "runt -> dropped, no RST");
    CHECK_EQ((long)small_inuse(), 0L, "malformed path leaked no buffers");
}

/* ==========================================================================
 * 4. Socket / TCB lifecycle over the request path (attach/detach, pool).
 * ========================================================================== */
static UINT g_apptok;

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn = (USHORT)fn;
    r->sockdesc = desc;
}

static void app_init(void)
{
    NSFRQE r;
    rqe_init(&r, RQ_INITAPI, 0u);
    nsfreq_dispatch(&r);
    CHECK(r.retcode == NSF_RETOK, "INITAPI");
    g_apptok = r.apptok;
}

/* RQ_SOCKET(AF_INET, SOCK_STREAM, 6) -> retcode (descriptor or -1); errno out. */
static INT tcp_socket_e(int *errno_out)
{
    NSFRQE r;
    rqe_init(&r, RQ_SOCKET, 0u);
    r.apptok = g_apptok; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_STREAM; r.p3 = 6u;
    nsfreq_dispatch(&r);
    if (errno_out != NULL) { *errno_out = r.errno_; }
    return r.retcode;
}

static UINT tcp_socket(void)
{
    INT rc = tcp_socket_e(NULL);
    return (rc >= 0) ? (UINT)rc : 0u;
}

static void tcp_close(UINT desc)
{
    NSFRQE r;
    rqe_init(&r, RQ_CLOSE, desc);
    nsfreq_dispatch(&r);
}

/* attach allocates a TCB; close frees it. */
static void test_attach_lifecycle(void)
{
    UINT base = tcb_inuse();
    UINT desc = tcp_socket();

    CHECK(desc != 0u, "SOCKET(SOCK_STREAM, 6) -> descriptor");
    CHECK_EQ((long)tcb_inuse(), (long)base + 1, "attach allocated one TCB");
    tcp_close(desc);
    CHECK_EQ((long)tcb_inuse(), (long)base, "close (detach) freed the TCB");
    CHECK_EQ((long)small_inuse(), 0L, "attach/close leaked no buffers");
}

/* Attach + destroy N times returns the pool to baseline (the leak gate). */
static void test_destroy_leak_gate(void)
{
    UINT base = tcb_inuse();
    int  i;

    for (i = 0; i < 20; i++) {
        UINT desc = tcp_socket();
        CHECK(desc != 0u, "leak-gate: socket");
        tcp_close(desc);
    }
    CHECK_EQ((long)tcb_inuse(), (long)base, "20x attach+destroy -> TCB pool at baseline");
    CHECK_EQ((long)sock_inuse(), (long)0, "leak-gate: SOCKET pool at baseline");
}

/* Filling the TCPTCB pool -> the next SOCKET fails cleanly (EMFILE-class), and
 * the failed attach left s->pcb NULL so soc_create's teardown no-ops (no
 * double-free, no leak). Then closing all returns the pool to baseline. */
static void test_pool_exhaustion(void)
{
    UINT descs[TSTTCP_POOL];
    int  i, e = 0;
    INT  rc;

    CHECK_EQ((long)tcb_inuse(), 0L, "exhaustion: TCB pool starts at baseline");
    for (i = 0; i < TSTTCP_POOL; i++) {
        descs[i] = tcp_socket();
        CHECK(descs[i] != 0u, "exhaustion: socket within the pool");
    }
    CHECK_EQ((long)tcb_inuse(), (long)TSTTCP_POOL, "TCB pool full");

    rc = tcp_socket_e(&e);
    CHECK_EQ((long)rc, (long)NSF_RETERR, "one-too-many SOCKET fails");
    CHECK_EQ((long)e, (long)NSF_EMFILE, "pool exhaustion -> EMFILE (clean, no abend)");

    for (i = 0; i < TSTTCP_POOL; i++) {
        tcp_close(descs[i]);
    }
    CHECK_EQ((long)tcb_inuse(), 0L, "exhaustion: TCB pool back to baseline");
    CHECK_EQ((long)sock_inuse(), 0L, "exhaustion: SOCKET pool back to baseline");
}

/* ==========================================================================
 * 5. M4-2 handshake, teardown, TIME_WAIT.
 * ========================================================================== */

/* Inject an acceptable RST to abort a connection instantly (leak-gate helper),
 * then close the listener too for a passively-opened connection. */
static void conn_abort(NETDEV *dev, CONN *c)
{
    UCHAR  pkt[64];
    USHORT total;

    total = build_tcp(pkt, PEER_IP, HOME_IP, c->cport, c->lport,
                      c->cli_next, 0u, (UCHAR)TCP_FL_RST, 0u, NULL, 0u);
    inject(dev, pkt, total);
    if (c->ldesc != 0u) {
        conn_close(c->ldesc);
    }
}

/* Full active handshake (asserted inside establish_active), then teardown. */
static void test_active_handshake(NETDEV *dev)
{
    CONN c;

    establish_active(dev, 8000u, &c);
    conn_abort(dev, &c);
    CHECK_EQ((long)tcb_inuse(), 0L, "active handshake: torn down, pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "active handshake: no buffer leak");
}

/* Full passive handshake incl. the SYN|ACK + MSS option + accept (asserted inside
 * establish_passive), then teardown. */
static void test_passive_handshake(NETDEV *dev)
{
    CONN c;

    establish_passive(dev, &c);
    conn_abort(dev, &c);
    CHECK_EQ((long)tcb_inuse(), 0L, "passive handshake: torn down, pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "passive handshake: no buffer leak");
}

/* Active connect refused by a RST|ACK in SYN_SENT -> ECONNREFUSED. */
static void test_active_refused(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total, sport;
    UINT   synseq;
    UCHAR  flags;
    UINT   desc = tcp_socket();

    rqe_init(&r, RQ_CONNECT, desc); r.p1 = PEER_IP; r.p2 = 8010u;
    g_capcount = 0;
    dispatch(&r);
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);
    CHECK_EQ((long)tcb_state(desc), (long)TCP_SYN_SENT, "refused: SYN_SENT");

    total = build_tcp(pkt, PEER_IP, HOME_IP, 8010u, sport, 0u, synseq + 1u,
                      (UCHAR)(TCP_FL_RST | TCP_FL_ACK), 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK(r.retcode == NSF_RETERR, "refused: connect failed");
    CHECK_EQ((long)r.errno_, (long)NSF_ECONNREFUSED, "refused: errno ECONNREFUSED");
    CHECK_EQ((long)tcb_state(desc), -1L, "refused: connection destroyed");
    CHECK_EQ((long)tcb_inuse(), 0L, "refused: pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "refused: no buffer leak");
}

/* A RST in a synchronized state completes a PARKED request with ECONNRESET. The
 * parked request is a CONNECT that reached SYN_RCVD via a simultaneous open (the
 * only synchronized state that carries a parked request in M4-2). */
static void test_rst_econnreset(NETDEV *dev)
{
    NSFRQE  r;
    SOCKCB *s;
    UCHAR   pkt[64];
    USHORT  total, sport;
    UINT    synseq;
    UCHAR   flags;
    UINT    peerISS = 0x33330000u;
    UINT    desc = tcp_socket();

    rqe_init(&r, RQ_CONNECT, desc); r.p1 = PEER_IP; r.p2 = 8020u;
    g_capcount = 0;
    dispatch(&r);
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);

    /* A bare SYN (no ACK) -> simultaneous open -> SYN_RCVD, connect still parked. */
    total = build_tcp(pkt, PEER_IP, HOME_IP, 8020u, sport, peerISS, 0u,
                      (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(desc), (long)TCP_SYN_RCVD, "econnreset: SYN_RCVD (simultaneous open)");
    s = sock_lookup(desc);
    CHECK(s != NULL && s->pend_connect == &r, "econnreset: connect still parked");

    /* An acceptable RST (seq = our RCV.NXT = peerISS + 1) -> ECONNRESET, destroy. */
    total = build_tcp(pkt, PEER_IP, HOME_IP, 8020u, sport, peerISS + 1u, 0u,
                      (UCHAR)TCP_FL_RST, 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK(r.retcode == NSF_RETERR, "econnreset: connect failed");
    CHECK_EQ((long)r.errno_, (long)NSF_ECONNRESET, "econnreset: errno ECONNRESET");
    CHECK_EQ((long)tcb_state(desc), -1L, "econnreset: connection destroyed");
    CHECK_EQ((long)tcb_inuse(), 0L, "econnreset: pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "econnreset: no buffer leak");
}

/* A RST on an ESTABLISHED connection with no parked request -> destroy. */
static void test_rst_established(NETDEV *dev)
{
    CONN   c;
    UCHAR  pkt[64];
    USHORT total;
    UINT   rr0;

    establish_passive(dev, &c);
    rr0 = ctr_get("NSFTCP", "resetrcvd");
    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport, c.cli_next, 0u,
                      (UCHAR)TCP_FL_RST, 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(c.desc), -1L, "RST on ESTABLISHED -> destroyed");
    CHECK_EQ((long)ctr_get("NSFTCP", "resetrcvd"), (long)rr0 + 1, "resetrcvd counted");
    conn_close(c.ldesc);
    CHECK_EQ((long)tcb_inuse(), 0L, "RST established: pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "RST established: no buffer leak");
}

/* Active close: FIN_WAIT_1 -> FIN_WAIT_2 -> TIME_WAIT -> (2MSL) -> destroyed. */
static void test_teardown_active(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total;
    UINT   fseq;
    UCHAR  fflags;

    establish_active(dev, 8030u, &c);

    rqe_init(&r, RQ_CLOSE, c.desc);
    g_capcount = 0;
    dispatch(&r);
    CHECK(r.retcode == NSF_RETOK, "active close: returns immediately (background FIN)");
    CHECK_EQ((long)g_capcount, 1L, "active close: one FIN sent");
    cap_tcp(&fseq, NULL, &fflags, NULL, NULL, NULL);
    CHECK_EQ((long)(fflags & TCP_FL_FIN), (long)TCP_FL_FIN, "active close: FIN flag set");
    CHECK_EQ((long)fseq, (long)c.srv_next, "active close: FIN seq = snd_nxt");
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_FIN_WAIT_1, "active close: FIN_WAIT_1");

    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next + 1u, (UCHAR)TCP_FL_ACK, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_FIN_WAIT_2, "active close: ACK of FIN -> FIN_WAIT_2");

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next + 1u, (UCHAR)(TCP_FL_FIN | TCP_FL_ACK),
                      0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "active close: peer FIN -> ACK sent");
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_TIME_WAIT, "active close: peer FIN -> TIME_WAIT");

    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_state(c.desc), -1L, "active close: 2MSL -> destroyed");
    CHECK_EQ((long)tcb_inuse(), 0L, "active close: pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "active close: no buffer leak");
}

/* Passive close: peer FIN -> CLOSE_WAIT, app close -> LAST_ACK -> (ACK) destroyed. */
static void test_teardown_passive(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total;

    establish_active(dev, 8040u, &c);

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next, (UCHAR)(TCP_FL_FIN | TCP_FL_ACK),
                      0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "passive close: our ACK of the peer FIN");
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_CLOSE_WAIT, "passive close: CLOSE_WAIT");
    c.cli_next = c.cli_next + 1u;               /* the peer's FIN consumed a seq   */

    rqe_init(&r, RQ_CLOSE, c.desc);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)g_capcount, 1L, "passive close: app close sends FIN");
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_LAST_ACK, "passive close: LAST_ACK");

    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next + 1u, (UCHAR)TCP_FL_ACK, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(c.desc), -1L, "passive close: ACK of our FIN -> CLOSED, destroyed");
    CHECK_EQ((long)tcb_inuse(), 0L, "passive close: pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "passive close: no buffer leak");
}

/* Simultaneous close: app close -> FIN_WAIT_1, peer FIN (not acking ours) ->
 * CLOSING, peer ACK -> TIME_WAIT -> (2MSL) destroyed. */
static void test_teardown_simultaneous(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total;

    establish_active(dev, 8050u, &c);

    rqe_init(&r, RQ_CLOSE, c.desc);
    dispatch(&r);
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_FIN_WAIT_1, "simultaneous: FIN_WAIT_1");

    /* Peer FIN that does NOT acknowledge our FIN (ack = snd_una, before our FIN). */
    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next, (UCHAR)(TCP_FL_FIN | TCP_FL_ACK),
                      0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "simultaneous: our ACK of the peer FIN");
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_CLOSING, "simultaneous: FIN_WAIT_1 + FIN -> CLOSING");
    c.cli_next = c.cli_next + 1u;

    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next + 1u, (UCHAR)TCP_FL_ACK, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_TIME_WAIT, "simultaneous: CLOSING + ACK -> TIME_WAIT");

    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_state(c.desc), -1L, "simultaneous: 2MSL -> destroyed");
    CHECK_EQ((long)tcb_inuse(), 0L, "simultaneous: pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "simultaneous: no buffer leak");
}

/* Malformed TCP options on an inbound SYN -> drop + count hdrerr, no SYN|ACK, no
 * child, no overrun (ASan). */
static void test_malformed_options(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total;
    UINT   he0;
    UINT   ldesc = tcp_socket();
    UCHAR  badlen0[4]  = { 2u, 0u, 0u, 0u };    /* MSS kind, length 0  -> malformed */
    UCHAR  badrun[4]   = { 2u, 40u, 0u, 0u };   /* length runs past the option area */

    rqe_init(&r, RQ_BIND, ldesc); r.p1 = HOME_IP; r.p2 = 6100u; dispatch(&r);
    rqe_init(&r, RQ_LISTEN, ldesc); r.p1 = 4u; dispatch(&r);
    he0 = ctr_get("NSFTCP", "hdrerr");

    g_capcount = 0;
    total = build_tcp_opt(pkt, PEER_IP, HOME_IP, 44000u, 6100u, 0x10u, 0u,
                          (UCHAR)TCP_FL_SYN, 0x2000u, badlen0, 4u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 0L, "malformed opt (len 0): no SYN|ACK");
    CHECK_EQ((long)ctr_get("NSFTCP", "hdrerr"), (long)he0 + 1, "malformed opt (len 0): hdrerr");

    g_capcount = 0;
    total = build_tcp_opt(pkt, PEER_IP, HOME_IP, 44001u, 6100u, 0x20u, 0u,
                          (UCHAR)TCP_FL_SYN, 0x2000u, badrun, 4u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 0L, "malformed opt (overrun): no SYN|ACK");
    CHECK_EQ((long)ctr_get("NSFTCP", "hdrerr"), (long)he0 + 2, "malformed opt (overrun): hdrerr");
    CHECK_EQ((long)tcb_inuse(), 1L, "malformed opt: only the listener TCB (no child allocated)");

    conn_close(ldesc);
    CHECK_EQ((long)tcb_inuse(), 0L, "malformed opt: listener closed, pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "malformed opt: no buffer leak");
}

/* Drive a fresh active connection all the way to TIME_WAIT; returns its
 * descriptor (a TIME_WAIT TCB lives until 2MSL). */
static UINT active_to_timewait(NETDEV *dev, USHORT peerport)
{
    CONN   c;
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total;

    establish_active(dev, peerport, &c);
    rqe_init(&r, RQ_CLOSE, c.desc);
    dispatch(&r);
    /* Peer FIN + ACK-of-our-FIN in one segment: FIN_WAIT_1 -> FIN_WAIT_2 -> TIME_WAIT. */
    total = build_tcp(pkt, PEER_IP, HOME_IP, c.cport, c.lport,
                      c.cli_next, c.srv_next + 1u, (UCHAR)(TCP_FL_FIN | TCP_FL_ACK),
                      0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_TIME_WAIT, "reclaim setup: TIME_WAIT reached");
    return c.desc;
}

/* Pool exhaustion at SYN time reclaims the oldest TIME_WAIT TCB (spec 13.4). Fill
 * the pool with a listener + (POOL-1) TIME_WAIT TCBs, then a new SYN reclaims one. */
static void test_timewait_reclaim(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total;
    UINT   ldesc, recl0;
    int    i;

    CHECK_EQ((long)tcb_inuse(), 0L, "reclaim: pool starts at baseline");
    ldesc = tcp_socket();
    rqe_init(&r, RQ_BIND, ldesc); r.p1 = HOME_IP; r.p2 = 6000u; dispatch(&r);
    rqe_init(&r, RQ_LISTEN, ldesc); r.p1 = 8u; dispatch(&r);

    for (i = 0; i < TSTTCP_POOL - 1; i++) {
        (void)active_to_timewait(dev, (USHORT)(8100 + i));
    }
    CHECK_EQ((long)tcb_inuse(), (long)TSTTCP_POOL, "reclaim: pool full (listener + TIME_WAITs)");
    recl0 = ctr_get("NSFTCP", "twreclaim");      /* 12-char stats field (nsfsts.h) */

    /* A new SYN to the listener: the pool is full -> reclaim the oldest TIME_WAIT,
     * retry the allocation, and reply SYN|ACK for the new connection. */
    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 8200u, 6000u, 0x70000000u, 0u,
                      (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)ctr_get("NSFTCP", "twreclaim"), (long)recl0 + 1,
             "reclaim: one TIME_WAIT reclaimed");
    CHECK_EQ((long)g_capcount, 1L, "reclaim: SYN|ACK sent for the new connection");
    CHECK_EQ((long)tcb_inuse(), (long)TSTTCP_POOL, "reclaim: pool still full (child took the slot)");

    /* Cleanup: closing the listener destroys the embryonic child too; fire the
     * 2MSL timers to drain the remaining TIME_WAIT TCBs. */
    conn_close(ldesc);
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "reclaim: pool back to baseline after cleanup");
    CHECK_EQ((long)small_inuse(), 0L, "reclaim: no buffer leak");
}

/* M4-6: a NEW ACTIVE socket must also reclaim the oldest TIME_WAIT when the TCB
 * pool is full (tcp_attach), mirroring the passive path -- otherwise a guest doing
 * rapid active connect->close against a REMOTE peer walls at EMFILE once its own
 * accumulated TIME_WAITs fill the pool (there is no local passive open to reclaim
 * for it). This is the M4-6 fold-in fix; the host loss harness's self-talk
 * reclaim scenario cannot see it (its local child-creation reclaims via the
 * passive path), so it is pinned here with a synthetic remote peer. */
static void test_timewait_reclaim_active(NETDEV *dev)
{
    NSFRQE r;
    UINT   ldesc, adesc, recl0;
    int    i;

    CHECK_EQ((long)tcb_inuse(), 0L, "reclaim-active: pool starts at baseline");
    ldesc = tcp_socket();
    rqe_init(&r, RQ_BIND, ldesc); r.p1 = HOME_IP; r.p2 = 6100u; dispatch(&r);
    rqe_init(&r, RQ_LISTEN, ldesc); r.p1 = 8u; dispatch(&r);

    /* Fill the pool: listener + (POOL-1) active TIME_WAIT TCBs. */
    for (i = 0; i < TSTTCP_POOL - 1; i++) {
        (void)active_to_timewait(dev, (USHORT)(8300 + i));
    }
    CHECK_EQ((long)tcb_inuse(), (long)TSTTCP_POOL, "reclaim-active: pool full (listener + TIME_WAITs)");
    recl0 = ctr_get("NSFTCP", "twreclaim");

    /* A new ACTIVE socket with the pool full: tcp_attach must reclaim the oldest
     * TIME_WAIT (without the fix this returns 0 -- EMFILE). */
    adesc = tcp_socket();
    CHECK(adesc != 0u, "reclaim-active: new active socket succeeds (reclaimed a TIME_WAIT)");
    CHECK_EQ((long)ctr_get("NSFTCP", "twreclaim"), (long)recl0 + 1,
             "reclaim-active: exactly one TIME_WAIT reclaimed");

    tcp_close(adesc);                           /* the fresh (CLOSED) socket        */
    conn_close(ldesc);
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "reclaim-active: pool baseline after cleanup");
    CHECK_EQ((long)small_inuse(), 0L, "reclaim-active: no buffer leak");
}

/* ==========================================================================
 * 6. M4-3 data path: segmentation, sliding window, in-order receive, EOF,
 *    copy-on-transmit recovery (ADR-0032). The loop is still NOT running; each
 *    request is dispatched directly and each segment injected over the capture
 *    device, so every byte, counter and window transition is deterministic.
 * ========================================================================== */

#define REQ_PENDING  0x7F7F7F7F         /* retcode sentinel: a parked request    */

static UINT large_inuse(void)
{
#if NSF_DEBUG
    MMSTATS s;
    mm_stats(buf_debug_pool(NSFBUF_CLASS_LARGE), &s);
    return s.inuse;
#else
    return 0u;
#endif
}

/* TCB field peeks (TCB is a public struct, nsftcp.h). */
static TCB *tcb_of(UINT desc)
{
    SOCKCB *s = sock_lookup(desc);
    return (s != NULL) ? (TCB *)s->pcb : NULL;
}
static UINT tcb_sndq(UINT desc)   { TCB *t = tcb_of(desc); return (t != NULL) ? t->sndq_bytes : 0u; }
static UINT tcb_rcvwnd(UINT desc) { TCB *t = tcb_of(desc); return (t != NULL) ? t->rcv_wnd : 0u; }
static UINT tcb_sndnxt(UINT desc) { TCB *t = tcb_of(desc); return (t != NULL) ? t->snd_nxt : 0u; }

/* M4-4 timer peeks: is a TCB's retransmit / persist timer armed, and its current
 * RTO / backoff (all public TCB / TMR fields, nsftcp.h / nsftmr.h). */
static int  tcb_rexmit_on(UINT desc)  { TCB *t = tcb_of(desc); return (t != NULL) && t->t_rexmit.state == (UCHAR)TMR_PENDING; }
static int  tcb_persist_on(UINT desc) { TCB *t = tcb_of(desc); return (t != NULL) && t->t_persist.state == (UCHAR)TMR_PENDING; }
static UINT tcb_rto(UINT desc)        { TCB *t = tcb_of(desc); return (t != NULL) ? (UINT)t->rto : 0u; }
static UINT tcb_backoff(UINT desc)    { TCB *t = tcb_of(desc); return (t != NULL) ? (UINT)t->backoff : 0u; }

/* Drive `ticks` logical ticks, then flush any segment a fired timer emitted into
 * the capture (the timer path has no dispatch/inject wrapper of its own). */
static void tick(UINT ticks)
{
    nsftmr_run(ticks);
    nsfdev_kick_output();
}

/* The interval the retransmit/persist timer is armed for after k no-progress
 * expiries: base << min(k, 6), capped at the 64 s max. k=0 is the first arming. */
static UINT rexmit_interval(int k)
{
    UINT shift = (k > 6) ? 6u : (UINT)k;
    UINT t     = (UINT)NSFTCP_RTO_TICKS << shift;
    return (t > (UINT)NSFTCP_RTO_MAX_TICKS) ? (UINT)NSFTCP_RTO_MAX_TICKS : t;
}

/* The invariant that keeps the give-up teardown safe (ADR-0033): the retransmit
 * and persist timers are NEVER armed at the same time. */
static void check_excl(UINT desc, const char *tag)
{
    CHECK(!(tcb_rexmit_on(desc) && tcb_persist_on(desc)), tag);
}

/* Fill n bytes with a recognizable, position-dependent pattern. */
static void fill_pattern(UCHAR *buf, USHORT n, UCHAR base)
{
    USHORT i;
    for (i = 0u; i < n; i++) {
        buf[i] = (UCHAR)(base + (i & 0x3Fu));
    }
}

/* Decode captured outbound segment #i (variable IHL/data-offset). Returns the
 * payload pointer; NULL out-params are skipped. */
static const UCHAR *cap_seg(int i, UCHAR *flags, UINT *seq, UINT *ack,
                            USHORT *win, USHORT *paylen)
{
    const UCHAR *ip    = g_capsegs[i];
    UCHAR        ihl   = (UCHAR)((ip[0] & 0x0Fu) * 4u);
    const UCHAR *t     = ip + ihl;
    UCHAR        thoff = (UCHAR)(((t[12] >> 4) & 0x0Fu) * 4u);
    USHORT       total = get16(ip + 2);

    if (flags  != NULL) { *flags  = t[13]; }
    if (seq    != NULL) { *seq    = get32(t + 4); }
    if (ack    != NULL) { *ack    = get32(t + 8); }
    if (win    != NULL) { *win    = get16(t + 14); }
    if (paylen != NULL) { *paylen = (USHORT)(total - ihl - thoff); }
    return t + thoff;
}

/* Establish an ACTIVE connection advertising a peer window + MSS (via a SYN|ACK
 * MSS option), so the data tests control segmentation and flow control. */
static void establish_active_opt(NETDEV *dev, USHORT peerport, USHORT peer_win,
                                 USHORT peer_mss, CONN *c)
{
    NSFRQE r;
    UCHAR  pkt[64], opt[4];
    USHORT total, sport;
    UINT   synseq;
    UCHAR  flags;
    UINT   peerISS = 0x50000000u;

    memset(c, 0, sizeof(*c));
    c->desc = tcp_socket();
    CHECK(c->desc != 0u, "estab-opt: socket");

    rqe_init(&r, RQ_CONNECT, c->desc);
    r.p1 = PEER_IP; r.p2 = (UINT)peerport;
    g_capcount = 0;
    dispatch(&r);
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);
    c->lport    = sport;
    c->cport    = peerport;
    c->srv_next = synseq + 1u;

    opt[0] = 2u; opt[1] = 4u; put16(opt + 2, peer_mss);
    total = build_tcp_opt(pkt, PEER_IP, HOME_IP, peerport, sport, peerISS,
                          synseq + 1u, (UCHAR)(TCP_FL_SYN | TCP_FL_ACK), peer_win,
                          opt, 4u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(c->desc), (long)TCP_ESTABLISHED, "estab-opt: ESTABLISHED");
    c->cli_next = peerISS + 1u;
}

/* The peer's current send sequence == our RCV.NXT (a synced peer's bare ACK/RST
 * carries this, so it is always sequence-acceptable even after we received data
 * that advanced RCV.NXT past the handshake cli_next). */
static UINT peer_seq(CONN *c)
{
    TCB *t = tcb_of(c->desc);
    return (t != NULL) ? t->rcv_nxt : c->cli_next;
}

/* Inject a bare ACK from the peer, advancing our SND.UNA to `ack` and
 * advertising window `win`. */
static void inject_ack_win(NETDEV *dev, CONN *c, UINT ack, USHORT win)
{
    UCHAR  pkt[64];
    USHORT total = build_tcp(pkt, PEER_IP, HOME_IP, c->cport, c->lport,
                             peer_seq(c), ack, (UCHAR)TCP_FL_ACK, win, NULL, 0u);
    inject(dev, pkt, total);
}

/* Inject a data (and optionally FIN) segment from the peer at sequence `seq`. */
static void inject_seg(NETDEV *dev, CONN *c, UINT seq, const UCHAR *pay,
                       USHORT n, UCHAR extra)
{
    UCHAR  pkt[2100];
    USHORT total = build_tcp(pkt, PEER_IP, HOME_IP, c->cport, c->lport, seq,
                             c->srv_next, (UCHAR)(TCP_FL_ACK | extra), 0x2000u,
                             pay, n);
    inject(dev, pkt, total);
}

/* Build a SEND / RECV request; retcode starts at REQ_PENDING so a parked (not
 * yet completed) request is detectable after dispatch. */
static void req_send(NSFRQE *r, UINT desc, const void *buf, UINT len, USHORT flags)
{
    rqe_init(r, RQ_SEND, desc);
    r->ubuf = (void *)buf; r->ulen = len; r->flags = flags;
    r->retcode = REQ_PENDING;
}
static void req_recv(NSFRQE *r, UINT desc, void *buf, UINT len, USHORT flags)
{
    rqe_init(r, RQ_RECV, desc);
    r->ubuf = buf; r->ulen = len; r->flags = flags;
    r->retcode = REQ_PENDING;
}

/* Tear a connection down abruptly (a peer RST at RCV.NXT) so a test that leaves
 * it mid-transfer still returns the pools to baseline. */
static void conn_reset(NETDEV *dev, CONN *c)
{
    UCHAR  pkt[64];
    USHORT total = build_tcp(pkt, PEER_IP, HOME_IP, c->cport, c->lport,
                             peer_seq(c), c->srv_next, (UCHAR)TCP_FL_RST,
                             0x2000u, NULL, 0u);
    inject(dev, pkt, total);
}

/* 6a. Segmentation: 4096 bytes over MSS 1460 -> 1460 + 1460 + 1176 with correct
 *     seqs; sndq_bytes tracks; progressive ACKs free the sndq (partial head). */
static void test_data_segmentation(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[4096];
    UCHAR  fl;
    UINT   sq0, sq1, sq2;
    USHORT l0, l1, l2;

    fill_pattern(src, sizeof(src), 0x41u);
    establish_active_opt(dev, 8100u, 0x2000u, 1460u, &c);

    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 4096L, "seg: send buffered all 4096 bytes");
    CHECK_EQ((long)g_capcount, 3L, "seg: 4096 over MSS 1460 -> 3 segments");
    cap_seg(0, &fl, &sq0, NULL, NULL, &l0);
    cap_seg(1, NULL, &sq1, NULL, NULL, &l1);
    cap_seg(2, NULL, &sq2, NULL, NULL, &l2);
    CHECK_EQ((long)l0, 1460L, "seg[0] len 1460");
    CHECK_EQ((long)l1, 1460L, "seg[1] len 1460");
    CHECK_EQ((long)l2, 1176L, "seg[2] len 1176 (remainder)");
    CHECK_EQ((long)sq0, (long)c.srv_next,          "seg[0] seq = snd_nxt");
    CHECK_EQ((long)sq1, (long)(c.srv_next + 1460u), "seg[1] seq contiguous");
    CHECK_EQ((long)sq2, (long)(c.srv_next + 2920u), "seg[2] seq contiguous");
    CHECK(memcmp(cap_seg(0, NULL, NULL, NULL, NULL, NULL), src, 1460) == 0,
          "seg[0] payload byte-exact");
    CHECK_EQ((long)tcb_sndq(c.desc), 4096L, "seg: sndq holds all 4096 unacked");

    /* Partial ACK inside the first sndq PBUF -> buf_trim_head the front. */
    inject_ack_win(dev, &c, c.srv_next + 1000u, 0x2000u);
    CHECK_EQ((long)tcb_sndq(c.desc), 3096L, "seg: partial ACK freed 1000 (head adjust)");
    /* ACK the rest -> the sndq empties. */
    inject_ack_win(dev, &c, c.srv_next + 4096u, 0x2000u);
    CHECK_EQ((long)tcb_sndq(c.desc), 0L, "seg: full ACK drained the sndq");

    conn_close(c.desc);
    inject_ack_win(dev, &c, c.srv_next + 4097u, 0x2000u);   /* ACK our FIN        */
    conn_reset(dev, &c);                                    /* drop any TIME_WAIT */
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "seg: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "seg: BUFLARGE baseline");
}

/* 6b. Sliding window: peer window = 1 MSS -> one segment in flight; zero window
 *     pauses the sender; a window update resumes it. */
static void test_send_window(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[4096];
    USHORT l0;

    fill_pattern(src, sizeof(src), 0x61u);
    establish_active_opt(dev, 8101u, 1460u, 1460u, &c);     /* peer window = 1 MSS */

    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 4096L, "win: send buffered all 4096");
    CHECK_EQ((long)g_capcount, 1L, "win: 1-MSS window -> exactly one segment");
    cap_seg(0, NULL, NULL, NULL, NULL, &l0);
    CHECK_EQ((long)l0, 1460L, "win: the in-flight segment is 1 MSS");

    /* ACK it + re-advertise 1 MSS -> the next segment goes. */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next + 1460u, 1460u);
    CHECK_EQ((long)g_capcount, 1L, "win: ACK + window -> next segment");

    /* Zero window -> the sender pauses (no segment). */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next + 2920u, 0u);
    CHECK_EQ((long)g_capcount, 0L, "win: zero window pauses the sender");
    CHECK_EQ((long)tcb_sndq(c.desc), 1176L, "win: the remainder waits on the sndq");

    /* Window update (no new data acked) -> the sender resumes. */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next + 2920u, 1176u);
    CHECK_EQ((long)g_capcount, 1L, "win: window update resumes the sender");
    cap_seg(0, NULL, NULL, NULL, NULL, &l0);
    CHECK_EQ((long)l0, 1176L, "win: the resumed segment carries the remainder");

    conn_reset(dev, &c);
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "win: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "win: BUFLARGE baseline");
}

/* 6c. Blocking send larger than the budget: parks, drains as ACKs free space,
 *     completes with the full count. */
static void test_send_block(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[5000];

    fill_pattern(src, sizeof(src), 0x30u);
    establish_active_opt(dev, 8102u, 0x4000u, 1460u, &c);   /* wide peer window   */

    req_send(&r, c.desc, src, sizeof(src), 0u);             /* 5000 > SNDBUF 4096 */
    dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)REQ_PENDING, "block: 5000 > budget -> parked");
    CHECK_EQ((long)tcb_sndq(c.desc), 4096L, "block: 4096 buffered, 904 pending");

    /* ACK the first 4096 sent -> budget frees, the parked send buffers the rest
     * and completes with the full 5000. */
    inject_ack_win(dev, &c, c.srv_next + 4096u, 0x4000u);
    CHECK_EQ((long)r.retcode, 5000L, "block: parked send completes with the full count");

    /* Drain the tail + close. */
    inject_ack_win(dev, &c, c.srv_next + 5000u, 0x4000u);
    CHECK_EQ((long)tcb_sndq(c.desc), 0L, "block: sndq fully drained");
    conn_reset(dev, &c);
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "block: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "block: BUFLARGE baseline");
}

/* 6d. In-order receive to a parked RECV (byte-exact) + the ACK. */
static void test_recv_inorder(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  pay[100], out[256];
    UINT   ack;

    fill_pattern(pay, sizeof(pay), 0x50u);
    establish_active_opt(dev, 8103u, 0x2000u, 1460u, &c);

    memset(out, 0, sizeof(out));
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)REQ_PENDING, "recv: empty rxq -> parked");

    g_capcount = 0;
    inject_seg(dev, &c, c.cli_next, pay, 100u, 0u);
    CHECK_EQ((long)r.retcode, 100L, "recv: parked RECV completes with 100 bytes");
    CHECK(memcmp(out, pay, 100) == 0, "recv: delivered bytes are byte-exact");
    CHECK_EQ((long)g_capcount, 1L, "recv: one ACK for the accepted data");
    cap_tcp(NULL, &ack, NULL, NULL, NULL, NULL);
    CHECK_EQ((long)ack, (long)(c.cli_next + 100u), "recv: ACK advanced RCV.NXT by 100");

    conn_reset(dev, &c);
    CHECK_EQ((long)tcb_inuse(), 0L, "recv: TCB pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "recv: BUFSMALL baseline");
    CHECK_EQ((long)large_inuse(), 0L, "recv: BUFLARGE baseline");
}

/* 6e. Retransmission overlap (head-trim), pure duplicate (ACK-only), and a gap
 *     (drop + oooseg + dup-ACK carrying the old RCV.NXT). */
static void test_recv_overlap_dup_gap(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  a[100], b[100], out[256];
    UINT   oo0, ack;

    fill_pattern(a, sizeof(a), 0x41u);          /* first 100 bytes               */
    fill_pattern(b, sizeof(b), 0x80u);          /* the retransmission's payload  */
    establish_active_opt(dev, 8104u, 0x2000u, 1460u, &c);

    /* In order: 100 bytes at RCV.NXT -> queued (no reader). */
    inject_seg(dev, &c, c.cli_next, a, 100u, 0u);
    CHECK_EQ((long)tcb_rcvwnd(c.desc), (long)(NSFTCP_RCVWND_DEFAULT - 100),
             "overlap: window shrank by the queued 100");

    /* A retransmission overlapping below RCV.NXT (seq = cli_next+50, 100 bytes):
     * the first 50 are already received, the last 50 are new. */
    g_capcount = 0;
    inject_seg(dev, &c, c.cli_next + 50u, b, 100u, 0u);
    CHECK_EQ((long)g_capcount, 1L, "overlap: an ACK for the accepted tail");

    /* Pure duplicate (exactly the first segment again) -> ACK only, no re-queue. */
    oo0 = ctr_get("NSFTCP", "oooseg");
    g_capcount = 0;
    inject_seg(dev, &c, c.cli_next, a, 100u, 0u);
    CHECK_EQ((long)g_capcount, 1L, "dup: pure duplicate draws an ACK");
    CHECK_EQ((long)ctr_get("NSFTCP", "oooseg"), (long)oo0, "dup: not counted as oooseg");

    /* A gap above RCV.NXT (RCV.NXT is now cli_next+150): seq = cli_next+300. */
    g_capcount = 0;
    inject_seg(dev, &c, c.cli_next + 300u, b, 40u, 0u);
    CHECK_EQ((long)ctr_get("NSFTCP", "oooseg"), (long)oo0 + 1, "gap: counted oooseg");
    CHECK_EQ((long)g_capcount, 1L, "gap: a dup-ACK is emitted");
    cap_tcp(NULL, &ack, NULL, NULL, NULL, NULL);
    CHECK_EQ((long)ack, (long)(c.cli_next + 150u), "gap: dup-ACK carries the old RCV.NXT");

    /* Drain: 150 in-order bytes (a[0..100] then b's fresh tail b[50..100]). */
    memset(out, 0, sizeof(out));
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 150L, "overlap: reader drains 150 contiguous bytes");
    CHECK(memcmp(out, a, 100) == 0, "overlap: first 100 = the original segment");
    CHECK(memcmp(out + 100, b + 50, 50) == 0, "overlap: next 50 = the fresh tail");

    conn_reset(dev, &c);
    CHECK_EQ((long)tcb_inuse(), 0L, "overlap: TCB pool baseline");
    CHECK_EQ((long)small_inuse(), 0L, "overlap: BUFSMALL baseline");
    CHECK_EQ((long)large_inuse(), 0L, "overlap: BUFLARGE baseline");
}

/* 6f. rxq fills the advertised window (reaches 0), then a recv drains it and the
 *     pure window-update ACK is emitted (the deadlock rule). */
static void test_recv_window_update(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  chunk[1024], out[4096];
    USHORT win;
    int    i;

    fill_pattern(chunk, sizeof(chunk), 0x21u);
    establish_active_opt(dev, 8105u, 0x2000u, 1460u, &c);

    /* Four 1024-byte segments fill the 4096 window without a reader. */
    for (i = 0; i < 4; i++) {
        inject_seg(dev, &c, c.cli_next + (UINT)(i * 1024), chunk, 1024u, 0u);
    }
    CHECK_EQ((long)tcb_rcvwnd(c.desc), 0L, "wndupd: advertised window reached 0");

    /* A segment at the closed window is rejected (ACK, no accept). */
    g_capcount = 0;
    inject_seg(dev, &c, c.cli_next + 4096u, chunk, 100u, 0u);
    CHECK_EQ((long)tcb_rcvwnd(c.desc), 0L, "wndupd: still 0 (segment rejected)");

    /* Drain all 4096 -> window reopens from 0 -> a pure window-update ACK. */
    memset(out, 0, sizeof(out));
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 4096L, "wndupd: recv drained the full 4096");
    CHECK_EQ((long)tcb_rcvwnd(c.desc), (long)NSFTCP_RCVWND_DEFAULT, "wndupd: window fully reopened");
    CHECK_EQ((long)g_capcount, 1L, "wndupd: a pure window-update ACK is emitted");
    cap_tcp(NULL, NULL, NULL, NULL, NULL, NULL);
    win = get16(g_cap + 20 + 14);
    CHECK_EQ((long)win, (long)NSFTCP_RCVWND_DEFAULT, "wndupd: the update advertises the full window");
    CHECK(memcmp(out, chunk, 1024) == 0, "wndupd: first drained chunk is byte-exact");

    conn_reset(dev, &c);
    CHECK_EQ((long)tcb_inuse(), 0L, "wndupd: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "wndupd: BUFLARGE baseline");
}

/* 6g. EOF matrix: data+FIN in one segment (data first, then EOF sticky); a pure
 *     FIN then a recv (rc=0); a recv parked before a pure FIN (completes rc=0). */
static void test_eof_matrix(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  pay[50], out[64];

    /* --- data + FIN in one segment --- */
    fill_pattern(pay, sizeof(pay), 0x70u);
    establish_active_opt(dev, 8106u, 0x2000u, 1460u, &c);
    memset(out, 0, sizeof(out));
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    dispatch(&r);                               /* parks (empty rxq)             */
    inject_seg(dev, &c, c.cli_next, pay, 50u, (UCHAR)TCP_FL_FIN);
    CHECK_EQ((long)r.retcode, 50L, "eof: data+FIN delivers the data first");
    CHECK(memcmp(out, pay, 50) == 0, "eof: the pre-FIN data is byte-exact");
    /* the next recv sees EOF, and it is sticky */
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 0L, "eof: recv after the FIN returns 0 (EOF)");
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 0L, "eof: EOF is sticky");
    conn_close(c.desc);                         /* CLOSE_WAIT -> LAST_ACK (FIN)   */
    inject_ack_win(dev, &c, c.srv_next + 1u, 0x2000u);   /* ACK our FIN -> gone   */
    CHECK_EQ((long)tcb_inuse(), 0L, "eof: connection torn down");

    /* --- recv parked, then a pure FIN completes it rc=0 --- */
    establish_active_opt(dev, 8107u, 0x2000u, 1460u, &c);
    req_recv(&r, c.desc, out, sizeof(out), 0u);
    dispatch(&r);                               /* parks                          */
    CHECK_EQ((long)r.retcode, (long)REQ_PENDING, "eof: recv parked on empty rxq");
    inject_seg(dev, &c, c.cli_next, NULL, 0u, (UCHAR)TCP_FL_FIN);   /* pure FIN    */
    CHECK_EQ((long)r.retcode, 0L, "eof: a pure FIN completes the parked recv rc=0");
    conn_close(c.desc);
    inject_ack_win(dev, &c, c.srv_next + 1u, 0x2000u);
    CHECK_EQ((long)tcb_inuse(), 0L, "eof: second connection torn down");
    CHECK_EQ((long)large_inuse(), 0L, "eof: BUFLARGE baseline");
    CHECK_EQ((long)small_inuse(), 0L, "eof: BUFSMALL baseline");
}

/* 6h. Copy-on-transmit recovery: a transmit failure leaves SND.NXT unmoved and
 *     the data on the sndq; the next ACK event retries successfully. */
static void test_xmit_failure(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[1000];
    USHORT l0;

    fill_pattern(src, sizeof(src), 0x11u);
    establish_active_opt(dev, 8108u, 0x2000u, 1460u, &c);

    dev->state = (UCHAR)NSFDEV_S_DOWN;          /* force dev_send rejection       */
    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 1000L, "xmit: send buffered the 1000 bytes");
    CHECK_EQ((long)g_capcount, 0L, "xmit: nothing reached the wire (device down)");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)c.srv_next, "xmit: SND.NXT not advanced past the failed segment");
    CHECK_EQ((long)tcb_sndq(c.desc), 1000L, "xmit: the data survives on the sndq");

    dev->state = (UCHAR)NSFDEV_S_UP;            /* recover the link               */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next, 0x2000u);   /* a window-update event       */
    CHECK_EQ((long)g_capcount, 1L, "xmit: the next event retransmits the segment");
    cap_seg(0, NULL, NULL, NULL, NULL, &l0);
    CHECK_EQ((long)l0, 1000L, "xmit: the retried segment carries all 1000 bytes");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)(c.srv_next + 1000u), "xmit: SND.NXT advanced on the retry");

    inject_ack_win(dev, &c, c.srv_next + 1000u, 0x2000u);
    conn_reset(dev, &c);
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "xmit: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "xmit: BUFLARGE baseline");
}

/* 6i. ENOTCONN / EWOULDBLOCK error paths. */
static void test_data_errors(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  buf[128];
    UINT   desc;

    /* SEND / RECV on an unconnected (fresh) socket -> ENOTCONN. */
    desc = tcp_socket();
    req_send(&r, desc, buf, 16u, 0u);
    dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_ENOTCONN, "err: SEND on a CLOSED socket -> ENOTCONN");
    req_recv(&r, desc, buf, sizeof(buf), 0u);
    dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_ENOTCONN, "err: RECV on a CLOSED socket -> ENOTCONN");
    tcp_close(desc);

    /* Non-blocking RECV on an empty rxq -> EWOULDBLOCK. */
    establish_active_opt(dev, 8109u, 0u, 1460u, &c);        /* peer window 0       */
    req_recv(&r, c.desc, buf, sizeof(buf), (USHORT)RQ_F_NONBLOCK);
    dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EWOULDBLOCK, "err: non-blocking RECV empty -> EWOULDBLOCK");

    /* Fill the send budget (window 0 -> nothing transmits, all buffered), then a
     * non-blocking SEND with no room -> EWOULDBLOCK. */
    {
        static UCHAR big[4096];
        fill_pattern(big, sizeof(big), 0x01u);
        req_send(&r, c.desc, big, sizeof(big), 0u);
        dispatch(&r);
        CHECK_EQ((long)r.retcode, 4096L, "err: send buffered the whole budget");
        CHECK_EQ((long)tcb_sndq(c.desc), 4096L, "err: sndq at the budget");
        req_send(&r, c.desc, buf, 16u, (USHORT)RQ_F_NONBLOCK);
        dispatch(&r);
        CHECK_EQ((long)r.errno_, (long)NSF_EWOULDBLOCK, "err: non-blocking SEND, budget full -> EWOULDBLOCK");
    }

    conn_reset(dev, &c);
    CHECK_EQ((long)tcb_inuse(), 0L, "err: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "err: BUFLARGE baseline");
}

/* ==========================================================================
 * 7. M4-4: retransmission (fixed RTO + exponential backoff) and zero-window
 *    persist probes (ADR-0033). The loop is still NOT running; a lost segment
 *    is simply a captured wire PBUF whose ACK is never injected, and the timer
 *    seam is driven by nsftmr_run(ticks) -- no threads. Every retransmission is
 *    asserted byte-identical, every backoff interval and counter is exact.
 * ========================================================================== */

/* 7a. Lost data segment: retransmit EXACTLY ONE segment at SND.UNA (go-back-N
 *     restraint), byte-identical; a progress ACK resets backoff and re-arms the
 *     timer for the remainder. */
static void test_rexmit_lost_data(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[300], fl;
    UINT   seq0, r0;
    USHORT len0;

    fill_pattern(src, sizeof(src), 0x41u);
    establish_active_opt(dev, 8120u, 0x2000u, 100u, &c);   /* MSS 100, wide window */

    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 300L, "rxt: 300 bytes buffered");
    CHECK_EQ((long)g_capcount, 3L, "rxt: 300 over MSS 100 -> 3 segments in flight");
    CHECK(tcb_rexmit_on(c.desc), "rxt: rexmit armed for in-flight data");
    check_excl(c.desc, "rxt: rexmit XOR persist (data in flight)");

    /* The three wire PBUFs were captured + freed (dropped: no ACK). Tick to RTO:
     * ONE segment retransmitted starting at SND.UNA (RFC 1122 §4.2.3.1). */
    r0 = ctr_get("NSFTCP", "rexmit");
    g_capcount = 0;
    tick(NSFTCP_RTO_TICKS);
    CHECK_EQ((long)g_capcount, 1L, "rxt: RTO retransmits exactly ONE segment");
    CHECK_EQ((long)ctr_get("NSFTCP", "rexmit"), (long)r0 + 1, "rxt: rexmit counted");
    cap_seg(0, &fl, &seq0, NULL, NULL, &len0);
    CHECK_EQ((long)seq0, (long)c.srv_next, "rxt: retransmit seq = SND.UNA");
    CHECK_EQ((long)len0, 100L, "rxt: retransmit len = one MSS from SND.UNA");
    CHECK(memcmp(cap_seg(0, NULL, NULL, NULL, NULL, NULL), src, 100) == 0,
          "rxt: retransmitted payload byte-identical to the original");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)(c.srv_next + 300u),
             "rxt: SND.NXT unchanged by a retransmit");
    CHECK_EQ((long)tcb_backoff(c.desc), 1L, "rxt: backoff doubled once");

    /* A progress ACK (first 100) resets backoff and re-arms the timer at base for
     * the remaining 200. */
    inject_ack_win(dev, &c, c.srv_next + 100u, 0x2000u);
    CHECK_EQ((long)tcb_sndq(c.desc), 200L, "rxt: 100 acked, 200 remain");
    CHECK(tcb_rexmit_on(c.desc), "rxt: timer re-armed for the remainder");
    CHECK_EQ((long)tcb_backoff(c.desc), 0L, "rxt: progress reset the backoff");
    CHECK_EQ((long)tcb_rto(c.desc), (long)NSFTCP_RTO_TICKS, "rxt: RTO back to base");

    inject_ack_win(dev, &c, c.srv_next + 300u, 0x2000u);   /* ACK the rest */
    CHECK(!tcb_rexmit_on(c.desc), "rxt: all acked -> rexmit cancelled");

    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "rxt: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "rxt: BUFLARGE baseline");
}

/* 7b. Backoff to the cap + MAXTRIES give-up: the parked SEND completes ETIMEDOUT
 *     and the connection is torn down (leak gate). */
static void test_rexmit_giveup(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[6000];
    UINT   r0;
    int    k;

    fill_pattern(src, sizeof(src), 0x30u);
    establish_active_opt(dev, 8121u, 1460u, 1460u, &c);    /* window = 1 MSS */

    req_send(&r, c.desc, src, sizeof(src), 0u);            /* 6000 > SNDBUF -> parks */
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)REQ_PENDING, "giveup: blocking send parked");
    CHECK_EQ((long)g_capcount, 1L, "giveup: one segment on the wire");
    CHECK(tcb_rexmit_on(c.desc), "giveup: rexmit armed");

    /* No ACK ever. Each expiry retransmits one segment and doubles the backoff
     * (capped at NSFTCP_RTO_MAX_TICKS). */
    r0 = ctr_get("NSFTCP", "rexmit");
    for (k = 0; k < NSFTCP_RTO_MAXTRIES; k++) {
        UINT rto = tcb_rto(c.desc);
        CHECK_EQ((long)rto, (long)rexmit_interval(k), "giveup: RTO backed off as expected");
        tick(rto);
    }
    CHECK_EQ((long)ctr_get("NSFTCP", "rexmit"), (long)r0 + NSFTCP_RTO_MAXTRIES,
             "giveup: exactly MAXTRIES retransmissions");
    CHECK(tcb_of(c.desc) != NULL, "giveup: alive through MAXTRIES retransmits");

    tick(tcb_rto(c.desc));                                  /* the give-up expiry */
    CHECK_EQ((long)r.errno_, (long)NSF_ETIMEDOUT, "giveup: parked SEND -> ETIMEDOUT");
    CHECK_EQ((long)tcb_inuse(), 0L, "giveup: TCB pool baseline after teardown");
    CHECK_EQ((long)large_inuse(), 0L, "giveup: BUFLARGE baseline");
}

/* 7c. SYN loss (active open): the SYN is retransmitted byte-identical (incl the
 *     MSS option); the connect gives up with ETIMEDOUT (the classic connect
 *     timeout). */
static void test_rexmit_syn_active(NETDEV *dev)
{
    NSFRQE r;
    UINT   desc, synseq, r0;
    UCHAR  fl, syn0[24];
    USHORT sport, len0;
    int    k;

    (void)dev;                                  /* active open uses the route, not dev */
    desc = tcp_socket();
    CHECK(desc != 0u, "syn-rxt: socket");
    rqe_init(&r, RQ_CONNECT, desc);
    r.p1 = PEER_IP; r.p2 = 8122u;
    r.retcode = REQ_PENDING;                    /* detectable if it does not park  */
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)g_capcount, 1L, "syn-rxt: connect sent one SYN");
    CHECK_EQ((long)r.retcode, (long)REQ_PENDING, "syn-rxt: connect parked");
    cap_tcp(&synseq, NULL, &fl, &sport, NULL, NULL);
    CHECK_EQ((long)fl, (long)TCP_FL_SYN, "syn-rxt: SYN only");
    memcpy(syn0, g_cap + 20, 24);          /* TCP header + MSS option (byte-exact) */
    CHECK(tcb_rexmit_on(desc), "syn-rxt: rexmit armed for the SYN");

    r0 = ctr_get("NSFTCP", "rexmit");
    g_capcount = 0;
    tick(NSFTCP_RTO_TICKS);
    CHECK_EQ((long)g_capcount, 1L, "syn-rxt: RTO retransmits the SYN");
    CHECK_EQ((long)ctr_get("NSFTCP", "rexmit"), (long)r0 + 1, "syn-rxt: rexmit counted");
    cap_seg(0, &fl, NULL, NULL, NULL, &len0);
    CHECK_EQ((long)fl, (long)TCP_FL_SYN, "syn-rxt: retransmit is SYN only");
    CHECK_EQ((long)len0, 0L, "syn-rxt: SYN carries no payload");
    CHECK(memcmp(g_cap + 20, syn0, 24) == 0,
          "syn-rxt: SYN retransmit byte-identical (seq + MSS option)");

    for (k = 0; k < NSFTCP_RTO_MAXTRIES + 2 && tcb_of(desc) != NULL; k++) {
        tick(tcb_rto(desc));
    }
    CHECK(tcb_of(desc) == NULL, "syn-rxt: connection torn down after give-up");
    CHECK_EQ((long)r.errno_, (long)NSF_ETIMEDOUT, "syn-rxt: connect completed ETIMEDOUT");
    CHECK_EQ((long)tcb_inuse(), 0L, "syn-rxt: pool baseline");
}

/* 7c'. Establishment resets the backoff: a SYN that was retransmitted must NOT
 *      carry its grown backoff/RTO into the data phase (the handshake converges on
 *      tcp_enter_established, which neither ACK-progress path resets otherwise). */
static void test_rexmit_estab_resets_backoff(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64], src[100];
    USHORT total, sport;
    UINT   desc, synseq;
    UCHAR  flags;
    UINT   peerISS = 0x51000000u;

    desc = tcp_socket();
    CHECK(desc != 0u, "estab-rst: socket");
    rqe_init(&r, RQ_CONNECT, desc);
    r.p1 = PEER_IP; r.p2 = 8127u;
    r.retcode = REQ_PENDING;
    g_capcount = 0;
    dispatch(&r);
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);

    tick(NSFTCP_RTO_TICKS);                      /* retransmit the SYN once */
    CHECK_EQ((long)tcb_backoff(desc), 1L, "estab-rst: SYN retransmit grew the backoff");

    /* Complete the handshake -> ESTABLISHED must reset backoff/RTO to base. */
    total = build_tcp(pkt, PEER_IP, HOME_IP, 8127u, sport, peerISS, synseq + 1u,
                      (UCHAR)(TCP_FL_SYN | TCP_FL_ACK), 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(desc), (long)TCP_ESTABLISHED, "estab-rst: ESTABLISHED");
    CHECK_EQ((long)tcb_backoff(desc), 0L, "estab-rst: establishment reset the backoff");
    CHECK_EQ((long)tcb_rto(desc), (long)NSFTCP_RTO_TICKS, "estab-rst: RTO reset to base");

    /* The first data send must arm rexmit at the BASE interval, not the grown one.*/
    fill_pattern(src, sizeof(src), 0x41u);
    req_send(&r, desc, src, sizeof(src), 0u);
    dispatch(&r);
    CHECK(tcb_rexmit_on(desc), "estab-rst: first data send armed rexmit");
    CHECK_EQ((long)tcb_rto(desc), (long)NSFTCP_RTO_TICKS, "estab-rst: first-send RTO at base");

    {
        CONN c;
        memset(&c, 0, sizeof(c));
        c.desc = desc; c.lport = sport; c.cport = 8127u;
        c.srv_next = synseq + 1u; c.cli_next = peerISS + 1u;
        conn_reset(dev, &c);
    }
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "estab-rst: pool baseline");
}

/* 7d. SYN|ACK loss (passive open): the embryonic child retransmits its SYN|ACK
 *     byte-identical; after give-up the child is reclaimed and the listener
 *     survives. */
static void test_rexmit_synack(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64], synack0[24];
    USHORT total, lport = g_listen_port++;
    USHORT cport = 42000u;
    UINT   cliISS = 0x70000000u, r0;
    UINT   ldesc;
    int    k;

    ldesc = tcp_socket();
    CHECK(ldesc != 0u, "synack-rxt: listen socket");
    rqe_init(&r, RQ_BIND, ldesc); r.p1 = HOME_IP; r.p2 = (UINT)lport; dispatch(&r);
    rqe_init(&r, RQ_LISTEN, ldesc); r.p1 = 4u; dispatch(&r);

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, cport, lport, cliISS, 0u,
                      (UCHAR)TCP_FL_SYN, 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "synack-rxt: SYN -> SYN|ACK");
    memcpy(synack0, g_cap + 20, 24);       /* the child's SYN|ACK (TCP hdr + MSS) */
    CHECK_EQ((long)(g_cap[33] & (TCP_FL_SYN | TCP_FL_ACK)),
             (long)(TCP_FL_SYN | TCP_FL_ACK), "synack-rxt: SYN|ACK");
    CHECK_EQ((long)tcb_inuse(), 2L, "synack-rxt: listener + embryonic child");

    r0 = ctr_get("NSFTCP", "rexmit");
    g_capcount = 0;
    tick(NSFTCP_RTO_TICKS);
    CHECK_EQ((long)g_capcount, 1L, "synack-rxt: RTO retransmits the SYN|ACK");
    CHECK_EQ((long)ctr_get("NSFTCP", "rexmit"), (long)r0 + 1, "synack-rxt: rexmit counted");
    CHECK(memcmp(g_cap + 20, synack0, 24) == 0,
          "synack-rxt: SYN|ACK retransmit byte-identical (incl MSS)");

    /* Continue to give-up using the deterministic backoff sequence (no child
     * descriptor to peek); the child is reclaimed, the listener survives. */
    for (k = 1; k <= NSFTCP_RTO_MAXTRIES + 2 && tcb_inuse() > 1u; k++) {
        tick(rexmit_interval(k));
    }
    CHECK_EQ((long)tcb_inuse(), 1L, "synack-rxt: child reclaimed, listener survives");

    conn_close(ldesc);
    CHECK_EQ((long)tcb_inuse(), 0L, "synack-rxt: listener closed -> pool baseline");
}

/* 7e. FIN loss: the FIN is re-emitted at its own sequence and SND.NXT is NOT
 *     re-incremented (the FINSENT regression); the late ACK completes teardown. */
static void test_rexmit_fin(NETDEV *dev)
{
    CONN   c;
    UINT   nxt0, r0, rseq;
    UCHAR  fl;
    USHORT len0;

    establish_active_opt(dev, 8123u, 0x2000u, 1460u, &c);
    conn_close(c.desc);                        /* FIN sent, FIN_WAIT_1 */
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_FIN_WAIT_1, "fin-rxt: FIN_WAIT_1");
    CHECK(tcb_rexmit_on(c.desc), "fin-rxt: rexmit armed for the FIN");
    nxt0 = tcb_sndnxt(c.desc);                 /* = srv_next + 1 */

    r0 = ctr_get("NSFTCP", "rexmit");
    g_capcount = 0;
    tick(NSFTCP_RTO_TICKS);
    CHECK_EQ((long)g_capcount, 1L, "fin-rxt: RTO retransmits the FIN");
    CHECK_EQ((long)ctr_get("NSFTCP", "rexmit"), (long)r0 + 1, "fin-rxt: rexmit counted");
    cap_seg(0, &fl, &rseq, NULL, NULL, &len0);
    CHECK_EQ((long)(fl & TCP_FL_FIN), (long)TCP_FL_FIN, "fin-rxt: retransmit carries FIN");
    CHECK_EQ((long)len0, 0L, "fin-rxt: FIN retransmit has no payload");
    CHECK_EQ((long)rseq, (long)c.srv_next, "fin-rxt: FIN seq = its original sequence");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)nxt0,
             "fin-rxt: SND.NXT NOT re-incremented (FINSENT stays set)");

    inject_ack_win(dev, &c, c.srv_next + 1u, 0x2000u);     /* ACK the FIN */
    CHECK_EQ((long)tcb_state(c.desc), (long)TCP_FIN_WAIT_2, "fin-rxt: FIN acked -> FIN_WAIT_2");
    CHECK(!tcb_rexmit_on(c.desc), "fin-rxt: FIN acked -> rexmit cancelled");

    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "fin-rxt: pool baseline");
}

/* 7f. Zero-window persist: probe one byte beyond the window at backing-off
 *     intervals; a window update resumes the full send; wndprobe exact; rexmit
 *     and persist never armed together. */
static void test_persist(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[500], fl;
    UINT   pseq;
    USHORT plen;

    fill_pattern(src, sizeof(src), 0x61u);
    establish_active_opt(dev, 8124u, 0u, 1460u, &c);       /* peer window 0 */

    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 500L, "persist: 500 buffered (window 0 -> nothing sent)");
    CHECK_EQ((long)g_capcount, 0L, "persist: zero window -> no data segment");
    CHECK(tcb_persist_on(c.desc), "persist: persist armed (zero window, data waiting)");
    CHECK(!tcb_rexmit_on(c.desc), "persist: rexmit NOT armed (nothing in flight)");
    check_excl(c.desc, "persist: rexmit XOR persist");

    /* First probe at the base interval: ONE byte beyond the window at SND.NXT. */
    g_capcount = 0;
    tick(NSFTCP_RTO_TICKS);
    CHECK_EQ((long)g_capcount, 1L, "persist: base interval -> one probe");
    CHECK_EQ((long)ctr_get("NSFTCP", "wndprobe"), 1L, "persist: wndprobe counted");
    cap_seg(0, &fl, &pseq, NULL, NULL, &plen);
    CHECK_EQ((long)plen, 1L, "persist: probe carries exactly one byte");
    CHECK_EQ((long)pseq, (long)c.srv_next, "persist: probe seq = first unsent byte");
    CHECK_EQ((long)(*cap_seg(0, NULL, NULL, NULL, NULL, NULL)), (long)src[0],
             "persist: probe byte = sndq[0]");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)c.srv_next,
             "persist: probe did NOT advance SND.NXT");
    CHECK_EQ((long)tcb_backoff(c.desc), 1L, "persist: backoff after one probe");

    /* A zero-window ACK keeps persist armed (peer alive); the interval backs off. */
    inject_ack_win(dev, &c, c.srv_next, 0u);
    CHECK(tcb_persist_on(c.desc), "persist: still armed after a zero-window ACK");
    CHECK(!tcb_rexmit_on(c.desc), "persist: still no rexmit");
    CHECK_EQ((long)tcb_rto(c.desc), (long)(2 * NSFTCP_RTO_TICKS), "persist: interval backed off 2x");

    g_capcount = 0;
    tick(tcb_rto(c.desc));
    CHECK_EQ((long)g_capcount, 1L, "persist: second probe after backoff");
    CHECK_EQ((long)ctr_get("NSFTCP", "wndprobe"), 2L, "persist: second wndprobe counted");

    /* Window reopens -> persist cancelled, the buffered data flows, backoff reset. */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next, 0x2000u);
    CHECK(!tcb_persist_on(c.desc), "persist: window update cancels persist");
    CHECK(tcb_rexmit_on(c.desc), "persist: data now in flight -> rexmit armed");
    CHECK_EQ((long)g_capcount, 1L, "persist: buffered data sent on the window update");
    cap_seg(0, NULL, NULL, NULL, NULL, &plen);
    CHECK_EQ((long)plen, 500L, "persist: full buffered payload resumes");
    CHECK_EQ((long)tcb_backoff(c.desc), 0L, "persist: backoff reset on resume");
    check_excl(c.desc, "persist: rexmit XOR persist after resume");

    inject_ack_win(dev, &c, c.srv_next + 500u, 0x2000u);   /* ACK the data */
    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "persist: pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "persist: BUFLARGE baseline");
}

/* 7f'. M4-6 regression: the window reopens but the peer's window-update ACK is
 *      LOST, so the sender's persist probe is the only signal. The peer accepts
 *      the 1-byte probe (window now open) and ACKs SND.NXT+1. That ack is > our
 *      SND.NXT (the probe did not advance it, ADR-0033), and the old code rejected
 *      it as "acks unsent", never processing the window update it carried -> the
 *      sender livelocked probing forever (found by the loss harness). The fix
 *      accepts the probe-ACK and resumes. */
static void test_persist_probe_ack_reopens(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[500];
    USHORT plen;
    TCB   *t;

    fill_pattern(src, sizeof(src), 0x63u);
    establish_active_opt(dev, 8130u, 0u, 1460u, &c);       /* peer window 0 */

    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 500L, "probeack: 500 buffered (window 0)");
    CHECK(tcb_persist_on(c.desc), "probeack: persist armed");

    /* Persist probe: one byte at SND.NXT, SND.NXT unmoved. */
    g_capcount = 0;
    tick(NSFTCP_RTO_TICKS);
    CHECK_EQ((long)g_capcount, 1L, "probeack: one probe");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)c.srv_next, "probeack: probe did not move SND.NXT");

    /* The peer's window REOPENED and it accepted the probe byte -> it ACKs
     * SND.NXT+1 with an open window. The window-update ACK (at SND.NXT) was lost;
     * this probe-ACK is the only signal. The sender must resume. */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next + 1u, 0x2000u);
    t = tcb_of(c.desc);
    CHECK(t != NULL && t->snd_wnd == 0x2000u, "probeack: reopened window adopted");
    CHECK(!tcb_persist_on(c.desc), "probeack: persist cancelled");
    CHECK(tcb_rexmit_on(c.desc), "probeack: remaining data now in flight");
    CHECK_EQ((long)g_capcount, 1L, "probeack: the buffered remainder is sent");
    cap_seg(0, NULL, NULL, NULL, NULL, &plen);
    CHECK_EQ((long)plen, 499L, "probeack: remainder = 500 - the 1 probed byte");

    inject_ack_win(dev, &c, c.srv_next + 500u, 0x2000u);   /* ACK all 500 */
    CHECK_EQ((long)tcb_sndq(c.desc), 0L, "probeack: sndq fully drained");
    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "probeack: pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "probeack: BUFLARGE baseline");
}

/* 7g. Partial ACK during backoff: progress resets the backoff and RTO but the
 *     timer stays armed for the unacked remainder. */
static void test_partial_ack_backoff(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[300];

    fill_pattern(src, sizeof(src), 0x41u);
    establish_active_opt(dev, 8125u, 0x2000u, 100u, &c);   /* MSS 100 */
    req_send(&r, c.desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)g_capcount, 3L, "partial: 3 segments in flight");

    tick(NSFTCP_RTO_TICKS);                                /* one no-progress expiry */
    CHECK_EQ((long)tcb_backoff(c.desc), 1L, "partial: backoff after one expiry");
    CHECK_EQ((long)tcb_rto(c.desc), (long)(2 * NSFTCP_RTO_TICKS), "partial: RTO doubled");

    inject_ack_win(dev, &c, c.srv_next + 100u, 0x2000u);   /* partial ACK -> progress */
    CHECK_EQ((long)tcb_backoff(c.desc), 0L, "partial: progress reset the backoff");
    CHECK_EQ((long)tcb_rto(c.desc), (long)NSFTCP_RTO_TICKS, "partial: RTO back to base");
    CHECK(tcb_rexmit_on(c.desc), "partial: timer still armed for the remainder");
    CHECK_EQ((long)tcb_sndq(c.desc), 200L, "partial: 200 bytes remain");

    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "partial: pool baseline");
}

/* 7h. Invariant: the retransmit and persist timers are NEVER armed together --
 *     walk zero-window -> window-open -> all-acked. */
static void test_timer_exclusive(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[200];

    fill_pattern(src, sizeof(src), 0x41u);
    establish_active_opt(dev, 8126u, 0u, 1460u, &c);       /* zero window */
    req_send(&r, c.desc, src, sizeof(src), 0u);
    dispatch(&r);
    CHECK(tcb_persist_on(c.desc) && !tcb_rexmit_on(c.desc), "excl: zero window -> persist only");
    check_excl(c.desc, "excl: never both (persisting)");

    inject_ack_win(dev, &c, c.srv_next, 0x2000u);          /* window opens */
    CHECK(tcb_rexmit_on(c.desc) && !tcb_persist_on(c.desc), "excl: window open -> rexmit only");
    check_excl(c.desc, "excl: never both (transmitting)");

    inject_ack_win(dev, &c, c.srv_next + 200u, 0x2000u);   /* all acked */
    CHECK(!tcb_rexmit_on(c.desc) && !tcb_persist_on(c.desc), "excl: all acked -> neither armed");

    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "excl: pool baseline");
}

/* 7i. Flow control: an ACK that advances SND.UNA while SHRINKING the window
 *     (right edge fixed) must NOT let the sender transmit past the edge. The bug
 *     (found live via the M4-4 persist trace) was tcp_process_ack running
 *     tcp_output -- via send_resume -- with the advanced SND.UNA but the STALE
 *     window, so the right edge appeared to slide right by the acked amount and
 *     the sender over-sent. The window must be updated BEFORE any transmit. */
static void test_flowctl_no_oversend(NETDEV *dev)
{
    CONN   c;
    NSFRQE r;
    UCHAR  src[5000];

    fill_pattern(src, sizeof(src), 0x41u);
    establish_active_opt(dev, 8128u, 1460u, 1460u, &c);    /* peer window = 1 MSS */

    /* A blocking send larger than the send budget PARKS (pend_send set) -- this is
     * the path the bug lives on: an ACK's send_resume runs tcp_output, and it must
     * see the UPDATED window. (A send that fits completes at once and never calls
     * send_resume, so the bug hides -- exactly why M4-3's tests missed it.) */
    req_send(&r, c.desc, src, sizeof(src), 0u);            /* 5000 > SNDBUF -> parks */
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)REQ_PENDING, "flowctl: blocking send parked");
    CHECK_EQ((long)g_capcount, 1L, "flowctl: one MSS in flight under the 1460 window");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)(c.srv_next + 1460u),
             "flowctl: SND.NXT at the window right edge");

    /* ACK +1024 and shrink the window to 436: 1024 + 436 == 1460, so the right
     * edge is UNCHANGED. Correct: send_resume refills the parked send but nothing
     * new goes on the wire. The bug ran tcp_output with the advanced SND.UNA and
     * the STALE 1460 window -> right edge slid to 2484 -> over-sent 1024 bytes. */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next + 1024u, 436u);
    CHECK_EQ((long)g_capcount, 0L, "flowctl: ACK+shrunk window must NOT send past the edge");
    CHECK_EQ((long)tcb_sndnxt(c.desc), (long)(c.srv_next + 1460u),
             "flowctl: SND.NXT unchanged (no over-send beyond the window)");

    /* A genuine window opening (same ack, larger window) resumes the sender. */
    g_capcount = 0;
    inject_ack_win(dev, &c, c.srv_next + 1024u, 1460u);
    CHECK_EQ((long)g_capcount, 1L, "flowctl: a genuine window opening resumes the sender");

    conn_reset(dev, &c);
    tick((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "flowctl: pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "flowctl: BUFLARGE baseline");
}

/* M4-5: non-blocking CONNECT. RQ_F_NONBLOCK completes EINPROGRESS immediately (not
 * parked) and the connection proceeds; SYN|ACK -> ESTABLISHED (success), or a RST ->
 * the socket is destroyed (the SEL_DEAD teardown poke is what aborts a parked
 * select-for-write on it -- proven in tstsel S9). No parked request either way. */
static void test_nonblock_connect(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64];
    USHORT total, sport;
    UINT   synseq;
    UCHAR  flags;
    UINT   desc;

    /* success path */
    desc = tcp_socket();
    rqe_init(&r, RQ_CONNECT, desc);
    r.p1 = PEER_IP; r.p2 = 3500u; r.flags = (USHORT)RQ_F_NONBLOCK;
    r.retcode = 0x5A5A;
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, (long)NSF_RETERR, "nbconnect: RETCODE -1");
    CHECK_EQ((long)r.errno_, (long)NSF_EINPROGRESS, "nbconnect: errno EINPROGRESS");
    CHECK_EQ((long)g_capcount, 1L, "nbconnect: SYN sent (proceeds)");
    CHECK_EQ((long)tcb_state(desc), (long)TCP_SYN_SENT, "nbconnect: SYN_SENT");
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);

    total = build_tcp(pkt, PEER_IP, HOME_IP, 3500u, sport, 0x60000000u,
                      synseq + 1u, (UCHAR)(TCP_FL_SYN | TCP_FL_ACK), 0x2000u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(desc), (long)TCP_ESTABLISHED, "nbconnect: SYN|ACK -> ESTABLISHED");
    /* tear it down with a RST (no parked request to abort). The RST must sit at
     * RCV.NXT (peerISS+1) to be acceptable in a synchronized state. */
    total = build_tcp(pkt, PEER_IP, HOME_IP, 3500u, sport, 0x60000001u,
                      0u, TCP_FL_RST, 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(desc), -1L, "nbconnect: RST tore the socket down");

    /* failure path: a refused non-blocking connect (RST in SYN_SENT) destroys the
     * socket -- no parked request, no leak. */
    desc = tcp_socket();
    rqe_init(&r, RQ_CONNECT, desc);
    r.p1 = PEER_IP; r.p2 = 3501u; r.flags = (USHORT)RQ_F_NONBLOCK;
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.errno_, (long)NSF_EINPROGRESS, "nbconnect: refused starts EINPROGRESS");
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);
    total = build_tcp(pkt, PEER_IP, HOME_IP, 3501u, sport, 0u,
                      synseq + 1u, (UCHAR)(TCP_FL_RST | TCP_FL_ACK), 0u, NULL, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(desc), -1L, "nbconnect: refused RST destroyed the socket");
    CHECK_EQ((long)tcb_inuse(), 0L, "nbconnect: no TCB leaked");
}

/* M4-6 regression: the active opener must adopt the peer's advertised window from
 * the SYN|ACK even when the peer's ISS is in the UPPER half of the sequence space
 * (SEG.SEQ >= 2^31). tcp_connect leaves SND.WL1 == 0, and the RFC 793 window-update
 * comparison TCP_SEQ_LT(0, SEG.SEQ) wraps FALSE for an upper-half SEG.SEQ, so a
 * conditional-only adoption left SND.WND == 0 and the opener could never transmit
 * -- a bug on ~half of all peers (any ISS >= 2^31, incl. NSF's own passive side),
 * masked because every other active-open test used a LOWER-half synthetic peer ISS
 * (0x50000000 / 0x60000000) and live active-open never sent guest data. The fix
 * sets SND.WND/WL1/WL2 directly on the synchronizing segment (tcp_synsent_input),
 * matching the passive child. Without it the send below emits ZERO segments. */
static void test_synack_window_high_iss(NETDEV *dev)
{
    NSFRQE r;
    UCHAR  pkt[64], opt[4], src[100];
    USHORT total, sport, l0;
    UINT   synseq, desc;
    UCHAR  flags;
    UINT   peerISS = 0xC0000000u;               /* upper half -- the bug trigger  */
    TCB   *t;

    desc = tcp_socket();
    CHECK(desc != 0u, "highiss: socket");
    rqe_init(&r, RQ_CONNECT, desc);
    r.p1 = PEER_IP; r.p2 = 9200u;
    g_capcount = 0;
    dispatch(&r);
    cap_tcp(&synseq, NULL, &flags, &sport, NULL, NULL);

    opt[0] = 2u; opt[1] = 4u; put16(opt + 2, 1460u);
    total = build_tcp_opt(pkt, PEER_IP, HOME_IP, 9200u, sport, peerISS,
                          synseq + 1u, (UCHAR)(TCP_FL_SYN | TCP_FL_ACK), 0x2000u,
                          opt, 4u);
    inject(dev, pkt, total);
    CHECK_EQ((long)tcb_state(desc), (long)TCP_ESTABLISHED, "highiss: ESTABLISHED");
    t = tcb_of(desc);
    CHECK(t != NULL && t->snd_wnd == 0x2000u,
          "highiss: peer window adopted from the SYN|ACK (was stuck at 0)");

    /* The behavioral proof: a send now actually puts a segment on the wire. */
    fill_pattern(src, sizeof(src), 0x70u);
    req_send(&r, desc, src, sizeof(src), 0u);
    g_capcount = 0;
    dispatch(&r);
    CHECK_EQ((long)r.retcode, 100L, "highiss: send buffered 100");
    CHECK_EQ((long)g_capcount, 1L, "highiss: send emitted a data segment (window live)");
    cap_seg(0, NULL, NULL, NULL, NULL, &l0);
    CHECK_EQ((long)l0, 100L, "highiss: the data segment carries all 100 bytes");

    {
        CONN c;
        memset(&c, 0, sizeof(c));
        c.desc = desc; c.cport = 9200u; c.lport = sport;
        c.cli_next = peerISS + 1u; c.srv_next = synseq + 1u;
        conn_reset(dev, &c);                    /* abort -> pool baseline          */
    }
    nsftmr_run((UINT)NSFTCP_2MSL_TICKS);
    CHECK_EQ((long)tcb_inuse(), 0L, "highiss: TCB pool baseline");
    CHECK_EQ((long)large_inuse(), 0L, "highiss: BUFLARGE baseline");
}

/* M4-5: tcp_poll -- the REAL SELECT readiness logic behind the PROTOPS vtable, each
 * ADR-0035 source asserted directly (a dummy poll in the SELECT-engine test would
 * prove only that the engine CALLS poll, not that poll is right). States and queues
 * are set by hand; poll is side-effect-free so nothing else moves. */
static void test_tcp_poll(void)
{
    PROTOPS *ops  = nsftcp_protops();
    UINT     desc = tcp_socket();
    SOCKCB  *s    = sock_lookup(desc);
    TCB     *tcb;
    PBUF    *b;

    CHECK(s != NULL && s->pcb != NULL, "poll: socket + TCB");
    tcb = (TCB *)s->pcb;

    /* fresh CLOSED socket: neither readable nor writable */
    CHECK_EQ((long)ops->poll(s, (int)(SEL_READ | SEL_WRITE)), 0L,
             "poll: CLOSED -> nothing ready");

    /* ESTABLISHED, empty sndq -> write-ready, not read-ready */
    tcb->state = (UCHAR)TCP_ESTABLISHED; tcb->sndq_bytes = 0u;
    CHECK_EQ((long)ops->poll(s, (int)SEL_WRITE), (long)SEL_WRITE,
             "poll: ESTABLISHED+room -> write-ready");
    CHECK_EQ((long)ops->poll(s, (int)SEL_READ), 0L, "poll: no data -> not read-ready");

    /* data on the rxq -> read-ready */
    b = buf_alloc(0u);
    CHECK(b != NULL, "poll: buf_alloc");
    CHECK_EQ((long)q_enq(&s->rxq, &b->q), 0L, "poll: enqueue rxq");
    CHECK_EQ((long)ops->poll(s, (int)SEL_READ), (long)SEL_READ, "poll: rxq data -> read-ready");
    buf_free(Q_ENTRY(q_deq(&s->rxq), PBUF, q));     /* drain for the EOF check      */

    /* EOF (peer FIN consumed) -> read-ready with an empty rxq */
    tcb->flags |= (UCHAR)TCB_F_RCVFIN;
    CHECK_EQ((long)ops->poll(s, (int)SEL_READ), (long)SEL_READ, "poll: EOF -> read-ready");
    tcb->flags &= (UCHAR)~TCB_F_RCVFIN;

    /* sndq full -> NOT write-ready */
    tcb->sndq_bytes = (UINT)NSFTCP_SNDBUF;
    CHECK_EQ((long)ops->poll(s, (int)SEL_WRITE), 0L, "poll: sndq full -> not write-ready");
    tcb->sndq_bytes = 0u;

    /* SYN_SENT (connecting) -> not write-ready yet */
    tcb->state = (UCHAR)TCP_SYN_SENT;
    CHECK_EQ((long)ops->poll(s, (int)SEL_WRITE), 0L, "poll: SYN_SENT -> not write-ready");

    /* a listener with a pending connection on the acceptq -> read-ready */
    tcb->state = (UCHAR)TCP_LISTEN;
    {
        QELEM child;
        memset(&child, 0, sizeof(child));
        CHECK_EQ((long)q_enq(&s->acceptq, &child), 0L, "poll: enqueue acceptq");
        CHECK_EQ((long)ops->poll(s, (int)SEL_READ), (long)SEL_READ,
                 "poll: acceptq pending -> listener read-ready");
        (void)q_deq(&s->acceptq);                   /* unlink before teardown       */
    }

    tcp_close(desc);
}

int main(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("=== nsf370 NSFTCP tests (M4-1 + M4-2 + M4-3 + M4-4) ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init");
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve");
    CHECK(nsftcp_reserve(TSTTCP_POOL) == 0, "nsftcp_reserve");
    mm_init_complete();

    dev_init();
    nsfip_init();
    nsficmp_init();
    soc_init();
    nsfreq_init();
    nsftcp_init();                      /* registers the IP demux handler for 6 */
    CHECK_EQ((long)nsfreq_register_proto(6u, nsftcp_protops()), 0L, "register TCP proto");

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "CAP0", 4);
    cfg.cuu = 0x0500; cfg.type = NSFDEV_T_HOST; cfg.ipaddr = HOME_IP; cfg.mtu = 1500;
    dev = dev_register(&cfg, &cap_ops);
    CHECK(dev != NULL, "dev_register (capture)");
    CHECK(dev_start(dev) == 0, "dev_start (capture)");
    nsfip_local_add(HOME_IP);
    CHECK(nsfip_route_add(0u, 0u, dev, 0u) == 0, "default route -> capture");

    app_init();

    /* Pinned first. */
    test_seq_macros();
    test_checksum_vector();

    /* RFC 793 §3.4 RST generation. */
    test_rst_syn(dev);
    test_rst_ack(dev);
    test_rst_synack(dev);
    test_rst_on_rst_silent(dev);
    test_rst_nonunicast_src(dev);
    test_badcksum_drop(dev);
    test_malformed_drop(dev);

    /* Socket / TCB lifecycle. */
    test_attach_lifecycle();
    test_destroy_leak_gate();
    test_pool_exhaustion();

    /* M4-2: handshake, teardown matrix, TIME_WAIT, RST, malformed options. */
    test_active_handshake(dev);
    test_passive_handshake(dev);
    test_active_refused(dev);
    test_rst_econnreset(dev);
    test_rst_established(dev);
    test_teardown_active(dev);
    test_teardown_passive(dev);
    test_teardown_simultaneous(dev);
    test_malformed_options(dev);
    test_timewait_reclaim(dev);
    test_timewait_reclaim_active(dev);

    /* M4-3: data path -- segmentation, sliding window, in-order receive, EOF,
     * copy-on-transmit recovery, and the error paths. */
    test_data_segmentation(dev);
    test_send_window(dev);
    test_send_block(dev);
    test_recv_inorder(dev);
    test_recv_overlap_dup_gap(dev);
    test_recv_window_update(dev);
    test_eof_matrix(dev);
    test_xmit_failure(dev);
    test_data_errors(dev);

    /* M4-4: retransmission (fixed RTO + backoff) + zero-window persist. */
    test_rexmit_lost_data(dev);
    test_rexmit_giveup(dev);
    test_rexmit_syn_active(dev);
    test_rexmit_estab_resets_backoff(dev);
    test_rexmit_synack(dev);
    test_rexmit_fin(dev);
    test_persist(dev);
    test_persist_probe_ack_reopens(dev);
    test_partial_ack_backoff(dev);
    test_timer_exclusive(dev);
    test_flowctl_no_oversend(dev);

    /* M4-5: the SELECT readiness probe + non-blocking connect. */
    test_tcp_poll();
    test_nonblock_connect(dev);

    /* M4-6: active-open window adoption with an upper-half peer ISS (the bug the
     * loss harness exposed; folded in with the fix). */
    test_synack_window_high_iss(dev);

    {
        NSFRQE rt;
        rqe_init(&rt, RQ_TERMAPI, 0u); rt.apptok = g_apptok;
        nsfreq_dispatch(&rt);
    }

    /* Leak gate: every pool back to baseline. */
    CHECK_EQ((long)soc_count(), 0L, "final: no sockets left open");
    CHECK_EQ((long)tcb_inuse(), 0L, "final: TCPTCB pool fully returned");
    CHECK_EQ((long)sock_inuse(), 0L, "final: SOCKET pool fully returned");
    CHECK_EQ((long)small_inuse(), 0L, "final: BUFSMALL fully returned");
    CHECK_EQ((long)large_inuse(), 0L, "final: BUFLARGE fully returned");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "final: EVT pool at baseline");

    mm_shutdown();
    return mbt_test_summary("TSTTCP");
}
