/*
 * tstip.c -- NSFIP host unit tests (spec ch. 11, M2-2).
 *
 * Drives the IP layer deterministically over a CAPTURE device (a DEVOPS whose
 * send copies the outbound packet to a global and frees it) with the loop NOT
 * running -- nsfip_input / nsfip_output are plain functions, and nsfdev_kick_output
 * drains the sendq into the capture. This is the tstdev "loop not running"
 * pattern: no threads, fully repeatable, exact byte/counter assertions.
 *
 *   - input drop classes: a fragment, a not-our-dest, a bad header checksum, a
 *     bad version and a bad length each hit the matching §11.7 counter and free
 *     the PBUF (pool back to baseline);
 *   - valid demux: an ICMP echo request addressed to us demuxes to ICMP (which
 *     replies -- captured here), incrementing ip_in / icmp_inecho;
 *   - output: nsfip_output builds a header that verifies to 0, routes to the
 *     peer, and bumps the id counter each call;
 *   - no route: an output with no matching route counts ip_noroute and frees;
 *   - leak gate: all pools back to baseline.
 *
 * Counters are read back through the real registry (sts_value), so the test
 * exercises registration too.
 */
#include "nsfip.h"
#include "nsficmp.h"
#include "nsfcksum.h"
#include "nsfdev.h"
#include "nsfbuf.h"
#include "nsfevt.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include "nsftrc.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define ctr_get(comp, name)  sts_value((comp), (name))

#define HOME_IP   0x0A010102u       /* 10.1.1.2 -- this stack           */
#define PEER_IP   0x0A010101u       /* 10.1.1.1 -- the point-to-point peer */
#define OTHER_IP  0x0A0100FEu       /* 10.1.0.254 -- someone else       */

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

/* Build an ICMP echo message (type 8 request / 0 reply) with a valid checksum. */
static USHORT build_icmp(UCHAR *buf, UCHAR type, USHORT id, USHORT seq,
                         const UCHAR *data, USHORT dlen)
{
    USHORT len = (USHORT)(8u + dlen);
    buf[0] = type;
    buf[1] = 0u;                        /* code */
    buf[2] = 0u; buf[3] = 0u;           /* checksum (filled below) */
    put16(buf + 4, id);
    put16(buf + 6, seq);
    if (dlen != 0u) {
        memcpy(buf + 8, data, dlen);
    }
    put16(buf + 2, raw_cksum(buf, len));
    return len;
}

/* Build a complete IPv4 packet (20-byte header + payload) with a valid header
 * checksum. Returns the total length. */
static USHORT build_ip(UCHAR *buf, UCHAR ver_ihl, USHORT fragword, UCHAR ttl,
                       UCHAR proto, UINT src, UINT dst,
                       const UCHAR *payload, USHORT paylen)
{
    UCHAR  hlen  = (UCHAR)((ver_ihl & 0x0Fu) * 4u);
    USHORT total = (USHORT)(hlen + paylen);

    buf[0] = ver_ihl;
    buf[1] = 0u;                        /* TOS */
    put16(buf + 2, total);
    put16(buf + 4, 0x1234u);           /* id */
    put16(buf + 6, fragword);
    buf[8] = ttl;
    buf[9] = proto;
    buf[10] = 0u; buf[11] = 0u;        /* checksum (filled below) */
    put32(buf + 12, src);
    put32(buf + 16, dst);
    if (paylen != 0u) {
        memcpy(buf + hlen, payload, paylen);
    }
    put16(buf + 10, raw_cksum(buf, hlen));
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

/* ---- counter readback via the registry (sts_value, see the macro above) ----- */
static UINT pool_inuse(UCHAR class)
{
#if NSF_DEBUG
    MMSTATS s;
    mm_stats(buf_debug_pool(class), &s);
    return s.inuse;
#else
    (void)class;
    return 0u;
#endif
}

/* ---- input drop classes ------------------------------------------------------ */
static void test_input_drops(NETDEV *dev)
{
    UCHAR  pkt[128];
    UCHAR  icmp[16];
    USHORT il, total;
    PBUF  *b;
    UINT   before;

    il = build_icmp(icmp, 8u, 0x1111u, 1u, (const UCHAR *)"ping", 4u);

    /* 1. fragment (MF set) -> ip_fragdrop */
    before = ctr_get("NSFIP", "fragdrop");
    total  = build_ip(pkt, 0x45u, 0x2000u /* MF */, 64u, NSFIP_PROTO_ICMP,
                      PEER_IP, HOME_IP, icmp, il);
    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "frag: rx_pbuf allocated");
    nsfip_input(dev, b);
    CHECK_EQ((long)ctr_get("NSFIP", "fragdrop"), (long)before + 1, "fragment -> ip_fragdrop");

    /* 2. not our destination -> ip_inaddrerr */
    before = ctr_get("NSFIP", "inaddrerr");
    total  = build_ip(pkt, 0x45u, 0u, 64u, NSFIP_PROTO_ICMP,
                      PEER_IP, OTHER_IP /* not us */, icmp, il);
    b = rx_pbuf(pkt, total);
    nsfip_input(dev, b);
    CHECK_EQ((long)ctr_get("NSFIP", "inaddrerr"), (long)before + 1, "not-our-dest -> ip_inaddrerr");

    /* 3. bad header checksum -> ip_badcksum */
    before = ctr_get("NSFIP", "badcksum");
    total  = build_ip(pkt, 0x45u, 0u, 64u, NSFIP_PROTO_ICMP, PEER_IP, HOME_IP, icmp, il);
    pkt[11] ^= 0xFFu;                    /* corrupt the checksum */
    b = rx_pbuf(pkt, total);
    nsfip_input(dev, b);
    CHECK_EQ((long)ctr_get("NSFIP", "badcksum"), (long)before + 1, "bad checksum -> ip_badcksum");

    /* 4. bad version (6) -> ip_hdrerr */
    before = ctr_get("NSFIP", "hdrerr");
    total  = build_ip(pkt, 0x65u /* version 6, IHL 5 */, 0u, 64u,
                      NSFIP_PROTO_ICMP, PEER_IP, HOME_IP, icmp, il);
    b = rx_pbuf(pkt, total);
    nsfip_input(dev, b);
    CHECK_EQ((long)ctr_get("NSFIP", "hdrerr"), (long)before + 1, "bad version -> ip_hdrerr");

    /* 5. bad length (total_length claims more than the buffer holds) -> ip_badlen.
     * Build a valid packet, then rewrite total_length larger than the received
     * length; the checksum then also breaks, but the length check runs first. */
    before = ctr_get("NSFIP", "badlen");
    total  = build_ip(pkt, 0x45u, 0u, 64u, NSFIP_PROTO_ICMP, PEER_IP, HOME_IP, icmp, il);
    b = rx_pbuf(pkt, total);
    put16(b->data + 2, (USHORT)(total + 40u));   /* claim 40 bytes we do not have */
    nsfip_input(dev, b);
    CHECK_EQ((long)ctr_get("NSFIP", "badlen"), (long)before + 1, "bad length -> ip_badlen");

    /* Every drop path freed its PBUF. */
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "input drops leaked no small buffers");
}

/* ---- valid demux (input -> ICMP -> output, reply captured) ------------------- */
static void test_valid_demux(NETDEV *dev)
{
    UCHAR  pkt[128];
    UCHAR  icmp[16];
    USHORT il, total;
    PBUF  *b;
    UINT   in0, inecho0;

    in0     = ctr_get("NSFIP", "in");
    inecho0 = ctr_get("NSFICM", "inecho");
    g_capcount = 0;

    il    = build_icmp(icmp, 8u, 0xABCDu, 7u, (const UCHAR *)"12345678", 8u);
    total = build_ip(pkt, 0x45u, 0u, 64u, NSFIP_PROTO_ICMP, PEER_IP, HOME_IP, icmp, il);
    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "demux: rx_pbuf allocated");

    nsfip_input(dev, b);                 /* validates, demuxes to ICMP (replies) */
    nsfdev_kick_output();                /* drain the reply into the capture */

    CHECK_EQ((long)ctr_get("NSFIP", "in"), (long)in0 + 1, "ip_in counted the received packet");
    CHECK_EQ((long)ctr_get("NSFICM", "inecho"), (long)inecho0 + 1, "demuxed to ICMP (icmp_inecho)");
    CHECK_EQ((long)g_capcount, 1, "an echo reply was transmitted");
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "one PBUF request->reply, no leak");
}

/* ---- output builds a valid header and routes ------------------------------- */
static void test_output(NETDEV *dev)
{
    PBUF  *b;
    UCHAR  payload[16];
    UINT   i;
    USHORT id1, id2;
    UINT   out0;

    (void)dev;
    for (i = 0u; i < sizeof(payload); i++) {
        payload[i] = (UCHAR)i;
    }

    out0 = ctr_get("NSFIP", "out");

    /* First output. */
    b = buf_alloc(sizeof(payload));
    CHECK(b != NULL, "output: buf_alloc");
    (void)buf_copyin(b, payload, (USHORT)sizeof(payload));
    g_capcount = 0;
    CHECK(nsfip_output(b, HOME_IP, PEER_IP, NSFIP_PROTO_ICMP, 64u) == 0,
          "nsfip_output routed to the peer");
    nsfdev_kick_output();
    CHECK_EQ((long)g_capcount, 1, "output packet was transmitted");
    CHECK_EQ((long)ctr_get("NSFIP", "out"), (long)out0 + 1, "ip_out counted the transmitted packet");

    /* The captured packet: v4/IHL5, header verifies to 0, addresses, total len. */
    CHECK_EQ((long)(g_cap[0]), 0x45L, "output header is version 4 IHL 5");
    CHECK_EQ((long)raw_cksum(g_cap, 20u), 0L, "output header checksum verifies to 0");
    CHECK_EQ((long)get32(g_cap + 12), (long)HOME_IP, "output source address");
    CHECK_EQ((long)get32(g_cap + 16), (long)PEER_IP, "output destination address");
    CHECK_EQ((long)get16(g_cap + 2), (long)(20u + sizeof(payload)), "output total length");
    CHECK_EQ((long)(g_cap[9]), (long)NSFIP_PROTO_ICMP, "output protocol");
    id1 = get16(g_cap + 4);

    /* Second output: the id counter must advance. */
    b = buf_alloc(sizeof(payload));
    (void)buf_copyin(b, payload, (USHORT)sizeof(payload));
    CHECK(nsfip_output(b, HOME_IP, PEER_IP, NSFIP_PROTO_ICMP, 64u) == 0, "second output");
    nsfdev_kick_output();
    id2 = get16(g_cap + 4);
    CHECK(id2 != id1, "identification counter advanced between packets");
    CHECK_EQ((long)ctr_get("NSFIP", "out"), (long)out0 + 2, "ip_out counted both transmitted packets");

    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "output path leaked nothing");
}

/* ---- an unsupported protocol counts noproto (M2-4 trigger) ------------------ */
static void test_noproto(NETDEV *dev)
{
    UCHAR  pkt[64];
    UCHAR  payload[8];
    USHORT total;
    PBUF  *b;
    UINT   before, i;

    for (i = 0u; i < sizeof(payload); i++) {
        payload[i] = (UCHAR)i;
    }

    before = ctr_get("NSFIP", "noproto");
    total  = build_ip(pkt, 0x45u, 0u, 64u, 253u /* unassigned */,
                      PEER_IP, HOME_IP, payload, sizeof(payload));
    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "noproto: rx_pbuf allocated");
    nsfip_input(dev, b);
    nsfdev_kick_output();          /* drain the M2-4 protocol-unreachable error */
    CHECK_EQ((long)ctr_get("NSFIP", "noproto"), (long)before + 1,
             "unassigned protocol -> ip_noproto");
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "noproto path leaked nothing");
}

/* ---- trace flag gates the NSFTRC ring (M2-5) --------------------------------- */
static void test_trace(NETDEV *dev)
{
    UCHAR  pkt[128];
    UCHAR  icmp[16];
    USHORT il, total;
    PBUF  *b;
    UINT   base, i, found_ip, found_icmp;

    nsftrc_init();
    CHECK_EQ((long)nsftrc_flags, 0L, "trace flags are off by default");

    il    = build_icmp(icmp, 8u, 0x2222u, 1u, (const UCHAR *)"trace-me", 8u);
    total = build_ip(pkt, 0x45u, 0u, 64u, NSFIP_PROTO_ICMP, PEER_IP, HOME_IP, icmp, il);

    /* Flags off: nothing recorded even though the packet is fully valid. */
    base = nsftrc_count();
    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "trace(off): rx_pbuf allocated");
    nsfip_input(dev, b);
    nsfdev_kick_output();
    CHECK_EQ((long)nsftrc_count(), (long)base, "flags off -> ring gains nothing");

    /* Flags on: the same packet leaves both an IP and an ICMP entry. */
    nsftrc_enable((UINT)(TRCF_IP | TRCF_ICMP));
    base = nsftrc_count();
    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "trace(on): rx_pbuf allocated");
    nsfip_input(dev, b);
    nsfdev_kick_output();
    CHECK(nsftrc_count() > base, "flag on -> ring gained at least one entry");

    found_ip = 0u;
    found_icmp = 0u;
    for (i = base; i < nsftrc_count(); i++) {
        const TRCENT *e = nsftrc_peek(i);
        if (e == NULL) {
            continue;
        }
        if (e->flag == TRCF_IP) {
            found_ip = 1u;
        }
        if (e->flag == TRCF_ICMP) {
            found_icmp = 1u;
        }
    }
    CHECK(found_ip != 0u, "the ring has an IP-flagged entry for the packet");
    CHECK(found_icmp != 0u, "the ring has an ICMP-flagged entry for the packet");

    nsftrc_disable((UINT)(TRCF_IP | TRCF_ICMP));
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "trace test leaked nothing");
}

/* ---- no route -------------------------------------------------------------- */
static void test_no_route(void)
{
    PBUF *b;
    UINT  before = ctr_get("NSFIP", "noroute");

    /* Rebuild the table with NO routes so the lookup fails. */
    nsfip_init();
    nsfip_local_add(HOME_IP);

    b = buf_alloc(8u);
    CHECK(b != NULL, "noroute: buf_alloc");
    (void)buf_copyin(b, (const UCHAR *)"nowhere!", 8u);
    CHECK(nsfip_output(b, HOME_IP, PEER_IP, NSFIP_PROTO_ICMP, 64u) != 0,
          "nsfip_output with no route fails");
    CHECK_EQ((long)ctr_get("NSFIP", "noroute"), (long)before + 1, "no route -> ip_noroute");
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "no-route path freed the PBUF");
}

int main(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("=== nsf370 NSFIP tests ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init");
    CHECK(buf_init() == 0, "buf_init");
    mm_init_complete();

    dev_init();
    nsfip_init();
    nsficmp_init();

    /* One capture device standing in for the CTCI/HOST interface, HOME on it. */
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "CAP0", 4);
    cfg.cuu    = 0x0500;
    cfg.type   = NSFDEV_T_HOST;
    cfg.ipaddr = HOME_IP;
    cfg.mtu    = 1500;
    dev = dev_register(&cfg, &cap_ops);
    CHECK(dev != NULL, "dev_register (capture)");
    CHECK(dev_start(dev) == 0, "dev_start (capture up)");

    nsfip_local_add(HOME_IP);
    CHECK(nsfip_route_add(0u, 0u, dev, 0u) == 0, "default route -> capture device");
    CHECK(nsfip_route(PEER_IP, NULL) == dev, "route lookup resolves the peer to the device");
    CHECK(nsfip_is_local(HOME_IP) == 1, "HOME is local");
    CHECK(nsfip_is_local(PEER_IP) == 0, "peer is not local");

    test_input_drops(dev);
    test_valid_demux(dev);
    test_output(dev);
    test_noproto(dev);
    test_trace(dev);
    test_no_route();          /* rebuilds the routing table -- run last */

    CHECK_EQ((long)ctr_get("NSFIP", "ttlexp"), 0L,
             "ttlexp stays 0 in v1 (host, not router -- spec 11.1)");

    /* Leak gate: every buffer freed; the EVT pool untouched by this path. */
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_SMALL), 0, "BUFSMALL at baseline (no leak)");
    CHECK_EQ((long)pool_inuse(NSFBUF_CLASS_LARGE), 0, "BUFLARGE at baseline (no leak)");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline (no leak)");

    mm_shutdown();
    return mbt_test_summary("TSTIP");
}
