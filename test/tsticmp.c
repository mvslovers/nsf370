/*
 * tsticmp.c -- NSFICM echo responder host tests (spec ch. 11, M2-3).
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

static UINT ctr_get_icmp(const char *name);
static UINT pool_inuse_small(void);

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

/* ---- counter / pool helpers -------------------------------------------------- */
static UINT ctr_get_icmp(const char *name)
{
    static char buf[8192];
    char *line;

    (void)sts_render(buf, (UINT)sizeof(buf));
    line = buf;
    while (*line != '\0') {
        char  c[16], nm[16];
        unsigned v;
        char *nl = strchr(line, '\n');

        if (nl != NULL) {
            *nl = '\0';
        }
        if (sscanf(line, "%15s %15s %u", c, nm, &v) == 3 &&
            strcmp(c, "NSFICM") == 0 && strcmp(nm, name) == 0) {
            return (UINT)v;
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    return 0u;
}

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

    CHECK_EQ((long)pool_inuse_small(), 0, "BUFSMALL at baseline (no leak)");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline (no leak)");

    mm_shutdown();
    return mbt_test_summary("TSTICMP");
}
