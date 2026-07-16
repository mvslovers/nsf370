/*
 * nsftcp.c -- TCP (see nsftcp.h, spec ch. 13).
 *
 * M4-1: STRUCTURE, NOT BEHAVIOR. This file lays down the TCB, the sequence
 * arithmetic (nsftcp.h), the RFC 793 "SEGMENT ARRIVES" event skeleton and the
 * ONE real emitter (tcp_output_rst). nsftcp_input is written to be read line by
 * line against RFC 793 pp. 64-76: validate -> demux -> (no TCB -> RST per §3.4)
 * / (matched TCB -> the per-state handler). In M4-1 no TCB can leave CLOSED (the
 * connect/listen ops are NULL until M4-2), so the per-state handlers are present
 * as RFC-structured stubs but are never reached -- every real segment demuxes to
 * "no TCB" and, unless it is itself a RST, draws a RST. Behavior (handshake,
 * data, teardown, timers) lands in M4-2/M4-3.
 *
 * Everything here runs on the executive task, run-to-completion, no locking
 * (spec 3): the TCB pool, the TCB list and every callback are mainline state.
 *
 * Header fields are read/written BYTE BY BYTE (big-endian = native S/370), never
 * a struct overlay/cast -- the CTCI/IP/UDP discipline (a cast round-trips green
 * on both host and target while emitting host-endian bytes on the little-endian
 * test box). Addresses live as UINTs (octet-1 in the MSB); only the wire address
 * bytes and the pseudo-header touch byte order.
 *
 * CHECKSUM (ADR-0028). The IPv4 pseudo-header is summed once into a SEED and
 * threaded into the segment sum, reusing the ONE in_cksum routine (no second
 * checksum, no PBUF overlay). UNLIKE UDP there is NO zero-checksum exemption:
 * TCP checksums are mandatory and always verified, and a computed 0 is
 * transmitted as 0 (there is no RFC-768 0 -> 0xFFFF rule for TCP).
 */
#include "nsftcp.h"
#include "nsfip.h"              /* nsfip_output, IPHDR, NSFIP_PROTO_TCP        */
#include "nsfcksum.h"           /* in_cksum_partial / in_cksum_fold           */
#include "nsfmm.h"              /* TCPTCB pool                                */
#include "nsfbuf.h"             /* PBUF, buf_alloc/prepend/free               */
#include "nsftmr.h"             /* tmr_start / tmr_cancel (2MSL, teardown)     */
#include "nsftime.h"            /* nsf_now (ISS clock, RFC 793 §3.3)           */
#include "nsfsts.h"
#include "nsftrc.h"
#include <string.h>

/* -- big-endian byte-wise accessors (as in nsfudp.c / nsfip.c) --------------- */
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

/* A decoded inbound segment -- the RFC 793 SEG.* variables. Filled once by
 * nsftcp_input after the header is validated, so the demux and the per-state
 * handlers read named fields instead of re-parsing bytes. `seglen` is RFC 793
 * §3.3 SEG.LEN = data length + (SYN?1:0) + (FIN?1:0) -- the value the RST ack
 * math depends on. */
typedef struct tcpseg {
    UINT   src, dst;            /* IP source / destination (UINT, octet-1 MSB) */
    USHORT sport, dport;        /* TCP source / destination port (host order)  */
    UINT   seq;                 /* SEG.SEQ                                     */
    UINT   ack;                 /* SEG.ACK                                     */
    USHORT wnd;                 /* SEG.WND                                     */
    USHORT flags;              /* control bits (low 6: URG..FIN)              */
    USHORT dlen;                /* payload bytes (SEG data length)             */
    UINT   seglen;              /* SEG.LEN = dlen + SYN + FIN                  */
    const UCHAR *opt;           /* TCP options (at header+20), or NULL         */
    USHORT optlen;              /* option bytes = dataoffset*4 - 20            */
} TCPSEG;

/* -- state: TCPTCB pool + TCB list -------------------------------------------- */
static MMPOOL *g_tcbpool;                       /* TCPTCB pool (attach-time alloc)*/
static QUEUE   g_tcblist;                        /* live TCBs, demux scan list    */

/* -- statistics (spec 13.5; component NSFTCP, message range 500-599) ---------- */
/* The spec-13.5 minimum set (all 12) plus one private drop counter (hdrerr) for
 * malformed / truncated / bad-offset segments -- §13.5 is a MINIMUM and the CTCI
 * driver (nonip/rpurge/ierr) set the precedent for private additions; `hdrerr`
 * matches the NSFIP naming. In M4-1 only badcksum, hdrerr, resetsent and
 * resetrcvd can tick (the rest are wired for M4-2..M4-4). */
static STSCTR *tcp_activeopen, *tcp_passiveopen, *tcp_established;
static STSCTR *tcp_resetsent, *tcp_resetrcvd, *tcp_rexmit, *tcp_dupack;
static STSCTR *tcp_badcksum, *tcp_oooseg, *tcp_wndprobe, *tcp_keepdrop;
static STSCTR *tcp_timewaitreclaim, *tcp_hdrerr, *tcp_datadrop;
static int     tcp_stats_ready;

static void tcpc(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

static void tcp_stats_init(void)
{
    if (tcp_stats_ready) {
        return;
    }
    tcp_activeopen      = sts_register("NSFTCP", "activeopen");
    tcp_passiveopen     = sts_register("NSFTCP", "passiveopen");
    tcp_established     = sts_register("NSFTCP", "established");
    tcp_resetsent       = sts_register("NSFTCP", "resetsent");
    tcp_resetrcvd       = sts_register("NSFTCP", "resetrcvd");
    tcp_rexmit          = sts_register("NSFTCP", "rexmit");
    tcp_dupack          = sts_register("NSFTCP", "dupack");
    tcp_badcksum        = sts_register("NSFTCP", "badcksum");
    tcp_oooseg          = sts_register("NSFTCP", "oooseg");
    tcp_wndprobe        = sts_register("NSFTCP", "wndprobe");
    tcp_keepdrop        = sts_register("NSFTCP", "keepdrop");
    /* Registered "twreclaim", not the spec-13.5 concept name "timewaitreclaim":
     * an STSCTR name is a 12-char field (nsfsts.h), and the 15-char spec name
     * would be truncated to "timewaitrecl" -- unreadable by sts_value with the
     * full name (it demands an exact fit). "twreclaim" is the operator-visible
     * NSFTCP metric for a TIME_WAIT reclaim. */
    tcp_timewaitreclaim = sts_register("NSFTCP", "twreclaim");
    tcp_hdrerr          = sts_register("NSFTCP", "hdrerr");     /* private: bad hdr/
                                                                * malformed options */
    tcp_datadrop        = sts_register("NSFTCP", "datadrop");   /* private: M4-2 has
                                                                * no data path yet  */
    tcp_stats_ready = 1;
}

/* -- checksum: pseudo-header seed --------------------------------------------- */
/* Sum the 12-byte IPv4 TCP pseudo-header (src, dst, zero, proto=6, TCP length)
 * into an unfolded partial sum, to seed the segment sum. Reuses the ONE in_cksum
 * routine over a stack PBUF -- no second checksum. Even length (6 words), so the
 * seeded segment still opens on a high byte. */
static UINT tcp_pseudo_seed(UINT src, UINT dst, USHORT tcplen)
{
    UCHAR psh[12];
    PBUF  p;

    put32(psh + 0, src);
    put32(psh + 4, dst);
    psh[8] = 0u;
    psh[9] = (UCHAR)NSFIP_PROTO_TCP;
    put16(psh + 10, tcplen);

    memset(&p, 0, sizeof(p));
    p.data  = psh;
    p.len   = 12u;
    p.chain = NULL;
    return in_cksum_partial(&p, 0u, 12u, 0u);
}

/* -- demux (spec 13.3) -------------------------------------------------------- *
 * Linear TCB scan on the 4-tuple (an established/half-open connection), then a
 * listener match on (lport, laddr|ANY), a specific laddr beating ANY. The
 * segment's SOURCE is the connection's FOREIGN end and its DESTINATION is the
 * LOCAL end. In M4-1 no TCB is ever bound to a port or in LISTEN, so this always
 * returns NULL -- the structure is here for M4-2. */
static TCB *tcp_demux(const TCPSEG *seg)
{
    QELEM *e;
    TCB   *listener = NULL;

    /* First: an exact 4-tuple match. */
    for (e = g_tcblist.head.next; e != &g_tcblist.head; e = e->next) {
        TCB    *tcb = Q_ENTRY(e, TCB, q);
        SOCKCB *s   = tcb->sock;

        if (s == NULL || tcb->state == TCP_LISTEN) {
            continue;
        }
        if (s->fport == seg->sport && s->faddr == seg->src &&
            s->lport == seg->dport && s->laddr == seg->dst) {
            return tcb;
        }
    }
    /* Then: a listener on (lport, laddr|ANY). */
    for (e = g_tcblist.head.next; e != &g_tcblist.head; e = e->next) {
        TCB    *tcb = Q_ENTRY(e, TCB, q);
        SOCKCB *s   = tcb->sock;

        if (s == NULL || tcb->state != TCP_LISTEN || s->lport != seg->dport) {
            continue;
        }
        if (s->laddr == seg->dst) {
            return tcb;                         /* exact laddr wins immediately   */
        }
        if (s->laddr == 0u) {
            listener = tcb;                     /* remember an INADDR_ANY listener */
        }
    }
    return listener;
}

/* An IPv4 address that identifies a single host: not zero, not the limited
 * broadcast, not class-D multicast. Mirrors nsficmp_send_error's non-unicast
 * suppression (src/nsficmp.c) so the two error paths agree on what a "source" is
 * (0xE0000000/0xF0000000 == 224.x-239.x). */
static int tcp_addr_is_unicast(UINT ip)
{
    return ip != 0u && ip != 0xFFFFFFFFu && (ip & 0xF0000000u) != 0xE0000000u;
}

/* -- RST emitter (RFC 793 §3.4) ----------------------------------------------- *
 * The one real emitter in M4-1. Builds a header-only RST in a FRESH PBUF and
 * hands it to nsfip_output (which then owns it). The seq/ack are chosen per RFC
 * 793 §3.4 so the offending peer accepts the reset -- the part everyone gets
 * subtly wrong, so it is asserted byte-exact in the tests:
 *   - ACK present in the offending segment: <SEQ=SEG.ACK><CTL=RST>   (ack field 0)
 *   - ACK absent:            <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>
 * SEG.LEN counts SYN and FIN as one octet each (RFC 793 §3.3), which is why a
 * bare SYN draws ack = seg.seq + 1. Ownership: the built PBUF is passed to
 * nsfip_output on success or freed on an early error -- never both, never
 * neither. resetsent is counted only on a successful send. */
static void tcp_output_rst(const TCPSEG *seg)
{
    PBUF  *b;
    UCHAR *p;
    UINT   our  = seg->dst;                     /* addressed to us -> our source  */
    UINT   peer = seg->src;
    UINT   rst_seq, rst_ack, seed;
    USHORT rst_flags, ck;

    /* RFC 1122 §3.2.1.3 / robustness: never reflect a RST toward a source that
     * is not a single unicast host. nsfip_input validates only the DESTINATION
     * (nsfip_is_local), so a segment with a zero/broadcast/multicast source
     * reaches here; sending a RST to it would be a reflected/amplifiable packet
     * on a shared link. Suppressed silently, exactly as nsficmp_send_error
     * suppresses a non-unicast origin (spec 11.2; no counter for a suppressed
     * emission). */
    if (!tcp_addr_is_unicast(peer)) {
        return;
    }

    if (seg->flags & TCP_FL_ACK) {
        rst_seq   = seg->ack;
        rst_ack   = 0u;
        rst_flags = TCP_FL_RST;
    } else {
        rst_seq   = 0u;
        rst_ack   = seg->seq + seg->seglen;
        rst_flags = (USHORT)(TCP_FL_RST | TCP_FL_ACK);
    }

    b = buf_alloc(0u);                          /* header only, no payload        */
    if (b == NULL) {
        return;                                 /* ENOBUFS: drop (exhaustion normal)*/
    }
    if (buf_prepend(b, (USHORT)NSFTCP_HDRLEN) != 0) {
        buf_free(b);
        return;
    }
    p = b->data;
    put16(p + 0,  seg->dport);                  /* our source port = seg dest port */
    put16(p + 2,  seg->sport);                  /* dest port = seg source port     */
    put32(p + 4,  rst_seq);
    put32(p + 8,  rst_ack);
    p[12] = (UCHAR)((NSFTCP_HDRLEN / 4) << 4);  /* data offset = 5 words, resv 0   */
    p[13] = (UCHAR)rst_flags;
    put16(p + 14, 0u);                          /* window 0 on a RST               */
    put16(p + 16, 0u);                          /* checksum (computed below)       */
    put16(p + 18, 0u);                          /* urgent pointer                  */

    seed = tcp_pseudo_seed(our, peer, (USHORT)NSFTCP_HDRLEN);
    ck   = in_cksum_fold(in_cksum_partial(b, 0u, (USHORT)NSFTCP_HDRLEN, seed));
    put16(p + 16, ck);                          /* TCP: a computed 0 stays 0       */

    TRC(TCP, "RST -> %u.%u.%u.%u:%u seq=%08X ack=%08X flags=%02X",
        (unsigned)((peer >> 24) & 0xFFu), (unsigned)((peer >> 16) & 0xFFu),
        (unsigned)((peer >> 8) & 0xFFu),  (unsigned)(peer & 0xFFu),
        (unsigned)seg->sport, (unsigned)rst_seq, (unsigned)rst_ack,
        (unsigned)rst_flags);

    if (nsfip_output(b, our, peer, (UCHAR)NSFIP_PROTO_TCP,
                     (UCHAR)NSFIP_TTL_DEFAULT) == 0) {
        tcpc(tcp_resetsent);
    }
    /* else nsfip_output already freed b + counted (noroute / build error). */
}

/* -- M4-2 state-machine helpers ----------------------------------------------- *
 * Forward declarations: the connection lifecycle is naturally mutually recursive
 * (end-of-life -> soc_destroy -> tcp_detach -> tcp_destroy; established -> graduate
 * -> accept-deliver), so declare the set up front and define them in reading
 * order below. `tcp_destroy` itself is defined after nsftcp_input (spec 13.4). */
static void tcp_destroy(TCB *tcb);
static void tcp_close_done(TCB *tcb);
static void tcp_do_reset(TCB *tcb);
static void tcp_enter_established(TCB *tcb);
static void tcp_graduate(TCB *child);
static void tcp_accept_deliver(TCB *child, NSFRQE *r);
static void tcp_enter_timewait(TCB *tcb);
static void tcp_arm_2msl(TCB *tcb);
static int  tcp_process_ack(TCB *tcb, const TCPSEG *seg);
static void tcp_process_fin(TCB *tcb, const TCPSEG *seg);
static int  tcp_reclaim_timewait(void);

/* Build and send a control segment for `tcb` (SYN / SYN|ACK / ACK / FIN / RST --
 * no data payload in M4-2), optionally carrying an MSS option (SYN and SYN|ACK).
 * SEQ is ISS for a SYN-bearing segment (the SYN occupies ISS) and SND.NXT
 * otherwise; ACK carries RCV.NXT; the window is RCV.WND. A FRESH PBUF is built
 * and handed to nsfip_output (which then owns it) or freed on an early error --
 * single owner (spec 3.4). Returns 0 on success. The caller adjusts SND.NXT for a
 * sequence-consuming flag (SYN / FIN) around this call. */
static int tcp_emit(TCB *tcb, USHORT flags, int want_mss)
{
    SOCKCB *s = tcb->sock;
    PBUF   *b;
    UCHAR  *p;
    USHORT  hlen = (USHORT)NSFTCP_HDRLEN;
    USHORT  win, ck;
    UINT    seq, ackno, seed;

    if (want_mss) {
        hlen = (USHORT)(NSFTCP_HDRLEN + NSFTCP_OPT_MSS_LEN);   /* 24, dataoff 6  */
    }
    b = buf_alloc(0u);                          /* header only, no payload        */
    if (b == NULL) {
        return NSF_ENOBUFS;                     /* exhaustion is normal           */
    }
    if (buf_prepend(b, hlen) != 0) {
        buf_free(b);
        return NSF_ENOBUFS;
    }
    p     = b->data;
    seq   = (flags & TCP_FL_SYN) ? tcb->iss : tcb->snd_nxt;
    ackno = (flags & TCP_FL_ACK) ? tcb->rcv_nxt : 0u;
    win   = (tcb->rcv_wnd > 0xFFFFu) ? 0xFFFFu : (USHORT)tcb->rcv_wnd;

    put16(p + 0,  s->lport);
    put16(p + 2,  s->fport);
    put32(p + 4,  seq);
    put32(p + 8,  ackno);
    p[12] = (UCHAR)((hlen / 4u) << 4);          /* data offset (32-bit words)     */
    p[13] = (UCHAR)flags;
    put16(p + 14, win);
    put16(p + 16, 0u);                          /* checksum (computed below)      */
    put16(p + 18, 0u);                          /* urgent pointer                 */
    if (want_mss) {
        UINT    nh  = 0u;
        NETDEV *dev = nsfip_route(s->faddr, &nh);
        USHORT  mtu = (dev != NULL && dev->mtu != 0u) ? dev->mtu : 1500u;
        USHORT  my  = (USHORT)(mtu - NSFIP_HDR_MIN - NSFTCP_HDRLEN);  /* our rMSS */
        p[20] = (UCHAR)NSFTCP_OPT_MSS;
        p[21] = (UCHAR)NSFTCP_OPT_MSS_LEN;
        put16(p + 22, my);                      /* announce OUR receive MSS       */
    }
    seed = tcp_pseudo_seed(s->laddr, s->faddr, hlen);
    ck   = in_cksum_fold(in_cksum_partial(b, 0u, hlen, seed));
    put16(p + 16, ck);                          /* TCP: a computed 0 stays 0      */

    TRC(TCP, "OUT :%u -> %u.%u.%u.%u:%u flags=%02X seq=%08X ack=%08X",
        (unsigned)s->lport,
        (unsigned)((s->faddr >> 24) & 0xFFu), (unsigned)((s->faddr >> 16) & 0xFFu),
        (unsigned)((s->faddr >> 8) & 0xFFu),  (unsigned)(s->faddr & 0xFFu),
        (unsigned)s->fport, (unsigned)flags, (unsigned)seq, (unsigned)ackno);

    if (nsfip_output(b, s->laddr, s->faddr, (UCHAR)NSFIP_PROTO_TCP,
                     (UCHAR)NSFIP_TTL_DEFAULT) != 0) {
        return NSF_EHOSTUNREACH;                /* nsfip_output freed b + counted */
    }
    return 0;
}

/* An initial send sequence from the platform clock. RFC 793 §3.3 draws the ISS
 * from a notional 32-bit clock ticking ~ every 4 us, so a reused (host, port)
 * pair gets fresh sequence numbers. nsf_now's epoch/scale are platform-specific
 * (nsftime.h) -- only "advances over time, hard to guess" matters -- so mix both
 * halves in, weighting the low half toward the ~4 us tick spirit. */
static UINT tcp_gen_iss(void)
{
    NSFTIME now;

    nsf_now(&now);
    return (now.lo * 4u) + (now.hi << 13);      /* deliberate UINT wrap           */
}

/* Rotating ephemeral local-port picker for an active open with no explicit BIND.
 * Skips any port already used as a local port by a live TCB. Returns 0 only if
 * the whole dynamic range is in use (never at the modest TCB count here). */
static USHORT g_tcp_ephem = (USHORT)NSFTCP_EPHEM_LO;

static int tcp_port_inuse(USHORT port)
{
    QELEM *e;

    for (e = g_tcblist.head.next; e != &g_tcblist.head; e = e->next) {
        TCB *t = Q_ENTRY(e, TCB, q);
        if (t->sock != NULL && t->sock->lport == port) {
            return 1;
        }
    }
    return 0;
}

static USHORT tcp_ephemeral(void)
{
    UINT span = (UINT)(NSFTCP_EPHEM_HI - NSFTCP_EPHEM_LO) + 1u;
    UINT tries;

    for (tries = 0u; tries < span; tries++) {
        USHORT p = g_tcp_ephem;

        g_tcp_ephem = (g_tcp_ephem >= (USHORT)NSFTCP_EPHEM_HI)
                    ? (USHORT)NSFTCP_EPHEM_LO
                    : (USHORT)(g_tcp_ephem + 1u);
        if (!tcp_port_inuse(p)) {
            return p;
        }
    }
    return 0u;
}

/* Fill in the local name for an active open: source address from the route to the
 * peer (unless BIND already set one), local port from the ephemeral range (unless
 * BIND set one). Returns 0, or non-zero if no route / no free port. */
static int tcp_source_select(SOCKCB *s)
{
    UINT    nh  = 0u;
    NETDEV *dev = nsfip_route(s->faddr, &nh);

    if (dev == NULL) {
        return 1;                               /* no route -> EHOSTUNREACH       */
    }
    if (s->laddr == 0u) {
        s->laddr = dev->ipaddr;
    }
    if (s->lport == 0u) {
        USHORT p = tcp_ephemeral();
        if (p == 0u) {
            return 1;
        }
        s->lport = p;
    }
    return 0;
}

/* Parse the MSS option out of a SYN / SYN|ACK's options. Sets *mss to the
 * announced value (clamped to [MSS_MIN, MSS_MAX]) or NSFTCP_MSS_DEFAULT if none
 * is present. Returns 0 on a well-formed (or empty) option list, non-zero on a
 * MALFORMED one -- a length byte < 2, a missing length, or an option running past
 * the header -- so the caller drops the segment and NEVER overruns. */
static int tcp_parse_mss(const TCPSEG *seg, USHORT *mss)
{
    const UCHAR *o = seg->opt;
    USHORT       n = seg->optlen;
    USHORT       i = 0u;

    *mss = (USHORT)NSFTCP_MSS_DEFAULT;
    while (i < n) {
        UCHAR kind = o[i];
        if (kind == (UCHAR)NSFTCP_OPT_END) {
            break;                              /* end of option list             */
        }
        if (kind == (UCHAR)NSFTCP_OPT_NOP) {
            i = (USHORT)(i + 1u);               /* single-byte pad, no length     */
            continue;
        }
        if ((USHORT)(i + 1u) >= n) {
            return 1;                           /* length byte missing            */
        }
        {
            UCHAR len = o[i + 1u];
            if (len < 2u || (USHORT)(i + len) > n) {
                return 1;                       /* runt / overrunning option      */
            }
            if (kind == (UCHAR)NSFTCP_OPT_MSS && len == (UCHAR)NSFTCP_OPT_MSS_LEN) {
                USHORT v = (USHORT)(((USHORT)o[i + 2u] << 8) | o[i + 3u]);
                if (v < (USHORT)NSFTCP_MSS_MIN) { v = (USHORT)NSFTCP_MSS_MIN; }
                if (v > (USHORT)NSFTCP_MSS_MAX) { v = (USHORT)NSFTCP_MSS_MAX; }
                *mss = v;
            }
            i = (USHORT)(i + len);
        }
    }
    return 0;
}

/* Sequence-number acceptability (RFC 793 p.69), the four cases by (seglen,
 * rcv_wnd). Uses only the signed-difference macros -- never a raw comparison. */
static int tcp_seq_acceptable(const TCB *tcb, const TCPSEG *seg)
{
    UINT end = tcb->rcv_nxt + tcb->rcv_wnd;     /* RCV.NXT + RCV.WND              */

    if (seg->seglen == 0u) {
        if (tcb->rcv_wnd == 0u) {
            return (seg->seq == tcb->rcv_nxt);
        }
        return TCP_SEQ_GEQ(seg->seq, tcb->rcv_nxt) && TCP_SEQ_LT(seg->seq, end);
    }
    if (tcb->rcv_wnd == 0u) {
        return 0;                               /* cannot accept data, zero window */
    }
    {
        UINT last    = seg->seq + seg->seglen - 1u;
        int  head_in = TCP_SEQ_GEQ(seg->seq, tcb->rcv_nxt) &&
                       TCP_SEQ_LT(seg->seq, end);
        int  tail_in = TCP_SEQ_GEQ(last, tcb->rcv_nxt) &&
                       TCP_SEQ_LT(last, end);
        return head_in || tail_in;
    }
}

/* Send-window update (RFC 793 p.72): a newer window advertisement (by SEG.SEQ /
 * SEG.ACK ordering) refreshes SND.WND / SND.WL1 / SND.WL2. M4-3 consumes these. */
static void tcp_update_window(TCB *tcb, const TCPSEG *seg)
{
    if (TCP_SEQ_LT(tcb->snd_wl1, seg->seq) ||
        (tcb->snd_wl1 == seg->seq && TCP_SEQ_LEQ(tcb->snd_wl2, seg->ack))) {
        tcb->snd_wnd = seg->wnd;
        tcb->snd_wl1 = seg->seq;
        tcb->snd_wl2 = seg->ack;
    }
}

/* Send a RST built from the TCB state: <SEQ=SND.NXT><CTL=RST>. */
static void tcp_send_rst(TCB *tcb)
{
    (void)tcp_emit(tcb, (USHORT)TCP_FL_RST, 0);
}

/* End of life: a connection is fully finished (LAST_ACK -> CLOSED, TIME_WAIT 2MSL
 * expiry, or a fresh LISTEN / SYN_SENT close). Drive the ONE top-level teardown
 * -- soc_destroy(sock) -> tcp_detach -> tcp_destroy. tcp_destroy NEVER calls
 * soc_destroy, so the recursion terminates (the ownership inversion, spec 13.4).*/
static void tcp_close_done(TCB *tcb)
{
    if (tcb->sock != NULL) {
        soc_destroy(tcb->sock);
    } else {
        tcp_destroy(tcb);                       /* already detached (defensive)   */
    }
}

/* A RST was received in a synchronized state (RFC 793 p.70): complete any parked
 * request with the SPECIFIC errno FIRST -- that clears the pend slot, so the
 * following soc_destroy will not re-complete it with ECONNABORTED -- then tear the
 * connection down. */
static void tcp_do_reset(TCB *tcb)
{
    SOCKCB *s = tcb->sock;

    if (s != NULL) {
        if (s->pend_connect != NULL) {
            soc_complete(s->pend_connect, NSF_RETERR, NSF_ECONNRESET);
        }
        if (s->pend_recv != NULL) {
            soc_complete(s->pend_recv, NSF_RETERR, NSF_ECONNRESET);
        }
        if (s->pend_accept != NULL) {
            soc_complete(s->pend_accept, NSF_RETERR, NSF_ECONNRESET);
        }
    }
    tcp_close_done(tcb);
}

/* A SYN arrived inside an established window (RFC 793 p.71) -- an error: send a
 * RST and reset the connection locally (ECONNRESET to a parked request). */
static void tcp_abort(TCB *tcb)
{
    tcp_send_rst(tcb);
    tcpc(tcp_resetsent);
    tcp_do_reset(tcb);
}

/* Arm / re-arm the 2MSL timer (TIME_WAIT dwell). */
static void tcp_2msl_expire(void *arg)
{
    /* 2MSL elapsed -> the connection is fully gone. Freeing the TCB here is safe:
     * nsftmr_run detached t_2msl and marked it IDLE before this call and re-reads
     * the queue head afterward, and TIME_WAIT keeps ONLY t_2msl armed (the other
     * three were cancelled on TIME_WAIT entry), so no sibling timer of this TCB is
     * left linked when tcp_destroy frees it. */
    tcp_close_done((TCB *)arg);
}

static void tcp_arm_2msl(TCB *tcb)
{
    tmr_start(&tcb->t_2msl, (UINT)NSFTCP_2MSL_TICKS, tcp_2msl_expire, tcb);
}

/* Enter TIME_WAIT: cancel the other three timers (the free-safe invariant above),
 * transition, and arm 2MSL. */
static void tcp_enter_timewait(TCB *tcb)
{
    tmr_cancel(&tcb->t_rexmit);
    tmr_cancel(&tcb->t_persist);
    tmr_cancel(&tcb->t_keep);
    tcb->state = (UCHAR)TCP_TIME_WAIT;
    tcp_arm_2msl(tcb);
}

/* Hand an established, un-ACCEPTed child to a waiting ACCEPT (spec 10.2): the
 * child becomes an INDEPENDENT socket (listener back-pointer cleared) and the
 * request completes with the NEW socket's internal descriptor -- the facade maps
 * the 0-based halfword (M4-5), exactly as RQ_SOCKET returns a descriptor. */
static void tcp_accept_deliver(TCB *child, NSFRQE *r)
{
    SOCKCB *cs = child->sock;

    child->flags   &= (UCHAR)~TCB_F_ONACCEPTQ;
    child->listener = NULL;                     /* now independent                */
    if (cs != NULL) {
        r->p1 = cs->faddr;                      /* peer address (accept output)   */
        r->p2 = (UINT)cs->fport;                /* peer port                      */
        soc_complete(r, (INT)soc_desc(cs), 0);  /* retcode = the new descriptor   */
    } else {
        soc_complete(r, NSF_RETERR, NSF_ECONNABORTED);
    }
}

/* An established child graduates to its listener: complete a parked ACCEPT now,
 * else queue it on the listener's bounded acceptq for a future ACCEPT. */
static void tcp_graduate(TCB *child)
{
    TCB    *listener = child->listener;
    SOCKCB *ls       = (listener != NULL) ? listener->sock : NULL;

    if (ls == NULL) {
        return;                                 /* listener gone -> nothing to do */
    }
    if (ls->pend_accept != NULL) {
        tcp_accept_deliver(child, ls->pend_accept);
        return;
    }
    if (q_enq(&ls->acceptq, &child->acceptlink) == 0) {
        child->flags |= (UCHAR)TCB_F_ONACCEPTQ;
    }
    /* else: the acceptq is unexpectedly full -- unreachable, because the SYN-time
     * backlog check bounds pending children (embryonic + queued) to the acceptq
     * size. Leave the child established but un-queued (reaped by listener teardown
     * / TERMAPI); do NOT destroy it here -- this runs mid-ACK-processing and the
     * caller (tcp_process_ack -> tcp_synchronized_input) would then touch a freed
     * TCB. */
}

/* Reach ESTABLISHED (RFC 793): count it, complete a parked CONNECT (active side)
 * or graduate to the listener (passive side) -- the two are mutually exclusive. */
static void tcp_enter_established(TCB *tcb)
{
    SOCKCB *s = tcb->sock;

    tcb->state = (UCHAR)TCP_ESTABLISHED;
    if (s != NULL) {
        s->state = (UCHAR)SOC_ST_CONNECTED;
    }
    tcpc(tcp_established);
    if (s != NULL && s->pend_connect != NULL) {
        soc_complete(s->pend_connect, NSF_RETOK, 0);   /* active open done        */
    }
    if (tcb->listener != NULL) {
        tcp_graduate(tcb);                              /* passive open done       */
    }
}

/* ACK processing (RFC 793 p.72). Returns 1 to continue segment processing (URG /
 * text / FIN), 0 to stop -- the segment was consumed here, and the TCB may have
 * been destroyed (LAST_ACK -> CLOSED), so the caller must not touch it again. */
static int tcp_process_ack(TCB *tcb, const TCPSEG *seg)
{
    if (tcb->state == (UCHAR)TCP_SYN_RCVD) {
        /* The handshake-completing ACK: SND.UNA < SEG.ACK <= SND.NXT. */
        if (TCP_SEQ_LEQ(seg->ack, tcb->snd_una) || TCP_SEQ_GT(seg->ack, tcb->snd_nxt)) {
            tcp_output_rst(seg);                /* <SEQ=SEG.ACK><CTL=RST>, keep TCB */
            return 0;
        }
        tcb->snd_una = seg->ack;
        tcp_update_window(tcb, seg);
        tcp_enter_established(tcb);             /* a same-segment FIN may follow   */
        return 1;
    }

    if (TCP_SEQ_GT(seg->ack, tcb->snd_nxt)) {
        tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* ACKs something unsent -> ACK+drop*/
        return 0;
    }
    if (TCP_SEQ_GT(seg->ack, tcb->snd_una)) {
        tcb->snd_una = seg->ack;                /* new data / our FIN acknowledged */
    }
    /* (a duplicate ACK, SEG.ACK <= SND.UNA, is ignored in M4-2: no rexmit yet.) */
    tcp_update_window(tcb, seg);

    /* `our_fin_acked` == SND.UNA reached SND.NXT (our FIN was the last byte sent).*/
    switch (tcb->state) {
    case TCP_FIN_WAIT_1:
        if (tcb->snd_una == tcb->snd_nxt) {
            tcb->state = (UCHAR)TCP_FIN_WAIT_2;
        }
        return 1;                               /* a same-segment FIN may follow   */
    case TCP_CLOSING:
        if (tcb->snd_una == tcb->snd_nxt) {
            tcp_enter_timewait(tcb);
        }
        return 1;
    case TCP_LAST_ACK:
        if (tcb->snd_una == tcb->snd_nxt) {
            tcp_close_done(tcb);                /* our FIN acked -> CLOSED, destroy */
            return 0;                           /* tcb is gone                     */
        }
        return 1;
    case TCP_TIME_WAIT:
        tcp_arm_2msl(tcb);                      /* stray ACK: restart 2MSL         */
        return 1;
    case TCP_FIN_WAIT_2:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    default:
        return 1;
    }
}

/* FIN processing (RFC 793 pp. 75-76). Only the in-order FIN (sitting exactly at
 * RCV.NXT, with no undelivered data ahead of it -- M4-2 has no data path) is
 * acted on: advance RCV.NXT past it, ACK it, and transition. */
static void tcp_process_fin(TCB *tcb, const TCPSEG *seg)
{
    if (seg->seq + (UINT)seg->dlen != tcb->rcv_nxt) {
        return;                                 /* not the in-order FIN yet        */
    }
    tcb->rcv_nxt = tcb->rcv_nxt + 1u;           /* consume the FIN                 */
    tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);       /* ACK it                          */

    switch (tcb->state) {
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
        tcb->state = (UCHAR)TCP_CLOSE_WAIT;
        break;
    case TCP_FIN_WAIT_1:
        /* Our FIN was not yet acked (else step 5 already moved us to FIN_WAIT_2),
         * so this is a simultaneous close -> CLOSING. */
        tcb->state = (UCHAR)TCP_CLOSING;
        break;
    case TCP_FIN_WAIT_2:
        tcp_enter_timewait(tcb);
        break;
    case TCP_TIME_WAIT:
        tcp_arm_2msl(tcb);                      /* remote re-FIN: re-ACK + restart */
        break;
    default:
        break;                                  /* CLOSE_WAIT / CLOSING / LAST_ACK */
    }
}

/* Reclaim a TIME_WAIT TCB under pool pressure (spec 13.4): destroy it so its pool
 * object frees, then the caller retries the allocation once. Picks the first
 * TIME_WAIT TCB scanning the demux list from the head -- the oldest by INSERTION
 * order (new TCBs enqueue at the tail), which is NOT necessarily the one closest
 * to 2MSL expiry: a later-inserted connection can enter TIME_WAIT first. That
 * distinction is materially irrelevant for reclaim -- every candidate is an
 * already-dead connection, so any TIME_WAIT slot serves -- but if M4-6's
 * pool-pressure stress asserts evict-nearest-expiry fairness, order by the t_2msl
 * remaining instead. Returns 1 if one was reclaimed. */
static int tcp_reclaim_timewait(void)
{
    QELEM *e;

    for (e = g_tcblist.head.next; e != &g_tcblist.head; e = e->next) {
        TCB *t = Q_ENTRY(e, TCB, q);
        if (t->state == (UCHAR)TCP_TIME_WAIT) {
            tcpc(tcp_timewaitreclaim);
            tcp_close_done(t);                  /* full teardown -> pool object freed*/
            return 1;
        }
    }
    return 0;
}

/* Number of a listener's pending children (embryonic SYN_RCVD + established but
 * un-ACCEPTed), for the backlog check. */
static UINT tcp_pending_children(const TCB *listener)
{
    QELEM *e;
    UINT   n = 0u;

    for (e = g_tcblist.head.next; e != &g_tcblist.head; e = e->next) {
        const TCB *t = Q_ENTRY(e, TCB, q);
        if (t->listener == listener) {
            n++;
        }
    }
    return n;
}

/* Create the embryonic child socket + TCB for a passive open, reclaiming a
 * TIME_WAIT TCB under pool pressure and retrying once (spec 13.4). Scopes the
 * child to the listener's app (owner_ascb) so RQ_TERMAPI reaps it. Returns the
 * child SOCKCB (its pcb is a fresh TCB), or NULL if the pool is exhausted even
 * after reclaim -- the caller then drops the SYN silently (RFC-conformant). */
static SOCKCB *tcp_child_create(TCB *listener)
{
    SOCKCB *ls = listener->sock;
    SOCKCB *cs;

    cs = soc_create(ls->domain, ls->type, ls->proto, ls->ops);
    if (cs == NULL && tcp_reclaim_timewait()) {
        cs = soc_create(ls->domain, ls->type, ls->proto, ls->ops);
    }
    if (cs != NULL) {
        cs->owner_ascb = ls->owner_ascb;        /* same app scope (TERMAPI)       */
    }
    return cs;
}

/* Passive open (RFC 793 p.65-66, LISTEN + SYN): allocate an embryonic child in
 * SYN_RCVD linked to the listener, seed its sequence spaces, and reply SYN|ACK.
 * Backlog and pool exhaustion drop the SYN silently (the peer retransmits); a
 * malformed option list drops it and counts hdrerr. Never half-allocates. */
static void tcp_passive_open(TCB *listener, const TCPSEG *seg)
{
    SOCKCB *ls = listener->sock;
    SOCKCB *cs;
    TCB    *child;
    USHORT  mss;

    if (tcp_pending_children(listener) >= (UINT)ls->acceptq.maxcount) {
        return;                                 /* backlog full -> drop SYN        */
    }
    if (tcp_parse_mss(seg, &mss) != 0) {
        tcpc(tcp_hdrerr);                       /* malformed options -> drop       */
        return;
    }
    cs = tcp_child_create(listener);
    if (cs == NULL) {
        return;                                 /* pool exhausted -> drop SYN      */
    }
    child = (TCB *)cs->pcb;

    cs->laddr = seg->dst;                       /* local = the SYN's destination   */
    cs->lport = ls->lport;
    cs->faddr = seg->src;                       /* foreign = the SYN's source      */
    cs->fport = seg->sport;
    cs->state = (UCHAR)SOC_ST_CONNECTED;

    child->listener = listener;
    child->mss      = mss;
    child->iss      = tcp_gen_iss();
    child->snd_una  = child->iss;
    child->snd_nxt  = child->iss + 1u;          /* our SYN occupies ISS            */
    child->irs      = seg->seq;
    child->rcv_nxt  = seg->seq + 1u;            /* past the peer's SYN             */
    child->rcv_wnd  = NSFTCP_RCVWND_DEFAULT;
    child->state    = (UCHAR)TCP_SYN_RCVD;
    tcpc(tcp_passiveopen);

    if (tcp_emit(child, (USHORT)(TCP_FL_SYN | TCP_FL_ACK), 1) != 0) {
        soc_destroy(cs);                        /* could not reply -> abandon child */
    }
}

/* -- per-state handlers (RFC 793 pp. 65-76 "SEGMENT ARRIVES") ------------------
 * Each handler reads line-by-line against the RFC's ordered steps; the M4-1 stub
 * comments are now the real bodies. */

/* RFC 793 p.65 "If the state is LISTEN". */
static void tcp_listen_input(TCB *tcb, const TCPSEG *seg)
{
    if (seg->flags & TCP_FL_RST) {
        return;                                 /* 1. RST -> ignore                */
    }
    if (seg->flags & TCP_FL_ACK) {
        tcp_output_rst(seg);                    /* 2. ACK -> <SEQ=SEG.ACK><RST>    */
        return;
    }
    if (seg->flags & TCP_FL_SYN) {
        tcp_passive_open(tcb, seg);             /* 3. SYN -> passive open          */
        return;
    }
    /* 4. anything else -> drop. */
}

/* RFC 793 p.66 "If the state is SYN-SENT". */
static void tcp_synsent_input(TCB *tcb, const TCPSEG *seg)
{
    SOCKCB *s      = tcb->sock;
    int     ack_ok = 0;

    /* 1. check the ACK bit: SEG.ACK must acknowledge our SYN (ISS < ACK <= NXT). */
    if (seg->flags & TCP_FL_ACK) {
        if (TCP_SEQ_LEQ(seg->ack, tcb->iss) || TCP_SEQ_GT(seg->ack, tcb->snd_nxt)) {
            if (!(seg->flags & TCP_FL_RST)) {
                tcp_output_rst(seg);            /* unacceptable ACK -> RST, drop   */
            }
            return;
        }
        ack_ok = 1;
    }

    /* 2. check the RST bit: an acceptable-ACK RST refuses the connect. */
    if (seg->flags & TCP_FL_RST) {
        if (ack_ok) {
            tcpc(tcp_resetrcvd);
            if (s != NULL && s->pend_connect != NULL) {
                soc_complete(s->pend_connect, NSF_RETERR, NSF_ECONNREFUSED);
            }
            tcp_close_done(tcb);
        }
        return;                                 /* an ACK-less RST is discarded    */
    }

    /* 3. security / precedence -- not modeled. */

    /* 4. check the SYN bit. */
    if (seg->flags & TCP_FL_SYN) {
        tcb->irs     = seg->seq;
        tcb->rcv_nxt = seg->seq + 1u;           /* past the peer's SYN             */
        if (ack_ok) {
            tcb->snd_una = seg->ack;
        }
        {
            USHORT mss;
            if (tcp_parse_mss(seg, &mss) == 0) {
                tcb->mss = mss;                 /* a malformed list keeps default  */
            }
        }
        tcp_update_window(tcb, seg);
        if (TCP_SEQ_GT(tcb->snd_una, tcb->iss)) {
            tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* our SYN acked -> ESTABLISHED */
            tcp_enter_established(tcb);
        } else {
            tcb->state = (UCHAR)TCP_SYN_RCVD;       /* simultaneous open           */
            tcp_emit(tcb, (USHORT)(TCP_FL_SYN | TCP_FL_ACK), 1);
        }
        return;
    }

    /* 5. neither SYN nor RST -> drop. */
}

/* RFC 793 pp. 69-76, the synchronized states (SYN_RCVD, ESTABLISHED, FIN_WAIT_1,
 * FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT), in RFC step order. */
static void tcp_synchronized_input(TCB *tcb, const TCPSEG *seg)
{
    /* 1. sequence-number acceptability -> unacceptable: ACK (unless RST), drop. */
    if (!tcp_seq_acceptable(tcb, seg)) {
        if (!(seg->flags & TCP_FL_RST)) {
            tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);
        }
        return;
    }
    /* 2. RST -> reset the connection (ECONNRESET to a parked request). */
    if (seg->flags & TCP_FL_RST) {
        tcpc(tcp_resetrcvd);
        tcp_do_reset(tcb);
        return;
    }
    /* 3. security / precedence -- not modeled. */
    /* 4. SYN in the window -> an error: RST and reset. */
    if (seg->flags & TCP_FL_SYN) {
        tcpc(tcp_resetrcvd);
        tcp_abort(tcb);
        return;
    }
    /* 5. ACK field -> advance SND.UNA, window update, per-state consequences. */
    if (!(seg->flags & TCP_FL_ACK)) {
        return;                                 /* no ACK bit -> drop              */
    }
    if (!tcp_process_ack(tcb, seg)) {
        return;                                 /* consumed / TCB destroyed        */
    }
    /* 6. URG -- not modeled. */
    /* 7. segment text -- M4-2 has no data path: drop + count (do NOT advance
     *    RCV.NXT over undelivered data, which would falsely ACK it). */
    if (seg->dlen > 0u) {
        tcpc(tcp_datadrop);
    }
    /* 8. FIN -> advance RCV.NXT past it, ACK, transition. */
    if (seg->flags & TCP_FL_FIN) {
        tcp_process_fin(tcb, seg);
    }
}

/* RFC 793 p.65 "If the state is CLOSED (i.e., TCB does not exist)". All data is
 * discarded; an incoming RST is discarded (never a RST in response to a RST);
 * anything else draws a RST (tcp_output_rst picks the §3.4 seq/ack). */
static void tcp_closed_input(const TCPSEG *seg)
{
    if (seg->flags & TCP_FL_RST) {
        tcpc(tcp_resetrcvd);                    /* silence: no RST-on-RST         */
        return;
    }
    tcp_output_rst(seg);
}

/* Route a matched TCB to its per-state handler. A demuxed TCB is never CLOSED
 * (a CLOSED TCB has no ports / is not a listener, so tcp_demux cannot return
 * it), but CLOSED is routed defensively. */
static void tcp_state_input(TCB *tcb, const TCPSEG *seg)
{
    switch (tcb->state) {
    case TCP_LISTEN:       tcp_listen_input(tcb, seg);        break;
    case TCP_SYN_SENT:     tcp_synsent_input(tcb, seg);       break;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:    tcp_synchronized_input(tcb, seg);  break;
    case TCP_CLOSED:
    default:               tcp_closed_input(seg);             break;
    }
}

/* -- inbound segment (the NSFIP demux target, spec 13.3) ---------------------- */
void nsftcp_input(NETDEV *dev, PBUF *b, const IPHDR *ip)
{
    const UCHAR *iph;
    UCHAR       *d;
    UCHAR        ihl, thoff;
    USHORT       total, tcpseg;
    UINT         src, dst, seed;
    TCPSEG       seg;
    TCB         *tcb;

    (void)dev;
    if (b == NULL || ip == NULL) {
        if (b != NULL) {
            buf_free(b);
        }
        return;
    }

    iph   = (const UCHAR *)ip;                  /* IP header at b->data (aliased) */
    d     = b->data;
    ihl   = (UCHAR)((iph[0] & 0x0Fu) * 4u);
    total = get16(iph + 2);
    src   = get32(iph + 12);
    dst   = get32(iph + 16);

    /* Length + data-offset sanity BEFORE reading seq/ack, or a truncated header
     * reads garbage. Segment length is purely ip.total - ihl (TCP has no length
     * field of its own). */
    if (total < (USHORT)ihl) {                  /* nsfip_input validated; defensive*/
        tcpc(tcp_hdrerr);
        buf_free(b);
        return;
    }
    tcpseg = (USHORT)(total - (USHORT)ihl);     /* TCP bytes per the IP length    */
    if (tcpseg < (USHORT)NSFTCP_HDRLEN) {       /* runt: no room for a TCP header */
        tcpc(tcp_hdrerr);
        buf_free(b);
        return;
    }
    thoff = (UCHAR)(((d[ihl + 12] >> 4) & 0x0Fu) * 4u);        /* data offset*4   */
    if (thoff < (USHORT)NSFTCP_HDRLEN || thoff > tcpseg) {
        tcpc(tcp_hdrerr);
        buf_free(b);
        return;
    }

    /* Mandatory checksum over pseudo-header + segment (length tcpseg). A valid
     * segment folds to 0. NO zero-checksum exemption (unlike UDP). */
    seed = tcp_pseudo_seed(src, dst, tcpseg);
    if (in_cksum_fold(in_cksum_partial(b, (USHORT)ihl, tcpseg, seed)) != 0u) {
        tcpc(tcp_badcksum);
        buf_free(b);
        return;
    }

    /* Decode SEG.* (RFC 793 §3.3). seglen counts SYN and FIN as one each. */
    seg.src    = src;
    seg.dst    = dst;
    seg.sport  = get16(d + ihl + 0);
    seg.dport  = get16(d + ihl + 2);
    seg.seq    = get32(d + ihl + 4);
    seg.ack    = get32(d + ihl + 8);
    seg.flags  = (USHORT)d[ihl + 13];
    seg.wnd    = get16(d + ihl + 14);
    seg.dlen   = (USHORT)(tcpseg - (USHORT)thoff);
    seg.seglen = (UINT)seg.dlen
               + ((seg.flags & TCP_FL_SYN) ? 1u : 0u)
               + ((seg.flags & TCP_FL_FIN) ? 1u : 0u);
    seg.optlen = (USHORT)(thoff - (UCHAR)NSFTCP_HDRLEN);        /* dataoff*4 - 20 */
    seg.opt    = (seg.optlen > 0u) ? (d + ihl + NSFTCP_HDRLEN) : NULL;

    TRC(TCP, "IN %u.%u.%u.%u:%u -> :%u flags=%02X seq=%08X ack=%08X len=%u",
        (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
        (unsigned)((src >> 8) & 0xFFu),  (unsigned)(src & 0xFFu),
        (unsigned)seg.sport, (unsigned)seg.dport, (unsigned)seg.flags,
        (unsigned)seg.seq, (unsigned)seg.ack, (unsigned)seg.dlen);

    /* Demux; no TCB (or a CLOSED one) -> the RFC 793 CLOSED processing (RST). */
    tcb = tcp_demux(&seg);
    if (tcb == NULL) {
        tcp_closed_input(&seg);
        buf_free(b);
        return;
    }
    tcp_state_input(tcb, &seg);
    buf_free(b);                                /* M4-1 handlers read seg, not b  */
}

/* -- teardown (spec 13.4) ----------------------------------------------------- *
 * The ONE destroy function every close/reset/shutdown path routes through, from
 * day one -- even though M4-1 never populates sndq/oooq. It tears down the TCB
 * ONLY: the SOCKCB side (flush rxq/acceptq, complete PARKED NSFRQEs with
 * ECONNABORTED, bump the slot generation, mm_free the SOCKCB) is soc_destroy's
 * checklist (spec 10.5), and in M4-1 the sole caller is soc_destroy via the
 * detach op. */
static void tcp_flushq(QUEUE *q)
{
    QELEM *e;

    while ((e = q_deq(q)) != NULL) {
        buf_free(Q_ENTRY(e, PBUF, q));
    }
}

static void tcp_destroy(TCB *tcb)
{
    if (tcb == NULL) {
        return;
    }
    /* 0. If this is a LISTENer, tear every pending child down first (embryonic
     *    SYN_RCVD + established un-ACCEPTed on the acceptq, M4-2). Each child is a
     *    full socket and dies through soc_destroy (-> its own tcp_detach ->
     *    tcp_destroy); dequeue it from OUR acceptq and clear its listener
     *    back-pointer BEFORE that, so the child's teardown never touches this
     *    dying listener. Capture the next demux-list link before destroying, since
     *    soc_destroy unlinks the child from g_tcblist. This is what keeps
     *    soc_destroy's acceptq (now protocol-owned) empty by the time it runs. */
    if (tcb->state == (UCHAR)TCP_LISTEN && tcb->sock != NULL) {
        QELEM *e = g_tcblist.head.next;
        while (e != &g_tcblist.head) {
            TCB   *child = Q_ENTRY(e, TCB, q);
            QELEM *nx    = e->next;
            if (child->listener == tcb) {
                if (child->flags & TCB_F_ONACCEPTQ) {
                    q_remove(&tcb->sock->acceptq, &child->acceptlink);
                }
                child->flags   &= (UCHAR)~TCB_F_ONACCEPTQ;
                child->listener = NULL;
                if (child->sock != NULL) {
                    soc_destroy(child->sock);
                }
            }
            e = nx;
        }
    }
    /* 0b. An established un-ACCEPTed child destroyed DIRECTLY (RST / abort /
     *     TERMAPI) while still queued on a LIVE listener's acceptq: unlink myself
     *     so the queue stays consistent (skipped in the listener path above, which
     *     already cleared these fields). */
    if ((tcb->flags & TCB_F_ONACCEPTQ) && tcb->listener != NULL &&
        tcb->listener->sock != NULL) {
        q_remove(&tcb->listener->sock->acceptq, &tcb->acceptlink);
        tcb->flags &= (UCHAR)~TCB_F_ONACCEPTQ;
    }

    /* 1. cancel all four timers (idempotent; TMR_IDLE-safe on a fresh TCB). */
    tmr_cancel(&tcb->t_rexmit);
    tmr_cancel(&tcb->t_persist);
    tmr_cancel(&tcb->t_keep);
    tmr_cancel(&tcb->t_2msl);
    /* 2. free the sndq + oooq PBUF chains (empty in M4-1). */
    tcp_flushq(&tcb->sndq);
    tcp_flushq(&tcb->oooq);
    tcb->sndq_bytes = 0u;
    /* 3. unlink from the TCB list. */
    q_remove(&g_tcblist, &tcb->q);
    /* 4. detach from the SOCKCB (soc_destroy owns the parked-request completion).*/
    if (tcb->sock != NULL) {
        tcb->sock->pcb = NULL;
        tcb->sock      = NULL;
    }
    /* 5. return the TCB to its pool. */
    mm_free(g_tcbpool, tcb);
}

/* -- PROTOPS callbacks (spec 10.2) -------------------------------------------- */

/* SOCKET: allocate + initialize the TCB (CLOSED), link it into the demux list.
 * On any failure leaves s->pcb == NULL so soc_create's teardown (soc_destroy ->
 * tcp_detach) no-ops. A non-zero return makes soc_create tear the socket down
 * and RQ_SOCKET complete with EMFILE (exhaustion is normal, never an abend). */
static int tcp_attach(SOCKCB *s)
{
    TCB *tcb;

    if (s->pcb != NULL) {
        return NSF_EINVAL;                      /* attach once (defensive)        */
    }
    tcb = (TCB *)mm_alloc(g_tcbpool);
    if (tcb == NULL) {
        return NSF_ENOBUFS;                     /* pool exhausted -> EMFILE-class */
    }
    memset(tcb, 0, sizeof(*tcb));
    tcb->sock    = s;
    tcb->state   = TCP_CLOSED;
    tcb->mss     = NSFTCP_MSS_DEFAULT;
    tcb->rcv_wnd = NSFTCP_RCVWND_DEFAULT;
    q_init(&tcb->sndq, (USHORT)NSFTCP_MAX_TCB);   /* bounded; byte flow ctrl M4-3 */
    q_init(&tcb->oooq, 4u);                        /* spec 13.2: oooq bounded 4    */
    if (q_enq(&g_tcblist, &tcb->q) != 0) {         /* demux list full             */
        mm_free(g_tcbpool, tcb);
        return NSF_ENOBUFS;
    }
    s->pcb = tcb;                                  /* published last: failure=NULL */
    TRC(TCP, "attach fd=%08X tcb", (unsigned)soc_desc(s));
    return 0;
}

/* DETACH (called by soc_destroy, spec 10.5): destroy the TCB. NULL-safe on a
 * socket whose attach failed. This is the ONLY teardown NSFTCP adds -- no second
 * path (spec 13.4). */
static int tcp_detach(SOCKCB *s)
{
    if (s->pcb != NULL) {
        tcp_destroy((TCB *)s->pcb);             /* nulls s->pcb                   */
    }
    return 0;
}

/* CONNECT (active open): record the foreign name, select a source, pick an ISS,
 * send SYN (+MSS), -> SYN_SENT, and PARK the request. The parked RQ_CONNECT is
 * completed later by the SYN|ACK (RETOK, tcp_enter_established) or a RST
 * (ECONNREFUSED, tcp_synsent_input) or teardown (ECONNABORTED, soc_destroy). */
static int tcp_connect(SOCKCB *s, NSFRQE *r)
{
    TCB *tcb = (TCB *)s->pcb;

    if (tcb == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EINVAL);
        return 0;
    }
    if (tcb->state != (UCHAR)TCP_CLOSED) {
        soc_complete(r, NSF_RETERR, NSF_EISCONN);   /* already connecting/connected */
        return 0;
    }
    s->faddr = r->p1;                               /* p1 = foreign addr (octet1 MSB)*/
    s->fport = (USHORT)r->p2;                       /* p2 = foreign port             */
    if (s->faddr == 0u || s->fport == 0u) {
        soc_complete(r, NSF_RETERR, NSF_EINVAL);
        return 0;
    }
    if (tcp_source_select(s) != 0) {
        soc_complete(r, NSF_RETERR, NSF_EHOSTUNREACH);
        return 0;
    }
    tcb->iss     = tcp_gen_iss();
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss + 1u;                   /* our SYN occupies ISS          */
    tcb->rcv_wnd = NSFTCP_RCVWND_DEFAULT;
    tcb->state   = (UCHAR)TCP_SYN_SENT;
    s->state     = (UCHAR)SOC_ST_CONNECTED;         /* connecting                    */
    if (tcp_emit(tcb, (USHORT)TCP_FL_SYN, 1) != 0) {
        tcb->state = (UCHAR)TCP_CLOSED;             /* could not send SYN            */
        soc_complete(r, NSF_RETERR, NSF_EHOSTUNREACH);
        return 0;
    }
    tcpc(tcp_activeopen);
    return soc_park(s, r, SOC_PEND_CONNECT);        /* wake on SYN|ACK / RST         */
}

/* LISTEN (passive open enable): clamp the backlog to the acceptq bound, size the
 * acceptq to it, and enter LISTEN. Synchronous -- do_listen (NSFREQ) completes r. */
static int tcp_listen(SOCKCB *s, int backlog)
{
    TCB *tcb = (TCB *)s->pcb;

    if (tcb == NULL || tcb->state != (UCHAR)TCP_CLOSED) {
        return NSF_EINVAL;                          /* only a fresh socket may listen */
    }
    if (backlog < 1) {
        backlog = 1;
    }
    if (backlog > NSFSOC_ACCEPTQ_MAX) {
        backlog = NSFSOC_ACCEPTQ_MAX;
    }
    q_init(&s->acceptq, (USHORT)backlog);           /* bound the backlog (spec 10.2)  */
    tcb->state = (UCHAR)TCP_LISTEN;
    s->state   = (UCHAR)SOC_ST_LISTEN;
    return 0;
}

/* ACCEPT: hand back the oldest established child (its internal descriptor), or
 * park (blocking) / EWOULDBLOCK (non-blocking) on an empty acceptq (spec 10.3). */
static int tcp_accept(SOCKCB *s, NSFRQE *r)
{
    TCB *tcb = (TCB *)s->pcb;

    if (tcb == NULL || tcb->state != (UCHAR)TCP_LISTEN) {
        soc_complete(r, NSF_RETERR, NSF_EINVAL);    /* accept on a non-listener      */
        return 0;
    }
    if (!Q_EMPTY(&s->acceptq)) {
        TCB *child = Q_ENTRY(q_deq(&s->acceptq), TCB, acceptlink);
        tcp_accept_deliver(child, r);               /* completes r with the desc     */
        return 0;
    }
    if (r->flags & RQ_F_NONBLOCK) {
        soc_complete(r, NSF_RETERR, NSF_EWOULDBLOCK);
        return 0;
    }
    return soc_park(s, r, SOC_PEND_ACCEPT);         /* wake when a child graduates   */
}

/* CLOSE (RFC 793 CLOSE call): graceful FIN teardown by state. The app is done, so
 * r completes immediately (RETOK) and the connection finishes in the background
 * (FIN handshake, TIME_WAIT) -- BSD close() semantics. A LISTEN / SYN_SENT / fresh
 * socket has nothing on the wire, so it is destroyed now. Every path completes r
 * exactly once and returns NSF_CLOSE_OWNED (this op OWNS r + teardown, so do_close
 * does nothing further); the TCB dies through tcp_destroy (spec 13.4). */
static int tcp_close(SOCKCB *s, NSFRQE *r)
{
    TCB *tcb = (TCB *)s->pcb;

    if (tcb == NULL) {
        soc_complete(r, NSF_RETOK, 0);
        soc_destroy(s);
        return NSF_CLOSE_OWNED;
    }
    switch (tcb->state) {
    case TCP_CLOSED:
    case TCP_LISTEN:
    case TCP_SYN_SENT:
        /* Nothing to finish. LISTEN teardown also destroys embryonic / acceptq
         * children; a parked CONNECT on a SYN_SENT socket is ECONNABORTED by
         * soc_destroy. Complete r (not parked on s) FIRST, then soc_destroy(s). */
        soc_complete(r, NSF_RETOK, 0);
        soc_destroy(s);
        return NSF_CLOSE_OWNED;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
        tcb->flags |= (UCHAR)TCB_F_APPCLOSED;
        tcp_emit(tcb, (USHORT)(TCP_FL_FIN | TCP_FL_ACK), 0);
        tcb->snd_nxt = tcb->snd_nxt + 1u;           /* our FIN consumes a sequence   */
        tcb->state   = (UCHAR)TCP_FIN_WAIT_1;
        soc_complete(r, NSF_RETOK, 0);
        return NSF_CLOSE_OWNED;
    case TCP_CLOSE_WAIT:
        tcb->flags |= (UCHAR)TCB_F_APPCLOSED;
        tcp_emit(tcb, (USHORT)(TCP_FL_FIN | TCP_FL_ACK), 0);
        tcb->snd_nxt = tcb->snd_nxt + 1u;
        tcb->state   = (UCHAR)TCP_LAST_ACK;
        soc_complete(r, NSF_RETOK, 0);
        return NSF_CLOSE_OWNED;
    default:
        /* Already closing (FIN_WAIT_*, CLOSING, LAST_ACK, TIME_WAIT): a second
         * close is a no-op; acknowledge it. */
        tcb->flags |= (UCHAR)TCB_F_APPCLOSED;
        soc_complete(r, NSF_RETOK, 0);
        return NSF_CLOSE_OWNED;
    }
}

static PROTOPS g_tcp_ops = {
    tcp_attach,     /* SOCKET  -- alloc the TCB                                   */
    NULL,           /* BIND    -- framework records the name (protocol-independent)*/
    tcp_connect,    /* CONNECT -- active open (M4-2)                              */
    tcp_listen,     /* LISTEN  -- passive open enable (M4-2)                      */
    NULL,           /* SEND    -- EOPNOTSUPP (M4-3)                               */
    NULL,           /* RECV    -- EOPNOTSUPP (M4-3)                               */
    tcp_close,      /* CLOSE   -- graceful FIN teardown (M4-2)                    */
    tcp_detach,     /* final resource release -> tcp_destroy                      */
    tcp_accept      /* ACCEPT  -- hand back an established child (M4-2)           */
};

/* -- init / introspection ----------------------------------------------------- */

int nsftcp_reserve(UINT count)
{
    USHORT objsize;

    if (count == 0u || count > NSFTCP_MAX_TCB) {
        count = NSFTCP_MAX_TCB;
    }
    /* Init-window only (mm_pool_create ABENDs after the seal). The slot is the
     * spec-13.2 256-byte pool object on the TARGET (the struct is 188 B, so 256
     * gives the M5 growth reserve), but it MUST also physically hold a TCB on the
     * HOST, where 8-byte pointers and the four 48-byte TMRs inflate sizeof(TCB)
     * to ~328 B -- larger than 256. So the slot is max(sizeof(TCB), 256): 256 on
     * target, the exact struct size on host. A fixed 256 would overflow the slot
     * on the host (the SOCKET pool avoids this by passing sizeof(SOCKCB); the TCB
     * keeps the 256 target reserve where it fits). */
    objsize = (sizeof(TCB) > (size_t)NSFTCP_TCB_OBJSIZE)
            ? (USHORT)sizeof(TCB)
            : (USHORT)NSFTCP_TCB_OBJSIZE;
    g_tcbpool = mm_pool_create("TCPTCB  ", objsize, (USHORT)count);
    return (g_tcbpool == NULL) ? 1 : 0;
}

void nsftcp_init(void)
{
    tcp_stats_init();
    q_init(&g_tcblist, (USHORT)NSFTCP_MAX_TCB);
    /* Register the inbound demux handler with NSFIP (after nsfip_init/config).
     * The SOCKET-side registration (nsfreq_register_proto) is the caller's, so
     * NSFTCP takes no upward dependency on NSFREQ. */
    (void)nsfip_register_proto((UCHAR)NSFIP_PROTO_TCP, nsftcp_input);
}

PROTOPS *nsftcp_protops(void)
{
    return &g_tcp_ops;
}

#if NSF_DEBUG
UINT nsftcp_debug_inuse(void)
{
    MMSTATS st;

    if (g_tcbpool == NULL) {
        return 0u;
    }
    mm_stats(g_tcbpool, &st);
    return st.inuse;
}
#endif
