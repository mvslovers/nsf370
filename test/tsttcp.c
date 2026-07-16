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
static UCHAR  g_cap[2048];
static USHORT g_caplen;
static int    g_capcount;

static int cap_init(NETDEV *d, const DEVCFG *c)     { (void)d; (void)c; return 0; }
static int cap_start(NETDEV *d)                     { (void)d; return 0; }
static int cap_shutdown(NETDEV *d)                  { (void)d; return 0; }
static int cap_send(NETDEV *d, PBUF *b)
{
    (void)d;
    g_caplen = buf_copyout(b, g_cap, (USHORT)sizeof(g_cap));
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

int main(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("=== nsf370 NSFTCP tests (M4-1) ===\n");

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
    CHECK_EQ((long)nsfevt_inuse(), 0L, "final: EVT pool at baseline");

    mm_shutdown();
    return mbt_test_summary("TSTTCP");
}
