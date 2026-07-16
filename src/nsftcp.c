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
#include "nsftmr.h"             /* tmr_cancel (teardown)                      */
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
static STSCTR *tcp_timewaitreclaim, *tcp_hdrerr;
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
    tcp_timewaitreclaim = sts_register("NSFTCP", "timewaitreclaim");
    tcp_hdrerr          = sts_register("NSFTCP", "hdrerr");     /* private drop   */
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

/* -- per-state handlers (RFC 793 pp. 65-76 "SEGMENT ARRIVES") ------------------
 * M4-1 STUBS. Each lays out the RFC's ordered steps as comments so the structure
 * can be read against the RFC, and drops the segment; the behavior (state
 * transitions, ACK/SYN emission, data, timers) lands in M4-2/M4-3. None of these
 * is reachable in M4-1 (no TCB leaves CLOSED until the M4-2 connect/listen ops),
 * so they add no live counter yet -- the RFC step that drops a segment gets its
 * own §13.5 counter (dupack, oooseg, ...) when that step is implemented. */

/* RFC 793 p.65 "If the state is LISTEN". */
static void tcp_listen_input(TCB *tcb, const TCPSEG *seg)
{
    /* 1. check for an RST  -- ignore (a RST is meaningless in LISTEN).          */
    /* 2. check for an ACK  -- any ACK is bad: send <SEQ=SEG.ACK><CTL=RST>.      */
    /* 3. check for a SYN   -- passive open: seed IRS/RCV.NXT, ISS/SND.NXT,      */
    /*                         -> SYN_RCVD, send SYN,ACK, arm the rexmit timer.  */
    /* 4. any other control/text -- drop.                                        */
    (void)tcb;
    (void)seg;
}

/* RFC 793 p.66 "If the state is SYN-SENT". */
static void tcp_synsent_input(TCB *tcb, const TCPSEG *seg)
{
    /* 1. check the ACK bit -- an unacceptable ACK (outside ISS..SND.NXT) that   */
    /*                         is not a RST draws <SEQ=SEG.ACK><CTL=RST>, drop.  */
    /* 2. check the RST bit -- an acceptable RST aborts the connect (ECONNREFUSED)*/
    /* 3. check security    -- not modeled.                                      */
    /* 4. check the SYN bit -- SYN(+ACK): set RCV.NXT/IRS; if ACK'd -> ESTABLISHED*/
    /*                         (send ACK), else simultaneous open -> SYN_RCVD.   */
    /* 5. neither SYN nor RST -- drop.                                           */
    (void)tcb;
    (void)seg;
}

/* RFC 793 pp. 69-76, the synchronized states (SYN_RCVD, ESTABLISHED, FIN_WAIT_1,
 * FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT). */
static void tcp_synchronized_input(TCB *tcb, const TCPSEG *seg)
{
    /* 1. check the sequence number -- unacceptable (outside the receive window) */
    /*                                 -> ACK (unless RST) and drop.             */
    /* 2. check the RST bit          -- reset the connection per state.          */
    /* 3. check security             -- not modeled.                            */
    /* 4. check the SYN bit          -- a SYN in the window is an error -> RST.  */
    /* 5. check the ACK field        -- advance SND.UNA; window update; the      */
    /*                                  state-specific ACK processing.           */
    /* 6. check the URG bit          -- not modeled (no urgent data).           */
    /* 7. process the segment text   -- deliver in-order data to the rxq (M4-3). */
    /* 8. check the FIN bit          -- advance RCV.NXT past FIN; state -> ...    */
    (void)tcb;
    (void)seg;
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

static PROTOPS g_tcp_ops = {
    tcp_attach,     /* SOCKET  -- alloc the TCB                                   */
    NULL,           /* BIND    -- framework records the name (M4-2 real bind)     */
    NULL,           /* CONNECT -- EOPNOTSUPP (M4-2)                               */
    NULL,           /* LISTEN  -- EOPNOTSUPP (M4-2)                               */
    NULL,           /* SEND    -- EOPNOTSUPP (M4-3)                               */
    NULL,           /* RECV    -- EOPNOTSUPP (M4-3)                               */
    NULL,           /* CLOSE   -- RQ_CLOSE goes straight to soc_destroy (NSFREQ)  */
    tcp_detach      /* final resource release -> tcp_destroy                      */
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
