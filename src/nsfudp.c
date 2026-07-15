/*
 * nsfudp.c -- UDP (see nsfudp.h, spec ch. 12).
 *
 * The end-to-end proof of the socket/request path: a bound socket receives
 * datagrams (demux -> rxq/parked-RECV) and sends them (SENDTO -> nsfip_output),
 * with no timers and no state machine. Everything here runs on the executive
 * task, run-to-completion, no locking (spec 3): the pcb list, the socket rxq and
 * every callback are mainline state.
 *
 * Header fields are read/written BYTE BY BYTE (big-endian = native S/370), never
 * a struct overlay/cast -- the CTCI/IP discipline (a cast round-trips green on
 * both host and target while emitting host-endian bytes on the little-endian
 * test box). Addresses live as UINTs (octet-1 in the MSB); only the four wire
 * address bytes and the pseudo-header touch byte order.
 *
 * CHECKSUM (ADR-0028). The IPv4 pseudo-header is summed once into a SEED (over a
 * 12-byte stack buffer wrapped in a throwaway PBUF -- reusing the ONE in_cksum
 * routine, no duplication) and threaded into the datagram sum. RFC 768
 * zero-checksum is handled BOTH directions: a computed 0x0000 is transmitted as
 * 0xFFFF; a received 0x0000 is accepted unverified.
 */
#include "nsfudp.h"
#include "nsfip.h"
#include "nsficmp.h"            /* nsficmp_send_error (port unreachable)      */
#include "nsfcksum.h"           /* in_cksum_partial / in_cksum_fold           */
#include "nsfmm.h"              /* UDPPCB pool                                */
#include "nsfbuf.h"             /* PBUF, buf_alloc/copyin/copyout/trim/prepend */
#include "nsfsts.h"
#include "nsftrc.h"
#include <string.h>

/* -- big-endian byte-wise accessors (as in nsfip.c / the codec) --------------- */
static USHORT get16(const UCHAR *p)
{
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

static UINT get32(const UCHAR *p)
{
    return ((UINT)p[0] << 24) | ((UINT)p[1] << 16) |
           ((UINT)p[2] << 8)  |  (UINT)p[3];
}

static void put16(UCHAR *p, USHORT v)
{
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

static void put32(UCHAR *p, UINT v)
{
    p[0] = (UCHAR)(v >> 24);
    p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);
    p[3] = (UCHAR)v;
}

/* -- state: UDPPCB pool + bound-pcb list -------------------------------------- */
static MMPOOL *g_pcbpool;                       /* UDPPCB pool (bind-time alloc) */
static QUEUE   g_pcblist;                        /* bound pcbs, bounded scan list */
static USHORT  g_ephem_next = NSFUDP_EPHEM_LO;   /* rotating ephemeral cursor     */

/* -- statistics (spec 8; component NSFUDP, message range 400-499) ------------- */
static STSCTR *udp_in, *udp_out, *udp_noport, *udp_badlen, *udp_badcksum;
static STSCTR *udp_rxfull, *udp_binds;
static int     udp_stats_ready;

static void udpc(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

static void udp_stats_init(void)
{
    if (udp_stats_ready) {
        return;
    }
    udp_in       = sts_register("NSFUDP", "in");        /* delivered to a socket  */
    udp_out      = sts_register("NSFUDP", "out");       /* transmitted            */
    udp_noport   = sts_register("NSFUDP", "noport");    /* no pcb -> port unreach */
    udp_badlen   = sts_register("NSFUDP", "badlen");    /* length/runt errors     */
    udp_badcksum = sts_register("NSFUDP", "badcksum");  /* checksum verify failed */
    udp_rxfull   = sts_register("NSFUDP", "rxfull");    /* rxq full -> drop       */
    udp_binds    = sts_register("NSFUDP", "binds");     /* pcbs created at bind    */
    udp_stats_ready = 1;
}

/* -- checksum: pseudo-header seed --------------------------------------------- */
/* Sum the 12-byte IPv4 UDP pseudo-header (src, dst, zero, proto, UDP length)
 * into an unfolded partial sum, to seed the datagram sum. Reuses the ONE
 * in_cksum routine over a stack PBUF -- no second checksum. The pseudo-header is
 * even-length (6 words), so the seeded datagram still opens on a high byte. */
static UINT udp_pseudo_seed(UINT src, UINT dst, USHORT udplen)
{
    UCHAR psh[12];
    PBUF  p;

    put32(psh + 0, src);
    put32(psh + 4, dst);
    psh[8] = 0u;
    psh[9] = (UCHAR)NSFIP_PROTO_UDP;
    put16(psh + 10, udplen);

    memset(&p, 0, sizeof(p));
    p.data  = psh;
    p.len   = 12u;
    p.chain = NULL;
    return in_cksum_partial(&p, 0u, 12u, 0u);
}

/* -- bind / demux helpers ----------------------------------------------------- */

/* Is the EXACT (port, laddr) tuple already bound? (dup-bind rejection; a
 * specific laddr and INADDR_ANY on the same port deliberately coexist so demux
 * can prefer the specific one -- so only an exact tuple match is a duplicate.) */
static int udp_bound(USHORT port, UINT laddr)
{
    QELEM *e;

    for (e = g_pcblist.head.next; e != &g_pcblist.head; e = e->next) {
        UDPPCB *pcb = Q_ENTRY(e, UDPPCB, q);
        if (pcb->lport == port && pcb->laddr == laddr) {
            return 1;
        }
    }
    return 0;
}

/* Is `port` used by any pcb (any laddr)? -- ephemeral-port selection. */
static int udp_port_inuse(USHORT port)
{
    QELEM *e;

    for (e = g_pcblist.head.next; e != &g_pcblist.head; e = e->next) {
        UDPPCB *pcb = Q_ENTRY(e, UDPPCB, q);
        if (pcb->lport == port) {
            return 1;
        }
    }
    return 0;
}

/* Pick a free ephemeral local port from the dynamic range, or 0 if none free
 * (never at <=32 pcbs). Rotates a cursor so successive binds spread out. */
static USHORT udp_ephemeral(void)
{
    UINT span = (UINT)(NSFUDP_EPHEM_HI - NSFUDP_EPHEM_LO) + 1u;
    UINT tries;

    for (tries = 0u; tries < span; tries++) {
        USHORT p = g_ephem_next;

        g_ephem_next = (g_ephem_next >= NSFUDP_EPHEM_HI)
                     ? (USHORT)NSFUDP_EPHEM_LO
                     : (USHORT)(g_ephem_next + 1u);
        if (!udp_port_inuse(p)) {
            return p;
        }
    }
    return 0u;
}

/* Demux an inbound (dport, dst) to a bound pcb: a specific laddr match beats an
 * INADDR_ANY match; no match -> NULL (the caller sends port unreachable). */
static UDPPCB *udp_demux(USHORT dport, UINT dst)
{
    QELEM  *e;
    UDPPCB *any = NULL;

    for (e = g_pcblist.head.next; e != &g_pcblist.head; e = e->next) {
        UDPPCB *pcb = Q_ENTRY(e, UDPPCB, q);
        if (pcb->lport != dport) {
            continue;
        }
        if (pcb->laddr == dst) {
            return pcb;                         /* exact laddr wins immediately   */
        }
        if (pcb->laddr == 0u) {
            any = pcb;                          /* remember an INADDR_ANY match   */
        }
    }
    return any;
}

/* -- receive delivery (shared by the parked-RECV and rxq-dequeue paths) -------
 * Copy the payload of `bpay` (headers already stripped) into the request's user
 * buffer, up to r->ulen (DATAGRAM TRUNCATION: any excess is discarded, not kept
 * for the next read -- BSD recvfrom without MSG_TRUNC), fill the peer address in
 * p1/p2, FREE bpay, and complete r with the delivered byte count. */
static void udp_complete_recv(NSFRQE *r, PBUF *bpay, UINT srcaddr, USHORT srcport)
{
    USHORT paylen = buf_chain_len(bpay);
    USHORT want   = 0u;
    USHORT got    = 0u;

    if (r->ubuf != NULL && r->ulen > 0u) {
        want = (paylen < (USHORT)r->ulen) ? paylen : (USHORT)r->ulen;
        got  = buf_copyout(bpay, r->ubuf, want);
    }
    r->p1 = srcaddr;                            /* RECVFROM output: peer address  */
    r->p2 = (UINT)srcport;                      /* RECVFROM output: peer port     */
    buf_free(bpay);
    soc_complete(r, (INT)got, 0);
}

/* -- PROTOPS callbacks (spec 10.2) -------------------------------------------- */

/* SOCKET: nothing to do -- the UDPPCB is created lazily at BIND (spec 12.3). */
static int udp_attach(SOCKCB *s)
{
    (void)s;
    return 0;
}

/* BIND: do_bind (NSFREQ) has already recorded s->laddr / s->lport and marked the
 * socket BOUND; here we allocate the demux pcb. Port 0 -> an ephemeral port
 * (written back to s->lport); a duplicate (lport, laddr) -> EADDRINUSE. */
static int udp_bind(SOCKCB *s)
{
    UDPPCB *pcb;
    USHORT  port  = s->lport;
    UINT    laddr = s->laddr;

    if (s->pcb != NULL) {
        return NSF_EINVAL;                      /* already bound (bind once)      */
    }
    if (port == 0u) {
        port = udp_ephemeral();
        if (port == 0u) {
            return NSF_EADDRINUSE;              /* no free ephemeral port         */
        }
        s->lport = port;
    } else if (udp_bound(port, laddr)) {
        return NSF_EADDRINUSE;                  /* exact (lport, laddr) taken     */
    }

    pcb = (UDPPCB *)mm_alloc(g_pcbpool);
    if (pcb == NULL) {
        return NSF_ENOBUFS;                     /* pool exhausted (drop, not abend)*/
    }
    memset(pcb, 0, sizeof(*pcb));
    pcb->sock  = s;
    pcb->laddr = laddr;
    pcb->lport = port;
    if (q_enq(&g_pcblist, &pcb->q) != 0) {      /* bounded pcb list full          */
        mm_free(g_pcbpool, pcb);
        return NSF_ENOBUFS;
    }
    s->pcb = pcb;
    udpc(udp_binds);
    TRC(UDP, "bind fd=%08X %u.%u.%u.%u:%u", (unsigned)soc_desc(s),
        (unsigned)((laddr >> 24) & 0xFFu), (unsigned)((laddr >> 16) & 0xFFu),
        (unsigned)((laddr >> 8) & 0xFFu),  (unsigned)(laddr & 0xFFu), (unsigned)port);
    return 0;
}

/* SEND / SENDTO: build a UDP datagram in a fresh PBUF and hand it to
 * nsfip_output. p1 = destination address, p2 = destination port (SENDTO); a
 * plain SEND requires a connected peer (not in M3-3) -> EDESTADDRREQ. Completes
 * r with the byte count sent or an error. Ownership: every path either hands b
 * to nsfip_output (which then owns it) or frees it -- never both, never neither. */
static int udp_send(SOCKCB *s, NSFRQE *r)
{
    UINT    dst, src, nexthop = 0u;
    USHORT  dport, sport, seg;
    UINT    ulen = r->ulen;
    NETDEV *dev;
    UCHAR  *p;
    UINT    seed;
    USHORT  ck, mtu, maxpay;
    PBUF   *b;

    if (r->fn == (USHORT)RQ_SENDTO) {
        dst   = r->p1;
        dport = (USHORT)r->p2;
    } else {                                    /* RQ_SEND: connected peer only   */
        if (s->faddr == 0u) {
            soc_complete(r, NSF_RETERR, NSF_EDESTADDRREQ);
            return 0;
        }
        dst   = s->faddr;
        dport = s->fport;
    }

    /* Route once for the MTU bound + source-address selection (nsfip_output
     * routes again on send -- cheap at <=16 entries). */
    dev = nsfip_route(dst, &nexthop);
    if (dev == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EHOSTUNREACH);
        return 0;
    }
    mtu    = (dev->mtu != 0u) ? dev->mtu : 1500u;
    maxpay = (USHORT)(mtu - NSFIP_HDR_MIN - NSFUDP_HDRLEN);     /* MTU - 28       */
    if (ulen > (UINT)maxpay) {                  /* spec 11.3: no fragmentation    */
        soc_complete(r, NSF_RETERR, NSF_EMSGSIZE);
        return 0;
    }

    src   = (s->laddr != 0u) ? s->laddr : dev->ipaddr;
    sport = (s->lport != 0u) ? s->lport : udp_ephemeral();      /* implicit source */

    b = buf_alloc((USHORT)ulen);
    if (b == NULL) {
        soc_complete(r, NSF_RETERR, NSF_ENOBUFS);
        return 0;
    }
    if (ulen > 0u) {
        (void)buf_copyin(b, r->ubuf, (USHORT)ulen);            /* payload -> data  */
    }
    if (buf_prepend(b, (USHORT)NSFUDP_HDRLEN) != 0) {          /* room for UDP hdr */
        buf_free(b);
        soc_complete(r, NSF_RETERR, NSF_ENOBUFS);
        return 0;
    }
    p   = b->data;
    seg = (USHORT)(NSFUDP_HDRLEN + ulen);       /* UDP length (header + data)     */
    put16(p + 0, sport);
    put16(p + 2, dport);
    put16(p + 4, seg);
    put16(p + 6, 0x0000u);                      /* checksum field zero to compute */
    seed = udp_pseudo_seed(src, dst, seg);
    ck   = in_cksum_fold(in_cksum_partial(b, 0u, seg, seed));
    if (ck == 0u) {
        ck = 0xFFFFu;                           /* RFC 768: 0 -> all-ones on wire */
    }
    put16(p + 6, ck);

    TRC(UDP, "OUT %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len %u",
        (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
        (unsigned)((src >> 8) & 0xFFu),  (unsigned)(src & 0xFFu), (unsigned)sport,
        (unsigned)((dst >> 24) & 0xFFu), (unsigned)((dst >> 16) & 0xFFu),
        (unsigned)((dst >> 8) & 0xFFu),  (unsigned)(dst & 0xFFu), (unsigned)dport,
        (unsigned)ulen);

    if (nsfip_output(b, src, dst, (UCHAR)NSFIP_PROTO_UDP,
                     (UCHAR)NSFIP_TTL_DEFAULT) == 0) {
        udpc(udp_out);
        soc_complete(r, (INT)ulen, 0);          /* bytes of payload sent          */
    } else {
        /* nsfip_output already freed b + counted (noroute / build error). */
        soc_complete(r, NSF_RETERR, NSF_EHOSTUNREACH);
    }
    return 0;
}

/* RECV / RECVFROM: deliver a queued datagram, or park (blocking) / EWOULDBLOCK
 * (non-blocking) on an empty rxq (spec 10.3). */
static int udp_recv(SOCKCB *s, NSFRQE *r)
{
    if (!Q_EMPTY(&s->rxq)) {
        QELEM  *e = q_deq(&s->rxq);
        PBUF   *b = Q_ENTRY(e, PBUF, q);
        UDPADDR rec;

        memcpy(&rec, b->data, sizeof(rec));     /* front 8 B = the address record */
        (void)buf_trim_head(b, (USHORT)sizeof(rec));           /* -> payload only */
        udp_complete_recv(r, b, rec.addr, rec.port);
        return 0;
    }
    if (r->flags & RQ_F_NONBLOCK) {
        soc_complete(r, NSF_RETERR, NSF_EWOULDBLOCK);
        return 0;
    }
    return soc_park(s, r, SOC_PEND_RECV);       /* app WAITs until a datagram lands */
}

/* DETACH (called by soc_destroy, spec 10.5): unlink + free the pcb. NULL-safe on
 * an unbound socket. The rxq PBUFs are freed by soc_destroy's own flush, so this
 * is the ONLY teardown NSFUDP adds -- no second path (spec 12.3). */
static int udp_detach(SOCKCB *s)
{
    UDPPCB *pcb = (UDPPCB *)s->pcb;

    if (pcb != NULL) {
        q_remove(&g_pcblist, &pcb->q);
        mm_free(g_pcbpool, pcb);
        s->pcb = NULL;
    }
    return 0;
}

static PROTOPS g_udp_ops = {
    udp_attach,     /* SOCKET  */
    udp_bind,       /* BIND    */
    NULL,           /* CONNECT -- M3-3 has no UDP connect (EOPNOTSUPP)          */
    NULL,           /* LISTEN  -- stream only                                   */
    udp_send,       /* SEND / SENDTO */
    udp_recv,       /* RECV / RECVFROM */
    NULL,           /* CLOSE   -- RQ_CLOSE goes straight to soc_destroy (NSFREQ)*/
    udp_detach      /* final resource release */
};

/* -- inbound datagram (the NSFIP demux target, spec 12.3) --------------------- */
void nsfudp_input(NETDEV *dev, PBUF *b, const IPHDR *ip)
{
    const UCHAR *iph;
    UCHAR       *d;
    UCHAR        hlen;
    USHORT       total, udpseg, sport, dport, ulen, ucksum, paylen, have;
    UINT         src, dst;
    UDPPCB      *pcb;
    SOCKCB      *s;

    (void)dev;
    if (b == NULL || ip == NULL) {
        if (b != NULL) {
            buf_free(b);
        }
        return;
    }

    iph   = (const UCHAR *)ip;                  /* IP header at b->data (aliased) */
    d     = b->data;
    hlen  = (UCHAR)((iph[0] & 0x0Fu) * 4u);
    total = get16(iph + 2);
    src   = get32(iph + 12);
    dst   = get32(iph + 16);

    if (total < (USHORT)hlen) {                 /* nsfip_input validated, defensive */
        udpc(udp_badlen);
        buf_free(b);
        return;
    }
    udpseg = (USHORT)(total - (USHORT)hlen);    /* UDP bytes per the IP length    */
    if (udpseg < (USHORT)NSFUDP_HDRLEN) {
        udpc(udp_badlen);
        buf_free(b);
        return;
    }

    sport  = get16(d + hlen);
    dport  = get16(d + hlen + 2u);
    ulen   = get16(d + hlen + 4u);              /* UDP length field (header+data) */
    ucksum = get16(d + hlen + 6u);
    if (ulen < (USHORT)NSFUDP_HDRLEN || ulen > udpseg) {
        udpc(udp_badlen);
        buf_free(b);
        return;
    }

    /* Checksum (RFC 768): 0 means "sender computed none" -> accept unverified.
     * Otherwise verify over pseudo-header + UDP header + data (length = ulen);
     * a valid datagram (checksum in place) folds to 0. */
    if (ucksum != 0u) {
        UINT seed = udp_pseudo_seed(src, dst, ulen);
        if (in_cksum_fold(in_cksum_partial(b, (USHORT)hlen, ulen, seed)) != 0u) {
            udpc(udp_badcksum);
            buf_free(b);
            return;
        }
    }

    pcb = udp_demux(dport, dst);
    if (pcb == NULL) {
        /* No listener -> ICMP port unreachable (spec 11.2), the first live
         * trigger of M2-4's dormant port-unreachable path. send_error reads the
         * UNTRIMMED b (full IP + UDP headers still present to quote) and does NOT
         * take ownership; we free b right after, exactly as every drop does. */
        udpc(udp_noport);
        nsficmp_send_error(b, (UCHAR)NSFICMP_DEST_UNREACH,
                           (UCHAR)NSFICMP_UNREACH_PORT);
        buf_free(b);
        return;
    }

    s      = pcb->sock;
    paylen = (USHORT)(ulen - (USHORT)NSFUDP_HDRLEN);
    udpc(udp_in);
    TRC(UDP, "IN %u.%u.%u.%u:%u -> :%u len %u",
        (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
        (unsigned)((src >> 8) & 0xFFu),  (unsigned)(src & 0xFFu),
        (unsigned)sport, (unsigned)dport, (unsigned)paylen);

    /* Strip IP + UDP headers down to the payload, then trim any trailing bytes
     * IP carried past the UDP length (ulen < udpseg). */
    (void)buf_trim_head(b, (USHORT)((USHORT)hlen + (USHORT)NSFUDP_HDRLEN));
    have = buf_chain_len(b);
    if (have > paylen) {
        (void)buf_trim_tail(b, (USHORT)(have - paylen));
    }

    /* A parked RECV takes the datagram immediately (spec 12.3); otherwise queue
     * it on the socket rxq (bounded -- full drops + counts, never grows). */
    if (s->pend_recv != NULL) {
        udp_complete_recv(s->pend_recv, b, src, sport);   /* frees b, completes r */
        return;
    }

    /* Prepend the address record so RECVFROM can recover the peer at dequeue
     * (the trimmed headers left >= 28 bytes of headroom, so this cannot fail for
     * a real datagram). */
    if (buf_prepend(b, (USHORT)sizeof(UDPADDR)) != 0) {
        udpc(udp_rxfull);
        buf_free(b);
        return;
    }
    {
        UDPADDR rec;
        rec.addr = src;
        rec.port = sport;
        rec.len  = paylen;
        memcpy(b->data, &rec, sizeof(rec));
    }
    if (q_enq(&s->rxq, &b->q) != 0) {           /* rxq full -> drop + count       */
        udpc(udp_rxfull);
        buf_free(b);
        return;
    }
}

/* -- init / introspection ----------------------------------------------------- */

int nsfudp_reserve(UINT count)
{
    if (count == 0u || count > NSFUDP_MAX_PCB) {
        count = NSFUDP_MAX_PCB;
    }
    /* Init-window only (mm_pool_create ABENDs after the seal, nsfmm.h). objsize
     * 64 per spec 12.2 ("for growth"); the struct is 20 B today. */
    g_pcbpool = mm_pool_create("UDPPCB  ", (USHORT)NSFUDP_PCB_OBJSIZE,
                               (USHORT)count);
    return (g_pcbpool == NULL) ? 1 : 0;
}

void nsfudp_init(void)
{
    udp_stats_init();
    q_init(&g_pcblist, (USHORT)NSFUDP_MAX_PCB);
    g_ephem_next = (USHORT)NSFUDP_EPHEM_LO;
    /* Register the inbound demux handler with NSFIP. Must run after nsfip_init/
     * nsfip_config (which do NOT touch the IP handler table). The SOCKET-side
     * registration (nsfreq_register_proto) is the caller's, so NSFUDP takes no
     * upward dependency on NSFREQ. */
    (void)nsfip_register_proto((UCHAR)NSFIP_PROTO_UDP, nsfudp_input);
}

PROTOPS *nsfudp_protops(void)
{
    return &g_udp_ops;
}

#if NSF_DEBUG
UINT nsfudp_debug_inuse(void)
{
    MMSTATS st;

    if (g_pcbpool == NULL) {
        return 0u;
    }
    mm_stats(g_pcbpool, &st);
    return st.inuse;
}
#endif
