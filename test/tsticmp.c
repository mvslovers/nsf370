/*
 * tsticmp.c -- NSFICM echo responder + error generator host tests
 * (spec ch. 11, M2-3/M2-4).
 *
 * Scenario 1 (over the NSFHOST loopback, real thread handoff + leak gate): a
 * crafted echo REQUEST is transmitted; the loopback relays it back inbound, the
 * EV_PACKET_RECEIVED handler runs the real nsfip_input -> nsficmp_input path, and
 * the generated echo REPLY loops back a second time -- where the handler captures
 * it and asserts type 0, swapped src/dst, and both the IP and ICMP checksums
 * verify to 0. ONE PBUF carries request -> reply (single owner, no alloc), freed
 * exactly once at capture: the leak gate proves it.
 *
 * Scenario 2 (direct call, deterministic): an echo request whose ICMP checksum is
 * corrupt is dropped and counted (icmp_badcksum), PBUF freed. No echo reply is
 * ever produced from a bad-checksum request.
 *
 * Scenario 3 (M2-4, over a CAPTURE device like tstip.c's): an inbound datagram
 * with an unassigned IP protocol demuxes to nsfip_input's existing `noproto`
 * path, which now also calls nsficmp_send_error -- the LIVE v1 trigger
 * (destination unreachable / protocol unreachable). The captured error is
 * decoded byte-for-byte: IP header, ICMP type/code/checksum, and the quoted
 * original IP header + first 8 payload bytes.
 *
 * Scenario 4 (M2-4 suppression, direct calls to nsficmp_send_error): each of the
 * four RFC 1122 §3.2.2 suppression rules is exercised directly against
 * nsficmp_send_error (nsfip_input's own demux would filter three of these four
 * cases -- fragments, non-local destinations, and ICMP -- before ever reaching
 * the noproto trigger, so they cannot be driven end-to-end). The capture device
 * and its route stay wired for these too: if a suppression check were ever
 * broken, the packet would reach nsfip_output and get captured (or, at worst,
 * counted as noroute) -- silence alone is not proof the guard fired.
 *
 * PORTABLE: nsfhost_ops() is NULL on the MVS build (no host driver there); the
 * test then skips scenario 1 and still runs the direct-call checks.
 */
#include "nsficmp.h"
#include "nsfip.h"
#include "nsfcksum.h"
#include "nsfdev.h"
#include "nsfhost.h"
#include "nsfbuf.h"
#include "nsfevt.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define HOME_IP   0x0A010102u       /* 10.1.1.2 -- this stack        */
#define PEER_IP   0x0A010101u       /* 10.1.1.1 -- the remote pinger */

static UINT pool_inuse_small(void);

/* Counter readback: a direct registry read (sts_value) -- robust on the target,
 * where libc370's sscanf does not parse a width-limited "%Ns %Ns %u". */
#define ctr_get_icmp(name)  sts_value("NSFICM", (name))
#define ctr_get_ip(name)    sts_value("NSFIP", (name))

/* ---- byte-wise helpers ------------------------------------------------------- */
static void put16(UCHAR *p, USHORT v) { p[0] = (UCHAR)(v >> 8); p[1] = (UCHAR)v; }
static void put32(UCHAR *p, UINT v)
{
    p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);  p[3] = (UCHAR)v;
}
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

/* Build a complete IPv4 packet (20-byte header, no options + payload) carrying
 * an arbitrary protocol/fragword/addresses, with a valid header checksum.
 * Returns the total length -- the M2-4 tests need protocols and fragment/
 * address combinations build_echo_request below cannot express. */
static USHORT build_ip_raw(UCHAR *buf, USHORT fragword, UCHAR proto,
                           UINT src, UINT dst, const UCHAR *payload, USHORT paylen)
{
    USHORT total = (USHORT)(20u + paylen);

    buf[0] = 0x45u; buf[1] = 0u;         /* version 4, IHL 5, TOS 0 */
    put16(buf + 2, total);
    put16(buf + 4, 0x2222u);            /* id */
    put16(buf + 6, fragword);
    buf[8] = 64u; buf[9] = proto;
    buf[10] = 0u; buf[11] = 0u;         /* checksum (filled below) */
    put32(buf + 12, src);
    put32(buf + 16, dst);
    if (paylen != 0u) {
        memcpy(buf + 20, payload, paylen);
    }
    put16(buf + 10, raw_cksum(buf, 20u));
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

/* ---- capture device (like tstip.c's): copies the outbound packet + frees it -- */
static UCHAR  g_cap[256];
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

/* Build an ICMP echo message with a valid checksum; returns its length. */
static USHORT build_icmp(UCHAR *buf, UCHAR type, USHORT id, USHORT seq,
                         const UCHAR *data, USHORT dlen)
{
    USHORT len = (USHORT)(8u + dlen);
    buf[0] = type; buf[1] = 0u; buf[2] = 0u; buf[3] = 0u;
    put16(buf + 4, id);
    put16(buf + 6, seq);
    if (dlen != 0u) {
        memcpy(buf + 8, data, dlen);
    }
    put16(buf + 2, raw_cksum(buf, len));
    return len;
}

/* Build a complete IPv4 echo-request packet; returns its total length. */
static USHORT build_echo_request(UCHAR *buf, UINT src, UINT dst,
                                 const UCHAR *data, USHORT dlen)
{
    USHORT il    = build_icmp(buf + 20, 8u, 0xABCDu, 7u, data, dlen);
    USHORT total = (USHORT)(20u + il);

    buf[0] = 0x45u; buf[1] = 0u;
    put16(buf + 2, total);
    put16(buf + 4, 0x9999u);            /* id */
    put16(buf + 6, 0u);                 /* no flags / fragment */
    buf[8] = 64u; buf[9] = (UCHAR)NSFIP_PROTO_ICMP;
    buf[10] = 0u; buf[11] = 0u;
    put32(buf + 12, src);
    put32(buf + 16, dst);
    put16(buf + 10, raw_cksum(buf, 20u));
    return total;
}

/* ---- scenario 1: loopback echo, reply captured ------------------------------- */
static int    g_gotreply;
static UCHAR  g_reply[128];
static USHORT g_replylen;

static void h_rx(EVT *ev)
{
    PBUF  *b = (PBUF *)ev->p1;
    NETDEV *d = dev_by_index(ev->u1);
    UCHAR  hdr[64];
    USHORT n = buf_copyout(b, hdr, (USHORT)sizeof(hdr));
    UCHAR  ihl = (UCHAR)((hdr[0] & 0x0Fu) * 4u);

    if (n >= (USHORT)(ihl + 1u) && hdr[9] == (UCHAR)NSFIP_PROTO_ICMP &&
        hdr[ihl] == (UCHAR)NSFICMP_ECHO_REPLY) {
        /* The reply that our responder generated, looped back. Capture + stop. */
        g_replylen = n;
        memcpy(g_reply, hdr, (n < sizeof(g_reply)) ? n : sizeof(g_reply));
        g_gotreply = 1;
        buf_free(b);
        nsfevt_stop();
        return;
    }
    /* The original request: run the real stack (which replies via nsfip_output). */
    nsfip_input(d, b);
}

static void scenario_loopback(DEVOPS *ops)
{
    DEVCFG  cfg;
    NETDEV *dev;
    UCHAR   pkt[80];
    USHORT  total;
    PBUF   *b;
    UCHAR   ihl;

    nsfevt_init();                      /* fresh loop state (clears dev hooks) */
    dev_init();                         /* fresh device table                  */
    nsfip_init();
    nsficmp_init();

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "LOOP0", 5);
    cfg.cuu    = 0x0500;
    cfg.type   = NSFDEV_T_HOST;
    cfg.ipaddr = HOME_IP;
    cfg.mtu    = 1500;
    cfg.drvcfg = NULL;                  /* loopback default */
    dev = dev_register(&cfg, ops);
    CHECK(dev != NULL, "loopback dev_register");

    nsfip_local_add(HOME_IP);
    CHECK(nsfip_route_add(0u, 0u, dev, 0u) == 0, "default route -> loopback");

    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_gotreply = 0;

    CHECK(dev_start(dev) == 0, "dev_start");

    /* Inject the echo request as an outbound frame; the loopback relays it back
     * inbound (data at the IP header, as a driver delivers). */
    total = build_echo_request(pkt, PEER_IP, HOME_IP,
                               (const UCHAR *)"nsf-ping", 8u);
    b = buf_alloc(total);
    CHECK(b != NULL, "buf_alloc for the request");
    (void)buf_copyin(b, pkt, total);
    CHECK(dev_send(dev, b) == 0, "dev_send injected the request");

    evt_mainloop();                     /* runs until h_rx stops it on the reply */

    CHECK(g_gotreply == 1, "an echo reply looped back");
    ihl = (UCHAR)((g_reply[0] & 0x0Fu) * 4u);
    CHECK_EQ((long)g_reply[ihl], (long)NSFICMP_ECHO_REPLY, "reply ICMP type is 0 (echo reply)");
    CHECK_EQ((long)g_reply[ihl + 1u], 0L, "reply ICMP code is 0");
    CHECK_EQ((long)get32(g_reply + 12), (long)HOME_IP, "reply source is our HOME (swapped)");
    CHECK_EQ((long)get32(g_reply + 16), (long)PEER_IP, "reply destination is the pinger (swapped)");
    CHECK_EQ((long)raw_cksum(g_reply, 20u), 0L, "reply IP header checksum verifies to 0");
    CHECK_EQ((long)raw_cksum(g_reply + ihl, (USHORT)(g_replylen - ihl)), 0L,
             "reply ICMP checksum verifies to 0");
    CHECK_EQ((long)ctr_get_icmp("inecho"), 1, "icmp_inecho counted the request");
    CHECK_EQ((long)ctr_get_icmp("outecho"), 1, "icmp_outecho counted the reply");

    CHECK(dev_shutdown(dev) == 0, "dev_shutdown");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after the loopback flow");
}

/* ---- scenario 2: bad ICMP checksum is dropped -------------------------------- */
static void scenario_badcksum(void)
{
    UCHAR   pkt[80];
    USHORT  total;
    PBUF   *b;
    UINT    before;

    nsfip_init();
    nsficmp_init();
    nsfip_local_add(HOME_IP);

    total = build_echo_request(pkt, PEER_IP, HOME_IP, (const UCHAR *)"corrupt!", 8u);
    /* Corrupt the ICMP checksum field (bytes 22..23: ICMP header at offset 20). */
    pkt[22] ^= 0xFFu;

    b = buf_alloc(total);
    CHECK(b != NULL, "buf_alloc for the bad-checksum request");
    buf_reset_rx(b);
    (void)buf_copyin(b, pkt, total);

    before = ctr_get_icmp("badcksum");
    /* dev is only forwarded to nsficmp_input, which ignores it -- pass NULL so
     * this runs with no device (and on the MVS build, no host driver). */
    nsfip_input(NULL, b);              /* validates IP, demuxes to ICMP, drops */
    CHECK_EQ((long)ctr_get_icmp("badcksum"), (long)before + 1, "bad ICMP checksum -> icmp_badcksum");
    CHECK_EQ((long)pool_inuse_small(), 0, "bad-checksum request freed (no leak)");
}

/* ---- scenario 3: an unassigned protocol draws a protocol-unreachable -------- */
static void scenario_send_error(NETDEV *dev)
{
    UCHAR   pkt[64];
    UCHAR   payload[12];
    UCHAR   orighdr[20];
    USHORT  total, icmplen;
    PBUF   *b;
    UINT    i, before_err, before_noproto;

    for (i = 0u; i < sizeof(payload); i++) {
        payload[i] = (UCHAR)(0x30u + i);
    }

    before_err     = ctr_get_icmp("errsent");
    before_noproto = ctr_get_ip("noproto");
    g_capcount     = 0;

    /* Protocol 253 is IANA "use for experimentation" -- guaranteed unassigned,
     * so this exercises the real noproto path, not a TCP/UDP special case. */
    total = build_ip_raw(pkt, 0u, 253u, PEER_IP, HOME_IP, payload, sizeof(payload));
    memcpy(orighdr, pkt, sizeof(orighdr));       /* the header nsfip_input demuxed */

    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "send_error: rx_pbuf allocated");
    nsfip_input(dev, b);                         /* noproto -> nsficmp_send_error */
    nsfdev_kick_output();                        /* drain the error into the capture */

    CHECK_EQ((long)ctr_get_ip("noproto"), (long)before_noproto + 1,
             "unassigned protocol -> ip_noproto");
    CHECK_EQ((long)ctr_get_icmp("errsent"), (long)before_err + 1,
             "protocol-unreachable was sent (icmp_errsent)");
    CHECK_EQ((long)g_capcount, 1, "exactly one error packet transmitted");

    CHECK_EQ((long)(g_cap[0]), 0x45L, "error IP header is version 4 IHL 5");
    CHECK_EQ((long)raw_cksum(g_cap, 20u), 0L, "error IP header checksum verifies to 0");
    CHECK_EQ((long)get32(g_cap + 12), (long)HOME_IP, "error source is our HOME address");
    CHECK_EQ((long)get32(g_cap + 16), (long)PEER_IP, "error destination is the original sender");
    CHECK_EQ((long)(g_cap[9]), (long)NSFIP_PROTO_ICMP, "error IP protocol is ICMP");

    icmplen = (USHORT)(g_caplen - 20u);
    CHECK_EQ((long)g_cap[20], (long)NSFICMP_DEST_UNREACH, "error ICMP type is dest-unreachable");
    CHECK_EQ((long)g_cap[21], (long)NSFICMP_UNREACH_PROTO, "error ICMP code is protocol-unreachable");
    CHECK_EQ((long)raw_cksum(g_cap + 20, icmplen), 0L, "error ICMP checksum verifies to 0");
    CHECK_EQ((long)icmplen, (long)(8u + sizeof(orighdr) + 8u),
             "error message is the 8-byte ICMP header + quoted 20-byte IP header + 8 payload bytes");
    CHECK(memcmp(g_cap + 20 + 8, orighdr, sizeof(orighdr)) == 0,
          "quoted IP header matches the original datagram");
    CHECK(memcmp(g_cap + 20 + 8 + (long)sizeof(orighdr), payload, 8u) == 0,
          "quoted first 8 bytes of payload match the original datagram");

    CHECK_EQ((long)pool_inuse_small(), 0, "send_error leaked nothing (orig + error PBUFs both freed)");
}

/* ---- scenario 4: RFC 1122 suppression, direct calls -------------------------- *
 * nsfip_input's own demux filters fragments/non-local-dest/ICMP before the
 * noproto trigger, so these four rules are driven directly against
 * nsficmp_send_error -- exactly the function under test (spec 11.2). The
 * capture device + route from scenario_send_error's caller stay wired: if a
 * guard were broken, the packet would reach nsfip_output and get captured
 * (proving the assertion means something, not just that nothing happened to
 * happen). */
static void suppress_case(const char *label, USHORT fragword, UCHAR proto,
                          UINT src, UINT dst)
{
    UCHAR   pkt[64];
    UCHAR   payload[12];
    USHORT  total;
    PBUF   *b;
    UINT    i, before_err;

    for (i = 0u; i < sizeof(payload); i++) {
        payload[i] = (UCHAR)(0x40u + i);
    }
    total = build_ip_raw(pkt, fragword, proto, src, dst, payload, sizeof(payload));
    b = rx_pbuf(pkt, total);
    CHECK(b != NULL, label);

    before_err = ctr_get_icmp("errsent");
    g_capcount = 0;
    nsficmp_send_error(b, (UCHAR)NSFICMP_DEST_UNREACH, (UCHAR)NSFICMP_UNREACH_PROTO);
    CHECK_EQ((long)g_capcount, 0, label);
    CHECK_EQ((long)ctr_get_icmp("errsent"), (long)before_err, label);

    buf_free(b);                        /* send_error only READS orig; we own it */
}

static void scenario_suppression(void)
{
    UCHAR err_orig[8];

    /* (a) the original is itself an ICMP error message. */
    {
        UCHAR   pkt[64];
        USHORT  total;
        PBUF   *b;
        UINT    before_err;

        memset(err_orig, 0, sizeof(err_orig));
        err_orig[0] = (UCHAR)NSFICMP_DEST_UNREACH;    /* type 3: an error itself */
        total = build_ip_raw(pkt, 0u, (UCHAR)NSFIP_PROTO_ICMP, PEER_IP, HOME_IP,
                             err_orig, sizeof(err_orig));
        b = rx_pbuf(pkt, total);
        CHECK(b != NULL, "suppression(error-on-error): rx_pbuf allocated");

        before_err = ctr_get_icmp("errsent");
        g_capcount = 0;
        nsficmp_send_error(b, (UCHAR)NSFICMP_DEST_UNREACH, (UCHAR)NSFICMP_UNREACH_PROTO);
        CHECK_EQ((long)g_capcount, 0, "no error generated for an ICMP-error original");
        CHECK_EQ((long)ctr_get_icmp("errsent"), (long)before_err,
                 "errsent unchanged (error-on-error)");
        buf_free(b);
    }

    /* (b) broadcast / multicast destination. */
    suppress_case("suppression(broadcast dst): no error generated",
                  0u, 253u, PEER_IP, 0xFFFFFFFFu);
    suppress_case("suppression(multicast dst): no error generated",
                  0u, 253u, PEER_IP, 0xE0000001u /* 224.0.0.1 */);

    /* (c) non-initial fragment (offset != 0, MF clear). */
    suppress_case("suppression(non-initial fragment): no error generated",
                  0x0002u, 253u, PEER_IP, HOME_IP);

    /* (d) source does not identify a single host: zero, broadcast, multicast. */
    suppress_case("suppression(zero source): no error generated",
                  0u, 253u, 0x00000000u, HOME_IP);
    suppress_case("suppression(broadcast source): no error generated",
                  0u, 253u, 0xFFFFFFFFu, HOME_IP);
    suppress_case("suppression(multicast source): no error generated",
                  0u, 253u, 0xE0000001u, HOME_IP);

    CHECK_EQ((long)pool_inuse_small(), 0, "suppression cases leaked nothing");
}

/* ---- pool helper ------------------------------------------------------------- */
static UINT pool_inuse_small(void)
{
#if NSF_DEBUG
    MMSTATS s;
    mm_stats(buf_debug_pool(NSFBUF_CLASS_SMALL), &s);
    return s.inuse;
#else
    return 0u;
#endif
}

int main(void)
{
    DEVOPS *ops;

    printf("=== nsf370 NSFICM echo responder tests ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init");
    CHECK(buf_init() == 0, "buf_init");
    mm_init_complete();

    ops = nsfhost_ops();
    if (ops != NULL) {
        scenario_loopback(ops);
    } else {
        printf("host driver unavailable -- skipping the loopback scenario\n");
    }

    scenario_badcksum();

    /* Scenarios 3+4 (M2-4): a fresh capture device + default route, matching
     * tstip.c's pattern. dev_init needs nsfevt_init to have just run (it clears
     * the loop's device hooks); re-run it fresh here as scenario_loopback does. */
    {
        DEVCFG  cfg;
        NETDEV *dev;

        CHECK(nsfevt_init() == 0, "nsfevt_init (scenario 3/4 setup)");
        dev_init();
        nsfip_init();
        nsficmp_init();

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

        scenario_send_error(dev);
        scenario_suppression();

        CHECK(dev_shutdown(dev) == 0, "dev_shutdown (capture)");
    }

    CHECK_EQ((long)pool_inuse_small(), 0, "BUFSMALL at baseline (no leak)");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline (no leak)");

    mm_shutdown();
    return mbt_test_summary("TSTICMP");
}
