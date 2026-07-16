/*
 * tsttcpm.c -- NSFTCP on MVS: the portable segment vectors, byte-order proof
 * (spec ch. 13, M4-1). host = false.
 *
 * The byte-order-sensitive half of tsttcp.c re-run on the S/370 target, tstip-M2
 * style: the same crafted segments driven through the real IP path over a
 * CAPTURE device, the loop NOT running, so every RST byte is asserted
 * deterministically -- but now on big-endian 3.8j, where the little-endian host
 * cannot prove the wire encoding. Proves:
 *   - the sequence-arithmetic macros (signed 32-bit difference) on S/370;
 *   - the TCP checksum against the independent hand vector 0x22F4;
 *   - the RFC 793 §3.4 RST generation byte-exact (SYN -> RST,ACK with
 *     ack = seg.seq + 1; ACK -> RST seq = seg.ack; SYN+ACK -> the ACK branch;
 *     RST -> silence), big-endian header build + nsfip_output on target.
 * The socket/TCB lifecycle + drop-path logic are host-covered by TSTTCP (not
 * byte-order-sensitive); the TCB SIZE_ASSERT (188 B) is enforced by this file's
 * cross-compile, and nsftcp_reserve here proves the 256-byte target slot builds.
 *
 * No live handshake yet -- that is M4-2's gate.
 */
#include "nsftcp.h"
#include "nsfip.h"
#include "nsficmp.h"
#include "nsfcksum.h"
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
    buf_free(b);
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

static USHORT raw_cksum(const UCHAR *p, USHORT len)
{
    PBUF tp;
    memset(&tp, 0, sizeof(tp));
    tp.data = (UCHAR *)p;
    tp.len  = len;
    return in_cksum(&tp, 0u, len);
}

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

/* Build a complete IPv4 + TCP segment with valid IP + TCP checksums. */
static USHORT build_tcp(UCHAR *buf, UINT src, UINT dst, USHORT sport, USHORT dport,
                        UINT seq, UINT ack, UCHAR flags, USHORT window)
{
    UCHAR *ip = buf;
    UCHAR *t  = buf + 20;
    USHORT tcplen = 20u;
    USHORT total  = 40u;
    UINT   seed;
    PBUF   tp;

    ip[0] = 0x45u; ip[1] = 0u; put16(ip + 2, total);
    put16(ip + 4, 0x4321u); put16(ip + 6, 0u);
    ip[8] = 64u; ip[9] = (UCHAR)NSFIP_PROTO_TCP;
    ip[10] = 0u; ip[11] = 0u; put32(ip + 12, src); put32(ip + 16, dst);
    put16(ip + 10, raw_cksum(ip, 20u));

    put16(t + 0, sport); put16(t + 2, dport);
    put32(t + 4, seq);   put32(t + 8, ack);
    t[12] = (UCHAR)((20u / 4u) << 4);
    t[13] = flags;
    put16(t + 14, window); put16(t + 16, 0u); put16(t + 18, 0u);
    seed = tcp_seed(src, dst, tcplen);
    memset(&tp, 0, sizeof(tp)); tp.data = t; tp.len = tcplen;
    put16(t + 16, in_cksum_fold(in_cksum_partial(&tp, 0u, tcplen, seed)));
    return total;
}

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

static void inject(NETDEV *dev, const UCHAR *pkt, USHORT total)
{
    PBUF *b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "inject: rx_pbuf allocated");
    nsfip_input(dev, b);
    nsfdev_kick_output();
}

/* ---- 1. sequence arithmetic across the 2^32 wrap ---------------------------- */
static void test_seq_macros(void)
{
    CHECK(TCP_SEQ_LT(100u, 200u),  "seq: 100 < 200");
    CHECK(TCP_SEQ_GT(200u, 100u),  "seq: 200 > 100");
    CHECK(!TCP_SEQ_LT(5u, 5u),     "seq: 5 not < 5");
    CHECK(TCP_SEQ_LEQ(5u, 5u),     "seq: 5 <= 5");
    CHECK(TCP_SEQ_GEQ(5u, 5u),     "seq: 5 >= 5");
    CHECK(TCP_SEQ_LT(0x7FFFFFF0u, 0x80000010u), "seq: 0x7FFFFFF0 < 0x80000010 (midpoint)");
    CHECK(TCP_SEQ_GT(0x80000010u, 0x7FFFFFF0u), "seq: 0x80000010 > 0x7FFFFFF0 (midpoint)");
    CHECK(TCP_SEQ_LT(0xFFFFFFF0u, 0x00000010u), "seq: 0xFFFFFFF0 < 0x10 (wrap past 0)");
    CHECK(TCP_SEQ_GT(0x00000010u, 0xFFFFFFF0u), "seq: 0x10 > 0xFFFFFFF0 (wrap past 0)");
    CHECK(TCP_SEQ_LT(0u, 0x7FFFFFFFu), "seq: 0 < 2^31-1");
    CHECK(TCP_SEQ_GT(0u, 0x80000001u), "seq: 0 > 2^31+1 (nearer going backward)");
}

/* ---- 2. checksum vector 0x22F4 (two-sided, big-endian on S/370) ------------- */
static void test_checksum_vector(void)
{
    UCHAR seg[20];
    PBUF  tp;
    UINT  seed;

    put16(seg + 0, 0x1234u);   put16(seg + 2, 0x0050u);
    put32(seg + 4, 0x11223344u); put32(seg + 8, 0x00000000u);
    seg[12] = (UCHAR)((20u / 4u) << 4);  seg[13] = (UCHAR)TCP_FL_SYN;
    put16(seg + 14, 0x2000u);  put16(seg + 16, 0u);  put16(seg + 18, 0u);

    seed = tcp_seed(PEER_IP, HOME_IP, 20u);
    memset(&tp, 0, sizeof(tp)); tp.data = seg; tp.len = 20u;
    CHECK_EQ((long)in_cksum_fold(in_cksum_partial(&tp, 0u, 20u, seed)), 0x22F4L,
             "TCP checksum over the hand vector == 0x22F4 (big-endian on S/370)");
    put16(seg + 16, 0x22F4u);
    CHECK_EQ((long)in_cksum_fold(in_cksum_partial(&tp, 0u, 20u, seed)), 0L,
             "a segment carrying 0x22F4 verifies to 0");
}

/* ---- 3. RST generation byte-exact ------------------------------------------- */
static void rst_fields(UINT *seq, UINT *ack, UCHAR *flags, USHORT *sport, USHORT *dport)
{
    PBUF tp;
    UINT seed;

    CHECK_EQ((long)g_caplen, 40L, "RST: header-only (IP20 + TCP20, no payload)");
    CHECK_EQ((long)g_cap[0], 0x45L, "RST: IP version 4 IHL 5");
    CHECK_EQ((long)g_cap[9], (long)NSFIP_PROTO_TCP, "RST: IP protocol TCP");
    CHECK_EQ((long)raw_cksum(g_cap, 20u), 0L, "RST: IP header verifies to 0");
    CHECK_EQ((long)get32(g_cap + 12), (long)HOME_IP, "RST: source is our address");
    CHECK_EQ((long)get32(g_cap + 16), (long)PEER_IP, "RST: dest is the offender");
    seed = tcp_seed(HOME_IP, PEER_IP, 20u);
    memset(&tp, 0, sizeof(tp)); tp.data = g_cap + 20; tp.len = 20u;
    CHECK_EQ((long)in_cksum_fold(in_cksum_partial(&tp, 0u, 20u, seed)), 0L,
             "RST: TCP checksum verifies to 0 (big-endian on S/370)");
    if (sport != NULL) { *sport = get16(g_cap + 20); }
    if (dport != NULL) { *dport = get16(g_cap + 22); }
    if (seq   != NULL) { *seq   = get32(g_cap + 24); }
    if (ack   != NULL) { *ack   = get32(g_cap + 28); }
    if (flags != NULL) { *flags = (UCHAR)(g_cap[33]); }
}

static void test_rst_syn(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   seq, ack;
    UCHAR  flags;
    USHORT sport, dport;

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40000u, 80u,
                      0x11223344u, 0u, (UCHAR)TCP_FL_SYN, 0x2000u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "SYN -> one RST sent");
    rst_fields(&seq, &ack, &flags, &sport, &dport);
    CHECK_EQ((long)sport, 80L,    "RST source port = offending dest port");
    CHECK_EQ((long)dport, 40000L, "RST dest port = offending source port");
    CHECK_EQ((long)flags, (long)(TCP_FL_RST | TCP_FL_ACK), "SYN -> RST|ACK");
    CHECK_EQ((long)seq, 0L, "SYN -> RST seq = 0");
    CHECK_EQ((long)ack, (long)0x11223345u, "SYN -> RST ack = seg.seq + 1 (SEG.LEN counts SYN)");
}

static void test_rst_ack(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   seq, ack;
    UCHAR  flags;

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40001u, 81u,
                      0x22334455u, 0xAABBCCDDu, (UCHAR)TCP_FL_ACK, 0x2000u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "ACK -> one RST sent");
    rst_fields(&seq, &ack, &flags, NULL, NULL);
    CHECK_EQ((long)flags, (long)TCP_FL_RST, "ACK -> RST only (no ACK flag)");
    CHECK_EQ((long)seq, (long)0xAABBCCDDu, "ACK -> RST seq = seg.ack");
    CHECK_EQ((long)ack, 0L, "ACK -> RST ack field 0");
}

static void test_rst_synack(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   seq, ack;
    UCHAR  flags;

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40002u, 82u,
                      0x01020304u, 0x0A0B0C0Du,
                      (UCHAR)(TCP_FL_SYN | TCP_FL_ACK), 0x2000u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 1L, "SYN+ACK -> one RST sent");
    rst_fields(&seq, &ack, &flags, NULL, NULL);
    CHECK_EQ((long)flags, (long)TCP_FL_RST, "SYN+ACK -> RST only (ACK bit dominates)");
    CHECK_EQ((long)seq, (long)0x0A0B0C0Du, "SYN+ACK -> RST seq = seg.ack");
    CHECK_EQ((long)ack, 0L, "SYN+ACK -> RST ack field 0");
}

static void test_rst_silence(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   rcvd0 = ctr_get("NSFTCP", "resetrcvd");

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40003u, 83u,
                      0x33445566u, 0u, (UCHAR)TCP_FL_RST, 0u);
    inject(dev, pkt, total);
    CHECK_EQ((long)g_capcount, 0L, "RST -> no reply (no RST-on-RST)");
    CHECK_EQ((long)ctr_get("NSFTCP", "resetrcvd"), (long)rcvd0 + 1, "inbound RST counted resetrcvd");
}

static void test_badcksum(NETDEV *dev)
{
    UCHAR  pkt[64];
    USHORT total;
    UINT   bad0 = ctr_get("NSFTCP", "badcksum");

    g_capcount = 0;
    total = build_tcp(pkt, PEER_IP, HOME_IP, 40005u, 85u,
                      0x55667788u, 0u, (UCHAR)TCP_FL_SYN, 0x2000u);
    pkt[20 + 17] ^= 0xFFu;              /* corrupt the TCP checksum            */
    inject(dev, pkt, total);
    CHECK_EQ((long)ctr_get("NSFTCP", "badcksum"), (long)bad0 + 1, "corrupt checksum -> badcksum");
    CHECK_EQ((long)g_capcount, 0L, "bad checksum -> dropped, no RST");
}

int main(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("=== nsf370 NSFTCP tests on MVS (M4-1, byte-order proof) ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init");
    CHECK(buf_init() == 0, "buf_init");
    CHECK(nsftcp_reserve(4) == 0, "nsftcp_reserve (256-byte target slot builds)");
    mm_init_complete();

    dev_init();
    nsfip_init();
    nsficmp_init();
    nsftcp_init();                      /* registers the IP demux handler for 6 */

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "CAP0", 4);
    cfg.cuu = 0x0500; cfg.type = NSFDEV_T_HOST; cfg.ipaddr = HOME_IP; cfg.mtu = 1500;
    dev = dev_register(&cfg, &cap_ops);
    CHECK(dev != NULL, "dev_register (capture)");
    CHECK(dev_start(dev) == 0, "dev_start (capture)");
    nsfip_local_add(HOME_IP);
    CHECK(nsfip_route_add(0u, 0u, dev, 0u) == 0, "default route -> capture");

    test_seq_macros();
    test_checksum_vector();
    test_rst_syn(dev);
    test_rst_ack(dev);
    test_rst_synack(dev);
    test_rst_silence(dev);
    test_badcksum(dev);

    CHECK_EQ((long)ctr_get("NSFTCP", "resetsent"), 3L,
             "resetsent == 3 (SYN, ACK, SYN+ACK; RST/badcksum sent none)");

    mm_shutdown();
    return mbt_test_summary("TSTTCPM");
}
