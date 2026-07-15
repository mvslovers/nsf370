/*
 * tstudp.c -- NSFUDP host unit tests (spec ch. 12, M3-3).
 *
 * UDP is where sockets become reachable end to end. The functional coverage is
 * DIRECT-CALL over the M3-2 request path (build an NSFRQE, nsfreq_dispatch it --
 * the executive side, no loop, no threads) with a CAPTURE device for outbound
 * packets, exactly the tstip/tstreq pattern -- fully repeatable, exact byte and
 * counter assertions:
 *
 *   - checksum: the OUTPUT path emits the independently-computed literal value
 *     (SENDTO of the 10.1.1.2->10.1.1.1 vector -> UDP checksum 0x9371 on the
 *     wire); a computed-zero checksum is transmitted as 0xFFFF (RFC 768); an
 *     INPUT datagram with a good checksum is delivered, a corrupt one dropped +
 *     counted, and a received checksum 0 accepted unverified;
 *   - bind/demux: exact match, INADDR_ANY match, specific-beats-ANY, duplicate
 *     (lport,laddr) -> EADDRINUSE, port 0 -> an ephemeral port;
 *   - input: datagram -> rxq -> RECVFROM returns the payload + the correct peer
 *     addr/port; a parked RECV is completed immediately on arrival; rxq full ->
 *     drop + count; an oversized datagram is TRUNCATED to the RECV buffer;
 *   - port unreachable: a datagram to an unbound port draws ICMP type 3 code 3
 *     quoting the original, with the datagram freed exactly once (leak gate);
 *   - SENDTO: the emitted UDP+IP framing is asserted byte-wise; PBUF freed once;
 *   - lifetime: soc_destroy (via RQ_CLOSE) runs udp_detach (frees the UDPPCB),
 *     flushes rxq PBUFs, completes a parked RECV with ECONNABORTED, and returns
 *     the SOCKET / UDPPCB / BUF pools to baseline.
 *
 * The reference UDP checksum in this test is a SEPARATE local one's-complement
 * routine (ref_udp_cksum, not in_cksum), so an independently-built valid
 * datagram that NSFUDP then rejects -- or vice versa -- fails the test: the two
 * implementations must agree, which is the real pin (the literal 0x9371 output
 * assertion pins the absolute value).
 *
 * HOST-ONLY (#ifndef __MVS__): the real threaded round-trip over the NSFHOST
 * loopback + a running event loop -- an app RECEIVER thread blocks in RECVFROM
 * (parks on the executive) while a SENDER thread's SENDTO loops back through the
 * driver, reaches udp_input on the executive, and completes the parked RECV;
 * the receiver wakes exactly once. Repeated under load for the #27 lost-request
 * / lost-wakeup gate (200x sequential + 100x single-core on Linux).
 */
#include "nsfudp.h"
#include "nsfreq.h"
#include "nsfsoc.h"
#include "nsfip.h"
#include "nsficmp.h"
#include "nsfcksum.h"
#include "nsfdev.h"
#include "nsfhost.h"
#include "nsfbuf.h"
#include "nsfevt.h"
#include "nsfevtp.h"            /* NSFECB_POSTED */
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include "nsftrc.h"
#include "nsfthr.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>
#ifndef __MVS__
#include <pthread.h>
#endif

#define HOME_IP   0x0A010102u       /* 10.1.1.2 -- this stack               */
#define PEER_IP   0x0A010101u       /* 10.1.1.1 -- the point-to-point peer   */

#define ctr_get(comp, name)  sts_value((comp), (name))

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

/* IP header checksum via the trusted in_cksum (already pinned in tstcksum). */
static USHORT raw_cksum(const UCHAR *p, USHORT len)
{
    PBUF tp;
    memset(&tp, 0, sizeof(tp));
    tp.data = (UCHAR *)p;
    tp.len  = len;
    return in_cksum(&tp, 0u, len);
}

/* Reference UDP checksum: an INDEPENDENT local one's-complement sum over the
 * pseudo-header + UDP header (checksum 0) + payload -- NOT in_cksum -- so a
 * valid datagram this test builds and NSFUDP rejects (or the reverse) fails the
 * test. Returns the raw complement; the caller applies RFC 768's 0 -> 0xFFFF. */
static USHORT ref_udp_cksum(UINT src, UINT dst, USHORT sport, USHORT dport,
                            const UCHAR *pay, USHORT paylen)
{
    UCHAR  hdr[20];             /* 12-byte pseudo-header + 8-byte UDP header */
    USHORT udplen = (USHORT)(8u + paylen);
    UINT   sum = 0u;
    USHORT i;

    put32(hdr + 0, src);
    put32(hdr + 4, dst);
    hdr[8] = 0u; hdr[9] = 17u; put16(hdr + 10, udplen);   /* pseudo-header */
    put16(hdr + 12, sport); put16(hdr + 14, dport);
    put16(hdr + 16, udplen); put16(hdr + 18, 0u);         /* UDP header, cksum 0 */

    for (i = 0u; i < 20u; i += 2u) {
        sum += ((UINT)hdr[i] << 8) | hdr[i + 1u];
    }
    for (i = 0u; i < paylen; i += 2u) {
        UINT hi = pay[i];
        UINT lo = (USHORT)(i + 1u) < paylen ? pay[i + 1u] : 0u;
        sum += (hi << 8) | lo;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (USHORT)(~sum & 0xFFFFu);
}

/* Build a complete IPv4+UDP datagram with a valid IP header checksum and the
 * reference UDP checksum (RFC 768 0 -> 0xFFFF). Returns the total IP length. The
 * UDP checksum field sits at offset 26 (IP 20 + UDP offset 6). */
static USHORT build_udp(UCHAR *buf, UINT src, UINT dst, USHORT sport, USHORT dport,
                        const UCHAR *pay, USHORT paylen)
{
    USHORT udplen = (USHORT)(8u + paylen);
    USHORT total  = (USHORT)(20u + udplen);
    USHORT ck;

    buf[0] = 0x45u; buf[1] = 0u; put16(buf + 2, total);
    put16(buf + 4, 0x4321u); put16(buf + 6, 0u);
    buf[8] = 64u; buf[9] = 17u; buf[10] = 0u; buf[11] = 0u;
    put32(buf + 12, src); put32(buf + 16, dst);
    put16(buf + 10, raw_cksum(buf, 20u));

    put16(buf + 20, sport); put16(buf + 22, dport);
    put16(buf + 24, udplen); put16(buf + 26, 0u);
    if (paylen != 0u) {
        memcpy(buf + 28, pay, paylen);
    }
    ck = ref_udp_cksum(src, dst, sport, dport, pay, paylen);
    if (ck == 0u) {
        ck = 0xFFFFu;
    }
    put16(buf + 26, ck);
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

/* ---- capture device (copies the outbound packet + frees it) ------------------ */
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

/* ---- request-path helpers (direct-call, executive side) --------------------- */
static void rqe_init(NSFRQE *r, UINT fn, UINT desc, UINT flags)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->flags    = (USHORT)flags;
    r->sockdesc = desc;
}

static int posted(const NSFRQE *r) { return (r->ecb & NSFECB_POSTED) != 0u; }

static UINT g_apptok;               /* one app instance for the whole suite */

/* INITAPI once. */
static void app_init(void)
{
    NSFRQE r;
    rqe_init(&r, RQ_INITAPI, 0u, 0u);
    nsfreq_dispatch(&r);
    CHECK(posted(&r) && r.retcode == NSF_RETOK, "INITAPI");
    g_apptok = r.apptok;
}

/* SOCKET(AF_INET, SOCK_DGRAM, 17) -> descriptor (0 on failure). */
static UINT udp_socket(void)
{
    NSFRQE r;
    rqe_init(&r, RQ_SOCKET, 0u, 0u);
    r.apptok = g_apptok; r.p1 = NSF_AF_INET; r.p2 = NSF_SOCK_DGRAM; r.p3 = 17u;
    nsfreq_dispatch(&r);
    return (r.retcode >= 0) ? (UINT)r.retcode : 0u;
}

/* BIND(desc, addr, port) -> errno (0 on success). */
static int udp_bind_req(UINT desc, UINT addr, UINT port)
{
    NSFRQE r;
    rqe_init(&r, RQ_BIND, desc, 0u);
    r.p1 = addr; r.p2 = port;
    nsfreq_dispatch(&r);
    return r.errno_;
}

static void udp_close(UINT desc)
{
    NSFRQE r;
    rqe_init(&r, RQ_CLOSE, desc, 0u);
    nsfreq_dispatch(&r);
}

/* Non-blocking RECVFROM into `buf`: returns the retcode; fills from + fport. */
static INT udp_recvfrom_nb(UINT desc, void *buf, UINT len, UINT *from, UINT *fport)
{
    NSFRQE r;
    rqe_init(&r, RQ_RECVFROM, desc, RQ_F_NONBLOCK);
    r.ubuf = buf; r.ulen = len;
    nsfreq_dispatch(&r);
    if (from  != NULL) { *from  = r.p1; }
    if (fport != NULL) { *fport = r.p2; }
    return r.retcode;
}

/* SENDTO(desc, dst:dport, payload) -> retcode. The datagram lands on the
 * device sendq; with the loop not running the test drains it explicitly (the
 * tstip pattern) so the capture device sees it. */
static INT udp_sendto(UINT desc, UINT dst, UINT dport, const void *pay, UINT len)
{
    NSFRQE r;
    rqe_init(&r, RQ_SENDTO, desc, 0u);
    r.ubuf = (void *)pay; r.ulen = len; r.p1 = dst; r.p2 = dport;
    nsfreq_dispatch(&r);
    nsfdev_kick_output();               /* flush the sendq into the capture */
    return r.retcode;
}

static UINT udp_pcb_inuse(void)
{
#if NSF_DEBUG
    return nsfudp_debug_inuse();
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

/* Inject a datagram inbound through the real IP path (dst must be local). */
static void inject(NETDEV *dev, const UCHAR *pkt, USHORT total)
{
    PBUF *b = rx_pbuf(pkt, total);
    CHECK(b != NULL, "inject: rx_pbuf allocated");
    nsfip_input(dev, b);
}

/* ==========================================================================
 * Direct-call functional tests (portable).
 * ========================================================================== */

/* ---- OUTPUT checksum: the literal 0x9371 vector + the zero->0xFFFF rule ------ */
static void test_output_checksum(NETDEV *dev)
{
    /* The tstcksum vector: src 10.1.1.2, dst 10.1.1.1, sport 0x1234, dport
     * 0x0035, payload {68 65 6C 6C 6F 21} -> UDP checksum 0x9371. Bind the
     * sender to HOME:0x1234 so the emitted source addr/port match the vector. */
    static const UCHAR pay[6] = { 0x68u, 0x65u, 0x6Cu, 0x6Cu, 0x6Fu, 0x21u };
    static const UCHAR zpay[2] = { 0xD7u, 0x6Cu };   /* computed cksum 0x0000 */
    UINT desc;

    (void)dev;                          /* SENDTO routes internally (no inject)  */
    desc = udp_socket();
    CHECK(desc != 0u, "output: socket");
    CHECK_EQ((long)udp_bind_req(desc, HOME_IP, 0x1234u), 0L, "output: bind HOME:0x1234");

    g_capcount = 0;
    CHECK(udp_sendto(desc, PEER_IP, 0x0035u, pay, 6u) == 6L, "SENDTO returns byte count 6");
    CHECK_EQ((long)g_capcount, 1L, "SENDTO transmitted one packet");

    /* IP framing */
    CHECK_EQ((long)g_cap[0], 0x45L, "emitted IP version 4 IHL 5");
    CHECK_EQ((long)g_cap[9], 17L, "emitted IP protocol UDP (17)");
    CHECK_EQ((long)raw_cksum(g_cap, 20u), 0L, "emitted IP header verifies to 0");
    CHECK_EQ((long)get32(g_cap + 12), (long)HOME_IP, "emitted source address HOME");
    CHECK_EQ((long)get32(g_cap + 16), (long)PEER_IP, "emitted dest address PEER");
    /* UDP framing */
    CHECK_EQ((long)get16(g_cap + 20), 0x1234L, "emitted UDP source port");
    CHECK_EQ((long)get16(g_cap + 22), 0x0035L, "emitted UDP dest port");
    CHECK_EQ((long)get16(g_cap + 24), 14L, "emitted UDP length (8 + 6)");
    CHECK_EQ((long)get16(g_cap + 26), 0x9371L, "emitted UDP checksum == 0x9371 (literal vector)");

    /* A payload whose computed checksum is 0x0000 must go out as 0xFFFF. */
    g_capcount = 0;
    CHECK(udp_sendto(desc, PEER_IP, 0x0035u, zpay, 2u) == 2L, "SENDTO zero-cksum payload");
    CHECK_EQ((long)g_capcount, 1L, "zero-cksum SENDTO transmitted");
    CHECK_EQ((long)get16(g_cap + 26), 0xFFFFL,
             "computed 0x0000 checksum transmitted as 0xFFFF (RFC 768)");

    udp_close(desc);
    CHECK_EQ((long)small_inuse(), 0L, "output checksum test: no leak");
}

/* ---- INPUT checksum: good delivered, corrupt dropped, zero accepted ---------- */
static void test_input_checksum(NETDEV *dev)
{
    UCHAR  pkt[64];
    UCHAR  out[16];
    USHORT total;
    UINT   desc, from = 0u, fport = 0u;
    UINT   bad0;

    desc = udp_socket();
    CHECK(desc != 0u, "input cksum: socket");
    CHECK_EQ((long)udp_bind_req(desc, HOME_IP, 7000u), 0L, "input cksum: bind HOME:7000");

    /* good checksum -> delivered (NSFUDP's verify agrees with the reference) */
    total = build_udp(pkt, PEER_IP, HOME_IP, 40000u, 7000u, (const UCHAR *)"abc", 3u);
    inject(dev, pkt, total);
    CHECK_EQ((long)udp_recvfrom_nb(desc, out, sizeof(out), &from, &fport), 3L,
             "good-checksum datagram delivered (3 bytes)");
    CHECK(from == PEER_IP && fport == 40000u, "peer addr/port from the datagram");

    /* corrupt checksum -> dropped + counted, nothing delivered */
    bad0 = ctr_get("NSFUDP", "badcksum");
    total = build_udp(pkt, PEER_IP, HOME_IP, 40000u, 7000u, (const UCHAR *)"abc", 3u);
    pkt[27] ^= 0xFFu;                    /* flip the low checksum byte */
    inject(dev, pkt, total);
    CHECK_EQ((long)ctr_get("NSFUDP", "badcksum"), (long)bad0 + 1, "corrupt checksum -> badcksum");
    CHECK_EQ((long)udp_recvfrom_nb(desc, out, sizeof(out), NULL, NULL),
             (long)NSF_RETERR, "corrupt datagram not delivered (RECV empty)");

    /* checksum 0 in the datagram -> accepted unverified (RFC 768) */
    total = build_udp(pkt, PEER_IP, HOME_IP, 40000u, 7000u, (const UCHAR *)"de", 2u);
    put16(pkt + 26, 0x0000u);            /* sender computed no checksum */
    inject(dev, pkt, total);
    CHECK_EQ((long)udp_recvfrom_nb(desc, out, sizeof(out), NULL, NULL), 2L,
             "received checksum 0 accepted without verification");

    udp_close(desc);
    CHECK_EQ((long)small_inuse(), 0L, "input checksum test: no leak");
}

/* ---- bind / demux ----------------------------------------------------------- */
static void test_bind_demux(NETDEV *dev)
{
    UCHAR  pkt[48];
    UCHAR  out[16];
    USHORT total;
    UINT   sExact, sAny, sSpec, sDup, sEph;
    INT    n;
    NSFRQE gsn;

    /* exact match: HOME:8000 gets a datagram to HOME:8000 */
    sExact = udp_socket();
    CHECK_EQ((long)udp_bind_req(sExact, HOME_IP, 8000u), 0L, "bind exact HOME:8000");
    total = build_udp(pkt, PEER_IP, HOME_IP, 41000u, 8000u, (const UCHAR *)"x", 1u);
    inject(dev, pkt, total);
    CHECK_EQ((long)udp_recvfrom_nb(sExact, out, sizeof(out), NULL, NULL), 1L,
             "exact (lport,laddr) match delivered");

    /* INADDR_ANY match: ANY:8001 gets a datagram to HOME:8001 */
    sAny = udp_socket();
    CHECK_EQ((long)udp_bind_req(sAny, 0u /* INADDR_ANY */, 8001u), 0L, "bind ANY:8001");
    total = build_udp(pkt, PEER_IP, HOME_IP, 41000u, 8001u, (const UCHAR *)"y", 1u);
    inject(dev, pkt, total);
    CHECK_EQ((long)udp_recvfrom_nb(sAny, out, sizeof(out), NULL, NULL), 1L,
             "INADDR_ANY match delivered");

    /* specific-beats-ANY: bind BOTH ANY:8002 and HOME:8002; a datagram to
     * HOME:8002 must land on the specific socket, not the ANY one. */
    sSpec = udp_socket();
    CHECK_EQ((long)udp_bind_req(sSpec, HOME_IP, 8002u), 0L, "bind HOME:8002 (specific)");
    {
        UINT sAny2 = udp_socket();
        CHECK_EQ((long)udp_bind_req(sAny2, 0u, 8002u), 0L, "bind ANY:8002 alongside");
        total = build_udp(pkt, PEER_IP, HOME_IP, 41000u, 8002u, (const UCHAR *)"z", 1u);
        inject(dev, pkt, total);
        CHECK_EQ((long)udp_recvfrom_nb(sSpec, out, sizeof(out), NULL, NULL), 1L,
                 "specific laddr beats INADDR_ANY");
        CHECK_EQ((long)udp_recvfrom_nb(sAny2, out, sizeof(out), NULL, NULL),
                 (long)NSF_RETERR, "the ANY socket did NOT get it");
        udp_close(sAny2);
    }

    /* duplicate (lport,laddr) -> EADDRINUSE */
    sDup = udp_socket();
    CHECK_EQ((long)udp_bind_req(sDup, HOME_IP, 8000u), (long)NSF_EADDRINUSE,
             "duplicate (HOME,8000) -> EADDRINUSE");
    udp_close(sDup);

    /* port 0 -> an ephemeral port in the dynamic range, visible via GETSOCKNAME */
    sEph = udp_socket();
    CHECK_EQ((long)udp_bind_req(sEph, HOME_IP, 0u), 0L, "bind HOME:0 (ephemeral)");
    rqe_init(&gsn, RQ_GETSOCKNAME, sEph, 0u);
    nsfreq_dispatch(&gsn);
    n = (INT)gsn.p2;
    CHECK(n >= (INT)NSFUDP_EPHEM_LO && n <= (INT)NSFUDP_EPHEM_HI,
          "ephemeral port is in the dynamic range");

    udp_close(sExact);
    udp_close(sAny);
    udp_close(sSpec);
    udp_close(sEph);
    CHECK_EQ((long)soc_count(), 0L, "bind/demux: all sockets closed");
    CHECK_EQ((long)udp_pcb_inuse(), 0L, "bind/demux: UDPPCB pool at baseline");
    CHECK_EQ((long)small_inuse(), 0L, "bind/demux: no buffer leak");
}

/* ---- input -> rxq / parked RECV / rxq full / truncation --------------------- */
static void test_input_paths(NETDEV *dev)
{
    UCHAR  pkt[1600];
    UCHAR  out[256];
    UCHAR  big[100];
    USHORT total;
    UINT   desc, from = 0u, fport = 0u, i;
    NSFRQE park;

    desc = udp_socket();
    CHECK_EQ((long)udp_bind_req(desc, HOME_IP, 9000u), 0L, "input: bind HOME:9000");

    /* queued -> RECVFROM returns payload + peer */
    total = build_udp(pkt, PEER_IP, HOME_IP, 42000u, 9000u, (const UCHAR *)"hello", 5u);
    inject(dev, pkt, total);
    CHECK_EQ((long)udp_recvfrom_nb(desc, out, sizeof(out), &from, &fport), 5L,
             "queued datagram -> RECVFROM 5 bytes");
    CHECK(from == PEER_IP && fport == 42000u, "RECVFROM peer addr/port correct");
    CHECK(memcmp(out, "hello", 5) == 0, "RECVFROM payload correct");

    /* parked RECV completed immediately on arrival: a BLOCKING RECVFROM on an
     * empty rxq parks; the next injected datagram completes it. */
    rqe_init(&park, RQ_RECVFROM, desc, 0u);
    park.ubuf = out; park.ulen = sizeof(out);
    nsfreq_dispatch(&park);
    CHECK(!posted(&park), "blocking RECVFROM on empty rxq parked (not posted)");
    total = build_udp(pkt, PEER_IP, HOME_IP, 42001u, 9000u, (const UCHAR *)"wake", 4u);
    inject(dev, pkt, total);
    CHECK(posted(&park), "parked RECVFROM completed on datagram arrival");
    CHECK_EQ((long)park.retcode, 4L, "parked RECVFROM delivered 4 bytes");
    CHECK(park.p1 == PEER_IP && park.p2 == 42001u, "parked RECVFROM peer correct");
    CHECK(memcmp(out, "wake", 4) == 0, "parked RECVFROM payload correct");

    /* rxq full -> drop + count. Fill NSFSOC_RXQ_MAX, then one more is dropped. */
    {
        UINT rxfull0 = ctr_get("NSFUDP", "rxfull");
        for (i = 0u; i < (UINT)NSFSOC_RXQ_MAX; i++) {
            total = build_udp(pkt, PEER_IP, HOME_IP, 42002u, 9000u, (const UCHAR *)"q", 1u);
            inject(dev, pkt, total);
        }
        total = build_udp(pkt, PEER_IP, HOME_IP, 42002u, 9000u, (const UCHAR *)"q", 1u);
        inject(dev, pkt, total);        /* the (MAX+1)th -> dropped */
        CHECK_EQ((long)ctr_get("NSFUDP", "rxfull"), (long)rxfull0 + 1,
                 "rxq full -> drop + count");
        /* drain the queue so the socket closes clean */
        for (i = 0u; i < (UINT)NSFSOC_RXQ_MAX; i++) {
            (void)udp_recvfrom_nb(desc, out, sizeof(out), NULL, NULL);
        }
        CHECK_EQ((long)udp_recvfrom_nb(desc, out, sizeof(out), NULL, NULL),
                 (long)NSF_RETERR, "rxq drained empty after MAX reads");
    }

    /* oversized datagram truncated to the RECV buffer (datagram semantics) */
    for (i = 0u; i < sizeof(big); i++) {
        big[i] = (UCHAR)i;
    }
    total = build_udp(pkt, PEER_IP, HOME_IP, 42003u, 9000u, big, (USHORT)sizeof(big));
    inject(dev, pkt, total);
    {
        INT n = udp_recvfrom_nb(desc, out, 10u, NULL, NULL);      /* only 10 bytes wanted */
        CHECK_EQ((long)n, 10L, "oversized datagram truncated to the RECV buffer (10)");
        CHECK(memcmp(out, big, 10) == 0, "truncated read holds the first 10 bytes");
    }
    /* the remainder was discarded, not kept for the next read */
    CHECK_EQ((long)udp_recvfrom_nb(desc, out, sizeof(out), NULL, NULL), (long)NSF_RETERR,
             "datagram remainder discarded (no second read)");

    udp_close(desc);
    CHECK_EQ((long)small_inuse(), 0L, "input paths: no buffer leak");
}

/* ---- port unreachable ------------------------------------------------------- */
static void test_port_unreach(NETDEV *dev)
{
    UCHAR  pkt[48];
    USHORT total;
    UINT   noport0, errsent0;
    UCHAR  hlen, itype, icode;

    noport0  = ctr_get("NSFUDP", "noport");
    errsent0 = ctr_get("NSFICM", "errsent");
    g_capcount = 0;

    /* No socket bound to HOME:9999 -> ICMP port unreachable. */
    total = build_udp(pkt, PEER_IP, HOME_IP, 43000u, 9999u, (const UCHAR *)"nobody", 6u);
    inject(dev, pkt, total);            /* udp_input owns + frees the datagram   */
    nsfdev_kick_output();               /* drain the emitted ICMP error          */

    CHECK_EQ((long)ctr_get("NSFUDP", "noport"), (long)noport0 + 1, "unbound port -> udp noport");
    CHECK_EQ((long)ctr_get("NSFICM", "errsent"), (long)errsent0 + 1, "ICMP error sent");
    CHECK_EQ((long)g_capcount, 1L, "one ICMP error captured");

    /* The captured error: IP proto ICMP, type 3 code 3, quoting the original IP
     * header (the errored datagram's src/dst). */
    CHECK_EQ((long)g_cap[9], (long)NSFIP_PROTO_ICMP, "error is an ICMP packet");
    hlen  = (UCHAR)((g_cap[0] & 0x0Fu) * 4u);
    itype = g_cap[hlen];
    icode = g_cap[hlen + 1u];
    CHECK_EQ((long)itype, (long)NSFICMP_DEST_UNREACH, "ICMP type 3 (dest unreachable)");
    CHECK_EQ((long)icode, (long)NSFICMP_UNREACH_PORT, "ICMP code 3 (port unreachable)");
    /* quoted original IP header begins at hlen + 8 (type/code/cksum/unused). */
    CHECK_EQ((long)get32(g_cap + hlen + 8u + 12u), (long)PEER_IP,
             "quoted original source address");
    CHECK_EQ((long)get32(g_cap + hlen + 8u + 16u), (long)HOME_IP,
             "quoted original destination address");
    CHECK_EQ((long)g_cap[hlen + 8u + 9u], 17L, "quoted original protocol UDP");

    CHECK_EQ((long)small_inuse(), 0L, "port unreach: original datagram freed exactly once");
}

/* ---- lifetime: soc_destroy frees the pcb + rxq + completes a parked RECV ----- */
static void test_lifetime(NETDEV *dev)
{
    UCHAR  pkt[48];
    USHORT total;
    UINT   desc, sbase, pbase, bbase;
    NSFRQE park;

    sbase = sock_inuse();
    pbase = udp_pcb_inuse();
    bbase = small_inuse();

    desc = udp_socket();
    CHECK_EQ((long)udp_bind_req(desc, HOME_IP, 9500u), 0L, "lifetime: bind");
#if NSF_DEBUG
    CHECK_EQ((long)udp_pcb_inuse(), (long)pbase + 1, "bind allocated a UDPPCB");
#endif

    /* queue two datagrams (never received) ... */
    total = build_udp(pkt, PEER_IP, HOME_IP, 44000u, 9500u, (const UCHAR *)"aa", 2u);
    inject(dev, pkt, total);
    total = build_udp(pkt, PEER_IP, HOME_IP, 44000u, 9500u, (const UCHAR *)"bb", 2u);
    inject(dev, pkt, total);

    /* ... plus a parked blocking RECV (a SECOND socket, so its rxq is empty). */
    {
        UINT desc2 = udp_socket();
        CHECK_EQ((long)udp_bind_req(desc2, HOME_IP, 9501u), 0L, "lifetime: bind 2");
        rqe_init(&park, RQ_RECVFROM, desc2, 0u);
        park.ubuf = pkt; park.ulen = sizeof(pkt);
        nsfreq_dispatch(&park);
        CHECK(!posted(&park), "lifetime: blocking RECV parked");

        /* CLOSE desc2 -> soc_destroy completes the parked RECV with ECONNABORTED */
        udp_close(desc2);
        CHECK(posted(&park), "CLOSE completed the parked RECV");
        CHECK_EQ((long)park.errno_, (long)NSF_ECONNABORTED, "parked RECV -> ECONNABORTED");
    }

    /* CLOSE desc -> udp_detach frees the pcb, soc_destroy flushes the rxq PBUFs */
    udp_close(desc);
    CHECK_EQ((long)soc_count(), 0L, "lifetime: all sockets closed");
    CHECK_EQ((long)udp_pcb_inuse(), (long)pbase, "lifetime: UDPPCB pool back to baseline");
    CHECK_EQ((long)sock_inuse(), (long)sbase, "lifetime: SOCKET pool back to baseline");
    CHECK_EQ((long)small_inuse(), (long)bbase, "lifetime: rxq PBUFs freed (BUF baseline)");
}

/* ==========================================================================
 * HOST-ONLY threaded round-trip over the NSFHOST loopback (#27 gate).
 * ========================================================================== */
#ifndef __MVS__

/* EV_PACKET_RECEIVED terminus: the real IP path. */
static void loop_rx(EVT *ev)
{
    PBUF   *b   = (PBUF *)ev->p1;
    NETDEV *dev = dev_by_index(ev->u1);
    if (b != NULL) {
        nsfip_input(dev, b);
    }
}

static NETDEV *g_loopdev;

/* Fresh loop + device + socket/req/udp layer, over the NSFHOST loopback. */
static void setup_loop(void)
{
    DEVCFG cfg;

    nsfevt_init();                      /* resets the loop seams */
    dev_init();
    nsfip_init();
    nsficmp_init();
    soc_init();
    nsfreq_init();
    nsfudp_init();
    CHECK_EQ((long)nsfreq_register_proto(17u, nsfudp_protops()), 0L, "register UDP proto");

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "LOOP0", 5);
    cfg.cuu = 0x0600; cfg.type = NSFDEV_T_HOST; cfg.ipaddr = HOME_IP; cfg.mtu = 1500;
    g_loopdev = dev_register(&cfg, nsfhost_ops());
    CHECK(g_loopdev != NULL, "loopback dev_register");
    nsfip_local_add(HOME_IP);
    CHECK_EQ((long)nsfip_route_add(0u, 0u, g_loopdev, 0u), 0L, "default route -> loopback");
    evt_register(EV_PACKET_RECEIVED, loop_rx);
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);
    CHECK_EQ((long)dev_start(g_loopdev), 0L, "loopback dev_start");
}

static void *exec_thread(void *arg) { (void)arg; evt_mainloop(); return NULL; }

/* The round-trip: RECEIVER blocks in RECVFROM (parks on the executive) while
 * SENDER's SENDTO loops back through the driver, reaches udp_input, and
 * completes the parked RECV. Repeated `iters` times per run.
 *
 * FLOW CONTROL (deliberate). The sender and receiver are LOCK-STEPPED to exactly
 * ONE datagram in flight: the sender waits until the receiver consumes datagram
 * k before sending k+1. Without this the sender -- whose SENDTO completes as soon
 * as the datagram is queued on the device sendq, not when it is transmitted --
 * races ahead and bursts up to `iters` datagrams into the bounded pipeline
 * (device sendq 32, socket rxq 32); §3 bounded queues DROP under overload by
 * design, and a single legitimate drop would strand a parked RECVFROM forever.
 * Lock-step keeps every stage at depth <=1, so the gate proves the wakeup path
 * (a datagram completing a parked RECV, no loss, no lost wakeup) rather than the
 * stack's overload-drop policy. */
#define RT_PORT_R   20000u
#define RT_PORT_S   20001u

static volatile int    g_rt_iters;
static volatile int    g_rt_recv_ok;
static volatile int    g_rt_recv_bad;
static pthread_mutex_t g_hs_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_hs_cv  = PTHREAD_COND_INITIALIZER;
static long            g_hs_recvd;               /* datagrams the receiver consumed */
static volatile int    g_recv_ready;             /* receiver has BOUND its port     */

static void *receiver_thread(void *arg)
{
    UINT   token, desc;
    NSFRQE ri, rs, rb;
    int    i;

    (void)arg;
    rqe_init(&ri, RQ_INITAPI, 0u, 0u);          nsfreq_call(&ri); token = ri.apptok;
    rqe_init(&rs, RQ_SOCKET, 0u, 0u);
    rs.apptok = token; rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_DGRAM; rs.p3 = 17u;
    nsfreq_call(&rs); desc = (UINT)rs.retcode;
    rqe_init(&rb, RQ_BIND, desc, 0u); rb.p1 = HOME_IP; rb.p2 = RT_PORT_R; nsfreq_call(&rb);

    /* Signal the sender that the receive port is BOUND: a datagram sent before
     * this would (correctly) draw ICMP port unreachable and be dropped -- the
     * real UDP semantics, not a bug -- so the sender must wait (see below). */
    pthread_mutex_lock(&g_hs_mtx);
    g_recv_ready = 1;
    pthread_cond_broadcast(&g_hs_cv);
    pthread_mutex_unlock(&g_hs_mtx);

    for (i = 0; i < g_rt_iters; i++) {
        NSFRQE rr;
        UCHAR  buf[32];
        rqe_init(&rr, RQ_RECVFROM, desc, 0u);
        rr.ubuf = buf; rr.ulen = sizeof(buf);
        nsfreq_call(&rr);                        /* blocks until a datagram lands */
        if (rr.retcode == 4 && buf[0] == (UCHAR)'p' && rr.p2 == RT_PORT_S) {
            g_rt_recv_ok++;
        } else {
            g_rt_recv_bad++;
        }
        pthread_mutex_lock(&g_hs_mtx);          /* release the sender for k+1 */
        g_hs_recvd = (long)(i + 1);
        pthread_cond_broadcast(&g_hs_cv);
        pthread_mutex_unlock(&g_hs_mtx);
    }

    {
        NSFRQE rc, rt;
        rqe_init(&rc, RQ_CLOSE, desc, 0u);   nsfreq_call(&rc);
        rqe_init(&rt, RQ_TERMAPI, 0u, 0u);   rt.apptok = token; nsfreq_call(&rt);
    }
    return NULL;
}

static void *sender_thread(void *arg)
{
    UINT   token, desc;
    NSFRQE ri, rs, rb;
    int    i;

    (void)arg;
    rqe_init(&ri, RQ_INITAPI, 0u, 0u);          nsfreq_call(&ri); token = ri.apptok;
    rqe_init(&rs, RQ_SOCKET, 0u, 0u);
    rs.apptok = token; rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_DGRAM; rs.p3 = 17u;
    nsfreq_call(&rs); desc = (UINT)rs.retcode;
    rqe_init(&rb, RQ_BIND, desc, 0u); rb.p1 = HOME_IP; rb.p2 = RT_PORT_S; nsfreq_call(&rb);

    /* Barrier: do not send until the receiver's port is bound (else datagram #0
     * races the receiver's BIND and is legitimately dropped as port-unreachable). */
    pthread_mutex_lock(&g_hs_mtx);
    while (!g_recv_ready) {
        pthread_cond_wait(&g_hs_cv, &g_hs_mtx);
    }
    pthread_mutex_unlock(&g_hs_mtx);

    for (i = 0; i < g_rt_iters; i++) {
        NSFRQE rsnd;
        UCHAR  pay[4];
        pay[0] = (UCHAR)'p'; pay[1] = (UCHAR)'i'; pay[2] = (UCHAR)'n'; pay[3] = (UCHAR)'g';
        rqe_init(&rsnd, RQ_SENDTO, desc, 0u);
        rsnd.ubuf = pay; rsnd.ulen = 4u; rsnd.p1 = HOME_IP; rsnd.p2 = RT_PORT_R;
        nsfreq_call(&rsnd);
        /* Lock-step: wait until the receiver consumed THIS datagram before the
         * next, so only one is ever in flight (see the note above). */
        pthread_mutex_lock(&g_hs_mtx);
        while (g_hs_recvd <= (long)i) {
            pthread_cond_wait(&g_hs_cv, &g_hs_mtx);
        }
        pthread_mutex_unlock(&g_hs_mtx);
    }

    {
        NSFRQE rc, rt;
        rqe_init(&rc, RQ_CLOSE, desc, 0u);   nsfreq_call(&rc);
        rqe_init(&rt, RQ_TERMAPI, 0u, 0u);   rt.apptok = token; nsfreq_call(&rt);
    }
    return NULL;
}

static void test_loopback_roundtrip(int iters)
{
    pthread_t exec, recv, send;

    setup_loop();
    g_rt_iters = iters; g_rt_recv_ok = 0; g_rt_recv_bad = 0; g_hs_recvd = 0;
    g_recv_ready = 0;

    pthread_create(&exec, NULL, exec_thread, NULL);
    pthread_create(&recv, NULL, receiver_thread, NULL);
    pthread_create(&send, NULL, sender_thread, NULL);
    pthread_join(recv, NULL);
    pthread_join(send, NULL);
    nsfevt_stop();
    pthread_join(exec, NULL);

    CHECK_EQ((long)g_rt_recv_ok, (long)iters, "loopback: every datagram received correctly");
    CHECK_EQ((long)g_rt_recv_bad, 0L, "loopback: no wrong / lost datagram");
    CHECK_EQ((long)soc_count(), 0L, "loopback: no sockets left open");
    CHECK_EQ((long)udp_pcb_inuse(), 0L, "loopback: UDPPCB pool at baseline");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "loopback: EVT pool at baseline");
    CHECK_EQ((long)small_inuse(), 0L, "loopback: BUFSMALL at baseline");
}
#endif /* !__MVS__ */

int main(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("=== nsf370 NSFUDP tests ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init");
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve");
    CHECK(nsfudp_reserve(0) == 0, "nsfudp_reserve");
    mm_init_complete();

    /* --- direct-call functional tests over a capture device --- */
    dev_init();
    nsfip_init();
    nsficmp_init();
    soc_init();
    nsfreq_init();
    nsfudp_init();                      /* registers the IP demux handler for 17 */
    CHECK_EQ((long)nsfreq_register_proto(17u, nsfudp_protops()), 0L, "register UDP proto");

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "CAP0", 4);
    cfg.cuu = 0x0500; cfg.type = NSFDEV_T_HOST; cfg.ipaddr = HOME_IP; cfg.mtu = 1500;
    dev = dev_register(&cfg, &cap_ops);
    CHECK(dev != NULL, "dev_register (capture)");
    CHECK(dev_start(dev) == 0, "dev_start (capture)");
    nsfip_local_add(HOME_IP);
    CHECK(nsfip_route_add(0u, 0u, dev, 0u) == 0, "default route -> capture");

    app_init();

    test_output_checksum(dev);
    test_input_checksum(dev);
    test_bind_demux(dev);
    test_input_paths(dev);
    test_port_unreach(dev);
    test_lifetime(dev);

    {
        NSFRQE rt;
        rqe_init(&rt, RQ_TERMAPI, 0u, 0u); rt.apptok = g_apptok;
        nsfreq_dispatch(&rt);
    }

#ifndef __MVS__
    /* --- host-only threaded round-trip over the NSFHOST loopback (#27) --- */
    test_loopback_roundtrip(32);
#endif

    CHECK_EQ((long)soc_count(), 0L, "final: no sockets left open");
    CHECK_EQ((long)sock_inuse(), 0L, "final: SOCKET pool fully returned");
    CHECK_EQ((long)udp_pcb_inuse(), 0L, "final: UDPPCB pool fully returned");
    CHECK_EQ((long)small_inuse(), 0L, "final: BUFSMALL fully returned");

    mm_shutdown();
    return mbt_test_summary("TSTUDP");
}
