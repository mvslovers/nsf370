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
    USHORT payoff;              /* bytes from b->data to the payload (M4-3):    */
                                /* IP ihl + TCP data-offset -- the trim-in-    */
                                /* place head-strip for the receive path       */
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
static STSCTR *tcp_timewaitreclaim, *tcp_hdrerr, *tcp_datadrop, *tcp_rxfull;
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
    /* private (M4-3): data-bearing text dropped in a state that legitimately
     * drops it (SYN_RCVD before data / a closing state past the peer's FIN) --
     * NOT in-window ESTABLISHED data, which is now delivered (ADR-0032). */
    tcp_datadrop        = sts_register("NSFTCP", "datadrop");
    /* private (M4-3): an in-order segment dropped because the rxq PBUF bound was
     * hit before the byte window closed (mirrors UDP rxfull; ADR-0032). */
    tcp_rxfull          = sts_register("NSFTCP", "rxfull");
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
static void tcp_output(TCB *tcb);               /* the data/FIN transmit clock */
static void tcp_send_resume(TCB *tcb);          /* refill a parked SEND on ACK */
static void tcp_sndq_free(TCB *tcb, UINT nbytes);
/* M4-4: retransmission + zero-window persist timers (ADR-0033). */
static void tcp_conn_abort(TCB *tcb, INT err);  /* give-up teardown (ETIMEDOUT) */
static void tcp_timers_update(TCB *tcb);        /* reconcile rexmit/persist arming */
static void tcp_rexmit_expire(void *arg);
static void tcp_persist_expire(void *arg);

/* Build and send a control segment for `tcb` (SYN / SYN|ACK / ACK / FIN / RST --
 * no data payload in M4-2), optionally carrying an MSS option (SYN and SYN|ACK).
 * SEQ is ISS for a SYN-bearing segment (the SYN occupies ISS) and SND.NXT
 * otherwise; ACK carries RCV.NXT; the window is RCV.WND. A FRESH PBUF is built
 * and handed to nsfip_output (which then owns it) or freed on an early error --
 * single owner (spec 3.4). Returns 0 on success. The caller adjusts SND.NXT for a
 * sequence-consuming flag (SYN / FIN) around this call. */
static int tcp_emit_seq(TCB *tcb, USHORT flags, int want_mss, UINT seq)
{
    SOCKCB *s = tcb->sock;
    PBUF   *b;
    UCHAR  *p;
    USHORT  hlen = (USHORT)NSFTCP_HDRLEN;
    USHORT  win, ck;
    UINT    ackno, seed;

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

/* Emit a control segment at the natural sequence: ISS for a SYN-bearing segment
 * (the SYN occupies ISS), SND.NXT otherwise. The M4-4 FIN retransmit uses
 * tcp_emit_seq directly to re-emit the FIN at its own (already-consumed)
 * sequence, SND.UNA, without touching SND.NXT. */
static int tcp_emit(TCB *tcb, USHORT flags, int want_mss)
{
    UINT seq = (flags & TCP_FL_SYN) ? tcb->iss : tcb->snd_nxt;
    return tcp_emit_seq(tcb, flags, want_mss, seq);
}

/* -- data path: send queue + segmentized output (M4-3, ADR-0032) --------------
 * Send is COPY-ON-TRANSMIT: sndq holds the application payload (front byte ==
 * SND.UNA); each transmission COPIES the slice into a fresh wire PBUF. Receive is
 * TRIM-IN-PLACE: an accepted segment is queued on the socket rxq as-is. Both keep
 * PBUFs single-owner (spec 3.4). All executive-task, run-to-completion. */

/* Append up to `n` bytes from `src` onto the sndq tail (into a tail PBUF's spare
 * capacity, else a fresh BUFLARGE), returning the count actually appended (< n on
 * budget/pool exhaustion, both normal). Maintains sndq_bytes; append is at the
 * tail, so the front byte stays SND.UNA. */
static USHORT tcp_sndq_append(TCB *tcb, const UCHAR *src, USHORT n)
{
    USHORT done = 0u;

    while (done < n) {
        PBUF *tail = Q_EMPTY(&tcb->sndq) ? NULL
                   : Q_ENTRY(tcb->sndq.head.prev, PBUF, q);

        if (tail == NULL || tail->len >= tail->size) {
            PBUF *nb = buf_alloc((USHORT)NSFBUF_LARGE_DATA);   /* force BUFLARGE  */
            if (nb == NULL || q_enq(&tcb->sndq, &nb->q) != 0) {
                if (nb != NULL) {
                    buf_free(nb);
                }
                break;                          /* pool/queue full -> partial      */
            }
            continue;                           /* retry with the new tail         */
        }
        {
            USHORT room = (USHORT)(tail->size - tail->len);
            USHORT want = (USHORT)(n - done);
            USHORT take = (want < room) ? want : room;
            USHORT got  = buf_copyin(tail, src + done, take);
            if (got == 0u) {
                break;                          /* defensive: no forward progress  */
            }
            done            = (USHORT)(done + got);
            tcb->sndq_bytes = tcb->sndq_bytes + (UINT)got;
        }
    }
    return done;
}

/* Copy `len` bytes starting at byte offset `off` within the sndq into `dst` (a
 * fresh wire PBUF), appending via buf_copyin -- across sndq-element boundaries.
 * off/off+len lie within sndq_bytes (off = SND.NXT-SND.UNA, len = the seg size).*/
static void tcp_sndq_slice(TCB *tcb, UINT off, PBUF *dst, USHORT len)
{
    QELEM *e;
    UINT   skip = off;
    USHORT left = len;

    for (e = tcb->sndq.head.next; e != &tcb->sndq.head && left > 0u; e = e->next) {
        PBUF  *sp = Q_ENTRY(e, PBUF, q);
        USHORT from, avail, take;

        if (skip >= (UINT)sp->len) {
            skip -= (UINT)sp->len;              /* whole element before the slice  */
            continue;
        }
        from  = (USHORT)skip;                   /* skip < sp->len here             */
        avail = (USHORT)(sp->len - from);
        take  = (left < avail) ? left : avail;
        (void)buf_copyin(dst, sp->data + from, take);
        skip  = 0u;
        left  = (USHORT)(left - take);
    }
}

/* Free `nbytes` of ACKed data off the FRONT of the sndq (front byte == SND.UNA):
 * drop fully-acked PBUFs, buf_trim_head a partially-acked front. `nbytes` is the
 * DATA acked (the caller clamps out any SYN/FIN sequence -- not in the sndq). */
static void tcp_sndq_free(TCB *tcb, UINT nbytes)
{
    while (nbytes > 0u && !Q_EMPTY(&tcb->sndq)) {
        PBUF *front = Q_ENTRY(tcb->sndq.head.next, PBUF, q);

        if ((UINT)front->len <= nbytes) {
            (void)q_deq(&tcb->sndq);
            nbytes          -= (UINT)front->len;
            tcb->sndq_bytes  = tcb->sndq_bytes - (UINT)front->len;
            buf_free(front);
        } else {
            (void)buf_trim_head(front, (USHORT)nbytes);
            tcb->sndq_bytes  = tcb->sndq_bytes - nbytes;
            nbytes           = 0u;
        }
    }
}

/* Emit ONE data segment: `len` bytes copied from the sndq at byte offset `off`
 * (off = seq - SND.UNA), header at `seq`, ACK always set (carries RCV.NXT/WND),
 * PSH on the last buffered byte. Builds a FRESH wire PBUF and hands it to
 * nsfip_output (which owns it) or frees it. Returns the bytes actually emitted
 * (buf_copyin may clamp `len` to the wire PBUF capacity, so a huge MSS still fits
 * -- the remainder rides the next segment) or 0 on a build/route failure, in
 * which case SND.NXT is NOT advanced (copy-on-transmit retry, ADR-0032). */
static USHORT tcp_data_emit(TCB *tcb, UINT off, UINT seq, USHORT len)
{
    SOCKCB *s    = tcb->sock;
    USHORT  hlen = (USHORT)NSFTCP_HDRLEN;
    PBUF   *b;
    UCHAR  *p;
    USHORT  actual, win, ck;
    UCHAR   flags;
    UINT    seed;

    b = buf_alloc(len);                         /* payload-sized (class by len)   */
    if (b == NULL) {
        return 0u;
    }
    tcp_sndq_slice(tcb, off, b, len);           /* COPY the slice out of the sndq  */
    actual = buf_chain_len(b);                  /* <= len if capacity clamped      */
    if (actual == 0u || buf_prepend(b, hlen) != 0) {
        buf_free(b);
        return 0u;
    }
    flags = (UCHAR)(TCP_FL_ACK |
                    ((off + (UINT)actual >= tcb->sndq_bytes) ? TCP_FL_PSH : 0u));
    p   = b->data;
    win = (tcb->rcv_wnd > 0xFFFFu) ? 0xFFFFu : (USHORT)tcb->rcv_wnd;
    put16(p + 0,  s->lport);
    put16(p + 2,  s->fport);
    put32(p + 4,  seq);
    put32(p + 8,  tcb->rcv_nxt);
    p[12] = (UCHAR)((hlen / 4u) << 4);          /* data offset = 5 words           */
    p[13] = flags;
    put16(p + 14, win);
    put16(p + 16, 0u);                          /* checksum (computed below)       */
    put16(p + 18, 0u);                          /* urgent pointer                  */
    seed = tcp_pseudo_seed(s->laddr, s->faddr, (USHORT)(hlen + actual));
    ck   = in_cksum_fold(in_cksum_partial(b, 0u, (USHORT)(hlen + actual), seed));
    put16(p + 16, ck);                          /* TCP: a computed 0 stays 0       */

    TRC(TCP, "DATA :%u -> :%u seq=%08X len=%u", (unsigned)s->lport,
        (unsigned)s->fport, (unsigned)seq, (unsigned)actual);

    if (nsfip_output(b, s->laddr, s->faddr, (UCHAR)NSFIP_PROTO_TCP,
                     (UCHAR)NSFIP_TTL_DEFAULT) != 0) {
        return 0u;                              /* freed b + counted; NXT unmoved  */
    }
    return actual;
}

/* Transmit clock: push as much sndq data as the peer's window allows in
 * MSS-bounded segments, then the FIN if the app has closed and all data is out.
 * Called after RQ_SEND appends, after an ACK advances SND.UNA / opens the window,
 * and after the close path queues a FIN. */
static void tcp_output(TCB *tcb)
{
    for (;;) {
        UINT   sent = tcb->snd_nxt - tcb->snd_una;             /* data in flight   */
        UINT   avail;
        INT    usable;
        USHORT seg, out;

        avail  = (sent < tcb->sndq_bytes) ? (tcb->sndq_bytes - sent) : 0u;
        usable = (INT)(tcb->snd_una + tcb->snd_wnd - tcb->snd_nxt);   /* signed     */
        if (avail == 0u || usable <= 0) {
            break;                              /* nothing to send / window closed  */
        }
        seg = (USHORT)((avail < (UINT)usable) ? avail : (UINT)usable);
        if (seg > tcb->mss) {
            seg = tcb->mss;
        }
        out = tcp_data_emit(tcb, sent, tcb->snd_nxt, seg);
        if (out == 0u) {
            break;                              /* build/route failed -> retry later */
        }
        tcb->snd_nxt = tcb->snd_nxt + (UINT)out;
    }

    /* FIN (RFC CLOSE): the app has closed (FINQ) and every data byte is sent
     * (SND.NXT reached SND.UNA + sndq_bytes). Guarded by FINSENT so it emits ONCE
     * -- the equality goes true AGAIN when the peer's ACK advances SND.UNA up to
     * SND.NXT, which must NOT re-send the FIN. A build/route failure leaves
     * FINSENT clear, so the FIN retries on the next tcp_output (copy-on-transmit).*/
    if ((tcb->flags & TCB_F_FINQ) && !(tcb->flags & TCB_F_FINSENT) &&
        tcb->snd_nxt == tcb->snd_una + tcb->sndq_bytes) {
        if (tcp_emit(tcb, (USHORT)(TCP_FL_FIN | TCP_FL_ACK), 0) == 0) {
            tcb->snd_nxt = tcb->snd_nxt + 1u;   /* our FIN consumes a sequence      */
            tcb->flags  |= (UCHAR)TCB_F_FINSENT;
        }
    }

    /* Reconcile the retransmit / persist timers with the new send state (M4-4):
     * data/SYN/FIN now in flight -> arm rexmit; blocked behind a zero window ->
     * arm persist; nothing outstanding -> cancel both. */
    tcp_timers_update(tcb);
}

/* Refill a parked blocking SEND from where it left off (r->p3 = bytes already
 * copied), as ACKs free budget; complete it once the whole request is buffered.
 * Called from tcp_process_ack after tcp_sndq_free opens room. */
static void tcp_send_resume(TCB *tcb)
{
    SOCKCB *s = tcb->sock;
    NSFRQE *r;
    UINT    total, already, room, take;
    USHORT  got;

    if (s == NULL || s->pend_send == NULL) {
        return;
    }
    r       = s->pend_send;
    total   = r->ulen;
    already = r->p3;
    room    = (tcb->sndq_bytes >= (UINT)NSFTCP_SNDBUF) ? 0u
            : (UINT)NSFTCP_SNDBUF - tcb->sndq_bytes;
    take    = (total > already) ? (total - already) : 0u;
    if (take > room) {
        take = room;
    }
    got     = (take > 0u)
            ? tcp_sndq_append(tcb, (const UCHAR *)r->ubuf + already, (USHORT)take)
            : 0u;
    r->p3   = already + (UINT)got;
    tcp_output(tcb);
    if (r->p3 >= total) {
        soc_complete(r, (INT)total, 0);         /* clears pend_send (soc_complete)  */
    }
}

/* Copy STREAM bytes out of the rxq into r's user buffer (up to r->ulen), freeing
 * fully-consumed PBUFs and head-trimming a partial one, grow rcv_wnd by the
 * drained bytes (clamped to the budget), and complete r with the count. Any bytes
 * available satisfy a blocking recv (stream semantics). Returns the byte count.
 * NO ACK here -- the caller decides (inbound path piggybacks one; the app op
 * sends a pure window update only if the window reopened). */
static UINT tcp_recv_drain_to(TCB *tcb, NSFRQE *r)
{
    SOCKCB *s   = tcb->sock;
    UCHAR  *dst = (UCHAR *)r->ubuf;
    UINT    want = r->ulen;
    UINT    got  = 0u;

    while (got < want && !Q_EMPTY(&s->rxq)) {
        PBUF  *front = Q_ENTRY(s->rxq.head.next, PBUF, q);
        USHORT flen  = buf_chain_len(front);
        USHORT take  = (USHORT)(((want - got) < (UINT)flen) ? (want - got)
                                                            : (UINT)flen);

        if (dst != NULL && take > 0u) {
            (void)buf_copyout(front, dst + got, take);
        }
        got = got + (UINT)take;
        if (take >= flen) {
            (void)q_deq(&s->rxq);
            buf_free(front);                    /* PBUF fully consumed             */
        } else {
            /* Partial: keep the remainder. An rxq element is ONE PBUF (one
             * inbound segment trimmed in place -- never a chain), so buf_trim_head
             * (bounded by len, not chainlen) trims the whole remainder here. */
            (void)buf_trim_head(front, take);
        }
    }
    tcb->rcv_wnd = tcb->rcv_wnd + got;
    if (tcb->rcv_wnd > (UINT)NSFTCP_RCVWND_DEFAULT) {
        tcb->rcv_wnd = (UINT)NSFTCP_RCVWND_DEFAULT;
    }
    soc_complete(r, (INT)got, 0);               /* clears pend_recv (soc_complete)  */
    return got;
}

/* Segment text (RFC 793 p.74). Accept in-order data at RCV.NXT into the socket
 * rxq (trim-in-place, ownership transfers -> *kept), head-trimming an overlap
 * already received (a retransmission) and tail-trimming beyond the window; a gap
 * above RCV.NXT is dropped + oooseg + dup-ACKed (oooq is M5), an rxq-full drop is
 * counted rxfull + re-ACKed, data in a non-receiving state is datadrop. On any
 * accept it ACKs the new RCV.NXT and delivers to a parked recv. */
static void tcp_recv_data(TCB *tcb, const TCPSEG *seg, PBUF *b, int *kept)
{
    SOCKCB *s = tcb->sock;
    UINT    below;
    USHORT  newlen, have;

    if (tcb->state != (UCHAR)TCP_ESTABLISHED &&
        tcb->state != (UCHAR)TCP_FIN_WAIT_1 &&
        tcb->state != (UCHAR)TCP_FIN_WAIT_2) {
        tcpc(tcp_datadrop);                     /* closing/embryonic state: spurious */
        tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);
        return;
    }
    if (TCP_SEQ_GT(seg->seq, tcb->rcv_nxt)) {
        tcpc(tcp_oooseg);                       /* a gap: oooq is M5 -> drop+dup-ACK */
        tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);
        return;
    }
    below = tcb->rcv_nxt - seg->seq;            /* bytes at/below RCV.NXT (overlap) */
    if (below >= (UINT)seg->dlen) {
        tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* pure duplicate -> ACK, drop      */
        return;
    }
    newlen = (USHORT)((UINT)seg->dlen - below);
    if ((UINT)newlen > tcb->rcv_wnd) {
        newlen = (USHORT)tcb->rcv_wnd;          /* clip past the advertised window  */
    }
    if (newlen == 0u) {
        tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* window full -> ACK, drop         */
        return;
    }
    (void)buf_trim_head(b, (USHORT)(seg->payoff + (USHORT)below)); /* -> RCV.NXT byte*/
    have = buf_chain_len(b);
    if (have > newlen) {
        (void)buf_trim_tail(b, (USHORT)(have - newlen));
    }
    if (q_enq(&s->rxq, &b->q) != 0) {           /* rxq PBUF bound hit -> rxfull     */
        tcpc(tcp_rxfull);
        tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* re-ACK old RCV.NXT               */
        return;                                 /* *kept stays 0 -> caller frees b  */
    }
    *kept        = 1;                           /* rxq owns b now                   */
    tcb->rcv_nxt = tcb->rcv_nxt + (UINT)newlen;
    tcb->rcv_wnd = (tcb->rcv_wnd > (UINT)newlen) ? (tcb->rcv_wnd - (UINT)newlen) : 0u;

    if (s->pend_recv != NULL) {
        (void)tcp_recv_drain_to(tcb, s->pend_recv);   /* deliver + grow rcv_wnd     */
    }
    tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);       /* ONE ACK: final RCV.NXT/WND       */

    /* Read-ready poke AT THE END (post-drain), unconditional (never gated on
     * pend_recv -- that is NULL exactly when a SELECT is the waiter): reflects the
     * FINAL rxq state, so a concurrent parked RECV that just consumed this segment
     * leaves the rxq empty and the SELECT correctly reports not-ready (ADR-0035). */
    soc_notify_ready(s, (UCHAR)SEL_READ);
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

/* -- retransmission + zero-window persist timers (M4-4, ADR-0033) --------------
 * Fixed RTO with exponential backoff. srtt/rttvar stay 0 (Karn + adaptive RTT is
 * M5, spec 13.1). The rexmit and persist timers are NEVER armed together: rexmit
 * governs whenever sequence space is in flight (SND.UNA < SND.NXT); persist
 * governs when the sender is paused on a zero window with nothing in flight. That
 * mutual exclusion (enforced by tcp_timers_update) keeps the give-up teardown
 * safe to free the TCB from a timer callback: only the firing timer is queued for
 * this TCB, exactly as tcp_2msl_expire relies on. `backoff` is the shared
 * consecutive-expiry / interval-shift count (the two timers cannot both be
 * armed). `rto` is the current interval in ticks. */

/* Current interval = base << backoff, capped at the 64 s max. The shift is capped
 * at 6 (10 << 6 = 640 = the max) so a large backoff never triggers shift UB
 * (persist grows backoff without an upper bound while a peer is silent). */
static UINT tcp_rto_compute(UCHAR backoff)
{
    UINT shift = (backoff > 6u) ? 6u : (UINT)backoff;
    UINT t     = (UINT)NSFTCP_RTO_TICKS << shift;

    return (t > (UINT)NSFTCP_RTO_MAX_TICKS) ? (UINT)NSFTCP_RTO_MAX_TICKS : t;
}

/* Reconcile the rexmit / persist timers with the current send state. Called after
 * every event that moves SND.UNA / SND.NXT / SND.WND / the sndq (tcp_output tail,
 * the SYN emitters, and tcp_process_ack). Idempotent by "arm only if idle", so a
 * doubled call in one event never restarts a running timer; a progress ACK forces
 * a fresh RTO by cancelling t_rexmit first (in tcp_process_ack) so this re-arms at
 * the base interval. */
static void tcp_timers_update(TCB *tcb)
{
    if (TCP_SEQ_LT(tcb->snd_una, tcb->snd_nxt)) {
        /* Sequence space in flight -> rexmit governs; persist must be off. If we
         * were persisting (window just opened, data now on the wire), start the
         * retransmit escalation fresh. */
        if (tcb->t_persist.state == (UCHAR)TMR_PENDING) {
            tmr_cancel(&tcb->t_persist);
            tcb->backoff = 0u;
            tcb->rto     = (USHORT)NSFTCP_RTO_TICKS;
        }
        if (tcb->t_rexmit.state == (UCHAR)TMR_IDLE) {
            if (tcb->rto == 0u) {
                tcb->rto = (USHORT)NSFTCP_RTO_TICKS;
            }
            tmr_start(&tcb->t_rexmit, (UINT)tcb->rto, tcp_rexmit_expire, tcb);
        }
    } else {
        /* Nothing in flight. Persist iff data is waiting behind a zero window
         * (a shrunk-to-zero peer window with unsent bytes); otherwise both off. */
        tmr_cancel(&tcb->t_rexmit);
        if (tcb->sndq_bytes > 0u && tcb->snd_wnd == 0u) {
            if (tcb->t_persist.state == (UCHAR)TMR_IDLE) {
                tcb->backoff = 0u;
                tcb->rto     = (USHORT)NSFTCP_RTO_TICKS;
                tcb->flags  &= (UCHAR)~TCB_F_PROBEACK;   /* fresh probe episode  */
                tmr_start(&tcb->t_persist, (UINT)tcb->rto, tcp_persist_expire, tcb);
            }
        } else {
            tmr_cancel(&tcb->t_persist);
        }
    }
}

/* Retransmit EXACTLY ONE segment starting at SND.UNA (RFC 1122 §4.2.3.1 go-back-N
 * restraint -- never re-blast the whole flight). The oldest outstanding sequence
 * is a SYN (SYN_SENT / SYN_RCVD), or data (copy-on-transmit the SND.UNA slice,
 * ADR-0032), or -- data all acked -- our FIN. A retransmitted segment is
 * byte-identical in payload and sequence to the original; only the ack/window
 * fields reflect newer state. SND.NXT is never moved here (the sequence is
 * already counted); TCB_F_FINSENT stays set across a FIN rexmit (the flag means
 * "the FIN occupies sequence space", not "sent once"). */
static void tcp_rexmit_one(TCB *tcb)
{
    switch (tcb->state) {
    case TCP_SYN_SENT:
        (void)tcp_emit(tcb, (USHORT)TCP_FL_SYN, 1);            /* seq = ISS = UNA  */
        return;
    case TCP_SYN_RCVD:
        (void)tcp_emit(tcb, (USHORT)(TCP_FL_SYN | TCP_FL_ACK), 1);
        return;
    default:
        break;
    }
    if (tcb->sndq_bytes > 0u) {
        USHORT len = (tcb->sndq_bytes < (UINT)tcb->mss)
                   ? (USHORT)tcb->sndq_bytes : tcb->mss;
        (void)tcp_data_emit(tcb, 0u, tcb->snd_una, len);       /* seq = SND.UNA    */
        return;
    }
    if (tcb->flags & TCB_F_FINSENT) {
        /* Only our FIN is outstanding: re-emit it at SND.UNA (its own sequence,
         * == SND.NXT-1 once the data drained). SND.NXT unchanged, FINSENT kept. */
        (void)tcp_emit_seq(tcb, (USHORT)(TCP_FL_FIN | TCP_FL_ACK), 0, tcb->snd_una);
    }
}

/* Retransmit timer expiry. After NSFTCP_RTO_MAXTRIES no-progress expiries the
 * connection is dead (NSF_ETIMEDOUT + teardown -- a SYN_SENT give-up is the
 * classic connect timeout). Otherwise retransmit one segment, count it, double
 * the backoff (capped), and re-arm. Freeing the TCB from here is safe: no sibling
 * timer of this TCB is armed (persist is mutually exclusive; keep is M5; 2msl is
 * TIME_WAIT-only, which carries no unacked data) and nsftmr_run detached +
 * IDLE'd t_rexmit before this call. */
static void tcp_rexmit_expire(void *arg)
{
    TCB *tcb = (TCB *)arg;

    if (tcb->backoff >= (UCHAR)NSFTCP_RTO_MAXTRIES) {
        tcp_conn_abort(tcb, NSF_ETIMEDOUT);
        return;
    }
    tcp_rexmit_one(tcb);
    tcpc(tcp_rexmit);
    tcb->backoff = (UCHAR)(tcb->backoff + 1u);
    tcb->rto     = (USHORT)tcp_rto_compute(tcb->backoff);
    tmr_start(&tcb->t_rexmit, (UINT)tcb->rto, tcp_rexmit_expire, tcb);
}

/* Persist (zero-window probe) timer expiry (RFC 793 §3.7 / RFC 1122 §4.2.2.17).
 * Send ONE byte of sndq data beyond the closed window, count wndprobe, back off,
 * re-arm. Persist does NOT advance SND.NXT (the probe byte stays on the sndq and
 * is sent for real once the window reopens), so the sender bookkeeping keeps
 * "nothing in flight" and persist stays the governing timer.
 *
 * Persist never gives up as long as the peer answers probes (a zero-window ACK
 * sets TCB_F_PROBEACK via the inbound path); only if probing draws NO segment at
 * all through the whole backoff escalation does it fall back to the rexmit
 * give-up discipline (documented choice, ADR-0033). backoff still grows on every
 * probe so the intervals visibly back off even while the peer keeps ACKing. */
static void tcp_persist_expire(void *arg)
{
    TCB *tcb = (TCB *)arg;
    UINT off = tcb->snd_nxt - tcb->snd_una;      /* 0 while persisting            */

    if (tcb->backoff >= (UCHAR)NSFTCP_RTO_MAXTRIES &&
        !(tcb->flags & TCB_F_PROBEACK)) {
        tcp_conn_abort(tcb, NSF_ETIMEDOUT);      /* silent peer -> dead           */
        return;
    }
    if (tcb->sndq_bytes > off) {
        (void)tcp_data_emit(tcb, off, tcb->snd_nxt, 1u);   /* 1 byte, no NXT move */
        tcpc(tcp_wndprobe);
    }
    tcb->backoff = (UCHAR)(tcb->backoff + 1u);
    tcb->rto     = (USHORT)tcp_rto_compute(tcb->backoff);
    tmr_start(&tcb->t_persist, (UINT)tcb->rto, tcp_persist_expire, tcb);
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

/* Abort a connection with a specific errno: complete every parked request with
 * `err` FIRST -- that clears each pend slot, so the following soc_destroy will not
 * re-complete it with the generic ECONNABORTED -- then drive the ONE end-of-life
 * teardown. Shared by the received-RST path (NSF_ECONNRESET) and the M4-4
 * retransmit / persist give-up (NSF_ETIMEDOUT). */
static void tcp_conn_abort(TCB *tcb, INT err)
{
    SOCKCB *s = tcb->sock;

    if (s != NULL) {
        if (s->pend_connect != NULL) {
            soc_complete(s->pend_connect, NSF_RETERR, err);
        }
        if (s->pend_recv != NULL) {
            soc_complete(s->pend_recv, NSF_RETERR, err);
        }
        if (s->pend_accept != NULL) {
            soc_complete(s->pend_accept, NSF_RETERR, err);
        }
        if (s->pend_send != NULL) {
            soc_complete(s->pend_send, NSF_RETERR, err);
        }
    }
    tcp_close_done(tcb);
}

/* A RST was received in a synchronized state (RFC 793 p.70). */
static void tcp_do_reset(TCB *tcb)
{
    tcp_conn_abort(tcb, NSF_ECONNRESET);
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
    } else if (q_enq(&ls->acceptq, &child->acceptlink) == 0) {
        child->flags |= (UCHAR)TCB_F_ONACCEPTQ;
    }
    /* else: the acceptq is unexpectedly full -- unreachable, because the SYN-time
     * backlog check bounds pending children (embryonic + queued) to the acceptq
     * size. Leave the child established but un-queued (reaped by listener teardown
     * / TERMAPI); do NOT destroy it here -- this runs mid-ACK-processing and the
     * caller (tcp_process_ack -> tcp_synchronized_input) would then touch a freed
     * TCB. */

    /* Read-ready poke on the LISTENER (ACCEPT counts as a read op): a pending
     * accept just delivered leaves the acceptq empty (SELECT reports not-ready);
     * one queued leaves it non-empty (SELECT reports the listener read-ready).
     * Unconditional, reflecting the final acceptq state (ADR-0035). */
    soc_notify_ready(ls, (UCHAR)SEL_READ);
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
    /* Start the data phase with a fresh RTO (M4-4): the handshake converges here
     * on BOTH the active (tcp_synsent_input) and passive (tcp_process_ack SYN_RCVD)
     * paths, neither of which runs the progress-ACK backoff reset -- so a SYN /
     * SYN|ACK that was retransmitted would otherwise leave a grown backoff/rto for
     * the first data send. The subsequent tcp_timers_update cancels rexmit here
     * (SYN acked -> nothing in flight), so nothing re-arms until that first send. */
    tcb->backoff = 0u;
    tcb->rto     = (USHORT)NSFTCP_RTO_TICKS;
    tcpc(tcp_established);
    if (s != NULL && s->pend_connect != NULL) {
        soc_complete(s->pend_connect, NSF_RETOK, 0);   /* active open done        */
    }
    if (tcb->listener != NULL) {
        tcp_graduate(tcb);                              /* passive open done       */
    }
    /* Write-ready poke (ADR-0035): an active open that just reached ESTABLISHED is
     * now write-ready -- a SELECT-for-write waiting on the connecting socket fires
     * (the nonblocking-CONNECT-then-select-for-write idiom). Unconditional (not
     * gated on pend_connect). The passive child is not app-visible until ACCEPT, so
     * a poke on it is a harmless no-op. */
    soc_notify_ready(s, (UCHAR)SEL_WRITE);
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
        tcp_timers_update(tcb);                 /* our SYN|ACK acked -> cancel rexmit*/
        return 1;
    }

    if (TCP_SEQ_GT(seg->ack, tcb->snd_nxt)) {
        /* A zero-window persist probe (tcp_persist_expire, ADR-0033) puts ONE byte
         * on the wire at SND.NXT but does NOT advance SND.NXT (so "nothing in
         * flight" and persist keeps governing). If the peer's window has since
         * reopened it ACCEPTS that byte and ACKs SND.NXT+1 -- a legitimate ack of a
         * byte we really sent, not of unsent data. Adopt it (account the probed
         * byte as sent) and fall through to the normal window-update + SND.UNA
         * advance + resume, so the sender comes out of persist even when the peer's
         * window-update ACK was LOST and this probe-ACK is the only signal that the
         * window reopened. Without this the probe-ACK is rejected as "unsent", the
         * window update it carries is never processed, and the sender livelocks
         * (M4-6 loss harness, ADR-0033 annotation). Only the single probed byte is
         * accepted this way (ack == SND.NXT+1); any larger jump is genuinely unsent
         * and still rejected. */
        if (seg->ack == tcb->snd_nxt + 1u &&
            tcb->t_persist.state == (UCHAR)TMR_PENDING &&
            tcb->sndq_bytes > (tcb->snd_nxt - tcb->snd_una)) {
            tcb->snd_nxt = seg->ack;            /* the probed byte is now sent     */
        } else {
            tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* ACKs something unsent -> ACK+drop*/
            return 0;
        }
    }
    /* Update the send window BEFORE any transmit below (RFC 793 p.72 step 5). The
     * progress branch advances SND.UNA and immediately re-clocks the sender
     * (send_resume -> tcp_output); if the window were still stale there, advancing
     * SND.UNA past a SHRUNK window would slide the right edge right and the sender
     * would transmit beyond it (found live via the M4-4 persist trace: a segment
     * sent past the peer's window, dropped, then retransmitted -- rexmit instead of
     * persist; test_flowctl_no_oversend). */
    tcp_update_window(tcb, seg);

    if (TCP_SEQ_GT(seg->ack, tcb->snd_una)) {
        UINT acked = seg->ack - tcb->snd_una;   /* new data / our FIN acknowledged */
        tcb->snd_una = seg->ack;
        /* Progress: reset the backoff + RTO and cancel the rexmit timer so the
         * reconciliation below re-arms it fresh for the new oldest segment
         * (RFC restart-on-ACK). backoff/rto/dupacks all clear on progress. */
        tcb->backoff = 0u;
        tcb->rto     = (USHORT)NSFTCP_RTO_TICKS;
        tcb->dupacks = 0u;
        tmr_cancel(&tcb->t_rexmit);
        /* Free the DATA portion off the sndq front (a SYN/FIN sequence is not in
         * the sndq -- clamp to sndq_bytes), then refill a parked SEND (M4-3). */
        tcp_sndq_free(tcb, (acked < tcb->sndq_bytes) ? acked : tcb->sndq_bytes);
        tcp_send_resume(tcb);
        /* Write-ready poke (ADR-0035): the ACK just freed send-budget room. Placed
         * here, after SND.UNA advances -- NOT behind tcp_send_resume's pend_send
         * early return, which is NULL exactly when a SELECT-for-write is the waiter.
         * tcp_poll gates on the actual state/room, so this is safe to fire always. */
        soc_notify_ready(tcb->sock, (UCHAR)SEL_WRITE);
    } else if (tcb->snd_una != tcb->snd_nxt && seg->dlen == 0u &&
               !(seg->flags & (TCP_FL_SYN | TCP_FL_FIN))) {
        tcb->dupacks = (UCHAR)(tcb->dupacks + 1u);   /* run length (counting only) */
        tcpc(tcp_dupack);                       /* a received duplicate ACK (M4-3)  */
    }

    /* Clock out data / the FIN with the possibly-opened window (states that still
     * transmit; a closing/destroying state is a no-op here or handled below). */
    if (tcb->state == (UCHAR)TCP_ESTABLISHED ||
        tcb->state == (UCHAR)TCP_CLOSE_WAIT ||
        tcb->state == (UCHAR)TCP_FIN_WAIT_1) {
        tcp_output(tcb);
    }

    /* Reconcile the rexmit / persist timers for EVERY surviving state (the
     * transmitting ones reconciled inside tcp_output above; the closing states
     * that do not transmit -- FIN_WAIT_2, LAST_ACK -- still need their rexmit
     * cancelled once our FIN is acked, or kept armed while it is not). M4-4. */
    tcp_timers_update(tcb);

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
    tcb->flags  |= (UCHAR)TCB_F_RCVFIN;         /* EOF: recv returns 0 from now on */
    tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);       /* ACK it                          */

    /* A recv parked on an empty rxq now sees EOF (rc=0). If data preceded the FIN
     * it was delivered by the data step, which drained the parked recv, so
     * pend_recv is NULL here and this is a no-op (data-before-EOF ordering). */
    if (tcb->sock != NULL && tcb->sock->pend_recv != NULL &&
        Q_EMPTY(&tcb->sock->rxq)) {
        soc_complete(tcb->sock->pend_recv, 0, 0);
    }
    /* EOF is level-triggered read-ready for a parked SELECT too (ADR-0035): a RECV
     * on this socket would now return 0. Poke after the pend_recv EOF completion so
     * the readiness reflects the FIN state regardless of a concurrent recv. */
    soc_notify_ready(tcb->sock, (UCHAR)SEL_READ);

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
    child->snd_wnd  = seg->wnd;                 /* the peer's advertised window     */
    child->snd_wl1  = seg->seq;
    child->snd_wl2  = child->iss;
    child->state    = (UCHAR)TCP_SYN_RCVD;
    tcpc(tcp_passiveopen);

    if (tcp_emit(child, (USHORT)(TCP_FL_SYN | TCP_FL_ACK), 1) != 0) {
        soc_destroy(cs);                        /* could not reply -> abandon child */
    } else {
        tcp_timers_update(child);               /* arm rexmit for the SYN|ACK       */
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
        /* Adopt the peer's window UNCONDITIONALLY on this first synchronizing
         * segment (RFC 793: SND.WND/WL1/WL2 <- SEG.WND/SEG.SEQ/SEG.ACK when a
         * connection becomes synchronized; the SND.WL1/WL2 comparison in
         * tcp_update_window governs only SUBSEQUENT segments). The conditional
         * update alone was a bug here: tcp_connect leaves snd_wl1 == 0, and
         * TCP_SEQ_LT(0, SEG.SEQ) wraps FALSE whenever the peer's ISS is in the
         * upper half of the sequence space (>= 2^31, ~half of all peers), so
         * snd_wnd stayed 0 and the active opener could never transmit data. The
         * passive child (tcp_passive_open) already sets these three fields
         * directly; this makes the active side match. Masked until M4-6 because
         * tsttcp used lower-half synthetic peer ISS and live active-open never
         * sent guest data. */
        tcb->snd_wnd = seg->wnd;
        tcb->snd_wl1 = seg->seq;
        tcb->snd_wl2 = seg->ack;
        if (TCP_SEQ_GT(tcb->snd_una, tcb->iss)) {
            tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);   /* our SYN acked -> ESTABLISHED */
            tcp_enter_established(tcb);
        } else {
            tcb->state = (UCHAR)TCP_SYN_RCVD;       /* simultaneous open           */
            tcp_emit(tcb, (USHORT)(TCP_FL_SYN | TCP_FL_ACK), 1);
        }
        tcp_timers_update(tcb);                     /* SYN acked -> cancel; else arm*/
        return;
    }

    /* 5. neither SYN nor RST -> drop. */
}

/* RFC 793 pp. 69-76, the synchronized states (SYN_RCVD, ESTABLISHED, FIN_WAIT_1,
 * FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT), in RFC step order. */
static void tcp_synchronized_input(TCB *tcb, const TCPSEG *seg, PBUF *b, int *kept)
{
    /* 1. sequence-number acceptability -> unacceptable: ACK (unless RST), drop. */
    if (!tcp_seq_acceptable(tcb, seg)) {
        if (!(seg->flags & TCP_FL_RST)) {
            tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);
        }
        return;
    }
    /* Any acceptable segment proves the peer is alive -> a zero-window probe will
     * be answered, so persist must not hit the silent-peer give-up (M4-4). */
    tcb->flags |= (UCHAR)TCB_F_PROBEACK;
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
    /* 6. URG -- not modeled (the urgent data is delivered inline, ADR-0032). */
    /* 7. segment text -> in-order data accepted to the socket rxq (trim-in-place;
     *    *kept set on a successful enqueue), else dropped + counted (ADR-0032). */
    if (seg->dlen > 0u) {
        tcp_recv_data(tcb, seg, b, kept);
    }
    /* 8. FIN -> advance RCV.NXT past it, ACK, transition (EOF to a parked recv). */
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
static void tcp_state_input(TCB *tcb, const TCPSEG *seg, PBUF *b, int *kept)
{
    switch (tcb->state) {
    case TCP_LISTEN:       tcp_listen_input(tcb, seg);                break;
    case TCP_SYN_SENT:     tcp_synsent_input(tcb, seg);               break;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:    tcp_synchronized_input(tcb, seg, b, kept); break;
    case TCP_CLOSED:
    default:               tcp_closed_input(seg);                     break;
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
    int          kept = 0;                      /* did a handler keep b? (M4-3)   */

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
    seg.payoff = (USHORT)((USHORT)ihl + (USHORT)thoff);         /* -> payload byte */

    TRC(TCP, "IN %u.%u.%u.%u:%u -> :%u flags=%02X seq=%08X ack=%08X len=%u",
        (unsigned)((src >> 24) & 0xFFu), (unsigned)((src >> 16) & 0xFFu),
        (unsigned)((src >> 8) & 0xFFu),  (unsigned)(src & 0xFFu),
        (unsigned)seg.sport, (unsigned)seg.dport, (unsigned)seg.flags,
        (unsigned)seg.seq, (unsigned)seg.ack, (unsigned)seg.dlen);

    /* Demux; no TCB (or a CLOSED one) -> the RFC 793 CLOSED processing (RST). */
    tcb = tcp_demux(&seg);
    if (tcb == NULL) {
        tcp_closed_input(&seg);
        buf_free(b);                            /* a closed-port seg is never kept */
        return;
    }
    tcp_state_input(tcb, &seg, b, &kept);
    if (!kept) {
        buf_free(b);                            /* not queued on an rxq -> free it */
    }
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
    tcb->rto     = NSFTCP_RTO_TICKS;            /* base RTO; srtt/rttvar M5 (0)   */
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
    tcp_timers_update(tcb);                         /* arm rexmit for the SYN        */
    if ((r->flags & (USHORT)RQ_F_NONBLOCK) != 0u) {
        /* Non-blocking CONNECT (M4-5, ADR-0035): the SYN is on the wire and the
         * connection proceeds in the background; report EINPROGRESS now and do NOT
         * park. Its completion is observed via SELECT-for-write (tcp_enter_established
         * pokes SEL_WRITE); a failure tears the socket down (SEL_DEAD -> the select
         * aborts), so no completion is lost. */
        soc_complete(r, NSF_RETERR, NSF_EINPROGRESS);
        return 0;
    }
    return soc_park(s, r, SOC_PEND_CONNECT);        /* wake on SYN|ACK / RST         */
}

/* SHUTDOWN (M4-5, ADR-0035 / conformance §3). HOW selects the direction(s):
 *  - SHUT_RD / SHUT_RDWR: mark a LOCAL EOF (TCB_F_RCVFIN) so a subsequent RECV
 *    returns 0; poke SEL_READ (EOF is read-ready). No wire action.
 *  - SHUT_WR / SHUT_RDWR: send our FIN (graceful write-side close) by the same
 *    FINQ mechanism CLOSE uses -- but WITHOUT TCB_F_APPCLOSED and without owning
 *    the socket's death: the app keeps the descriptor open for reading. The
 *    background FIN handshake / TIME_WAIT run as usual; the app's later CLOSE
 *    drives soc_destroy.
 * Synchronous: completes r and returns 0 (handled). */
static int tcp_shutdown(SOCKCB *s, NSFRQE *r)
{
    TCB *tcb = (TCB *)s->pcb;
    int  how = (int)r->p1;

    if (tcb == NULL) {
        soc_complete(r, NSF_RETERR, NSF_ENOTCONN);
        return 0;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        tcb->flags |= (UCHAR)TCB_F_RCVFIN;          /* local EOF (recv -> 0)         */
        soc_notify_ready(s, (UCHAR)SEL_READ);       /* EOF is read-ready             */
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        switch (tcb->state) {
        case TCP_ESTABLISHED:
            tcb->flags |= (UCHAR)TCB_F_FINQ;
            tcb->state  = (UCHAR)TCP_FIN_WAIT_1;
            tcp_output(tcb);                        /* FIN once the sndq drains      */
            break;
        case TCP_CLOSE_WAIT:
            tcb->flags |= (UCHAR)TCB_F_FINQ;
            tcb->state  = (UCHAR)TCP_LAST_ACK;
            tcp_output(tcb);
            break;
        default:
            break;                                  /* nothing to FIN in this state  */
        }
    }
    soc_complete(r, NSF_RETOK, 0);
    return 0;
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

/* SEND / SENDTO (RFC SEND, M4-3): copy the request's bytes into the sndq bounded
 * by the send budget, then clock them out under the peer's window. All copied ->
 * complete now (RETCODE = byte count); a blocking short copy PARKS (SOC_PEND_SEND,
 * progress cursor in r->p3) and completes when the rest is buffered as ACKs free
 * space; a non-blocking short copy returns the partial count (EWOULDBLOCK if
 * nothing fit). SEND outside ESTABLISHED / CLOSE_WAIT -> ENOTCONN (ADR-0032). */
static int tcp_send(SOCKCB *s, NSFRQE *r)
{
    TCB   *tcb = (TCB *)s->pcb;
    UINT   total, room, take;
    USHORT got;

    if (tcb == NULL ||
        (tcb->state != (UCHAR)TCP_ESTABLISHED &&
         tcb->state != (UCHAR)TCP_CLOSE_WAIT)) {
        soc_complete(r, NSF_RETERR, NSF_ENOTCONN);
        return 0;
    }
    total = r->ulen;
    room  = (tcb->sndq_bytes >= (UINT)NSFTCP_SNDBUF) ? 0u
          : (UINT)NSFTCP_SNDBUF - tcb->sndq_bytes;
    take  = (total < room) ? total : room;      /* room <= SNDBUF, so fits USHORT  */
    got   = (take > 0u)
          ? tcp_sndq_append(tcb, (const UCHAR *)r->ubuf, (USHORT)take)
          : 0u;
    tcp_output(tcb);                            /* push what the window allows      */

    if ((UINT)got >= total) {
        soc_complete(r, (INT)total, 0);         /* everything is buffered           */
        return 0;
    }
    if (r->flags & RQ_F_NONBLOCK) {
        if (got == 0u) {
            soc_complete(r, NSF_RETERR, NSF_EWOULDBLOCK);
        } else {
            soc_complete(r, (INT)got, 0);       /* partial (BSD non-blocking)       */
        }
        return 0;
    }
    r->p3 = (UINT)got;                          /* parked cursor: bytes copied so far*/
    return soc_park(s, r, SOC_PEND_SEND);       /* wake as ACKs free budget         */
}

/* RECV / RECVFROM (RFC RECEIVE, M4-3): deliver buffered stream bytes, or EOF, or
 * park. Any available bytes satisfy a blocking recv (stream semantics). A drain
 * that reopens the advertised window (from 0, or by >= 1 MSS) sends a pure
 * window-update ACK so a window-blocked peer resumes (the deadlock rule); the FIN
 * makes recv on an empty rxq return rc=0 (EOF, sticky) -- data queued ahead of the
 * FIN is delivered first. RECV before/after a receiving state -> ENOTCONN. */
static int tcp_recv(SOCKCB *s, NSFRQE *r)
{
    TCB *tcb = (TCB *)s->pcb;

    if (tcb == NULL) {
        soc_complete(r, NSF_RETERR, NSF_ENOTCONN);
        return 0;
    }
    if (!Q_EMPTY(&s->rxq)) {
        UINT wnd_before = tcb->rcv_wnd;
        UINT drained    = tcp_recv_drain_to(tcb, r);      /* copies out + completes r */

        if (drained > 0u &&
            (wnd_before == 0u || drained >= (UINT)tcb->mss)) {
            tcp_emit(tcb, (USHORT)TCP_FL_ACK, 0);         /* pure window update       */
        }
        return 0;
    }
    if (tcb->flags & TCB_F_RCVFIN) {
        soc_complete(r, 0, 0);                  /* EOF (sticky), never an errno     */
        return 0;
    }
    if (tcb->state != (UCHAR)TCP_ESTABLISHED &&
        tcb->state != (UCHAR)TCP_FIN_WAIT_1 &&
        tcb->state != (UCHAR)TCP_FIN_WAIT_2) {
        soc_complete(r, NSF_RETERR, NSF_ENOTCONN);
        return 0;
    }
    if (r->flags & RQ_F_NONBLOCK) {
        soc_complete(r, NSF_RETERR, NSF_EWOULDBLOCK);
        return 0;
    }
    return soc_park(s, r, SOC_PEND_RECV);       /* app WAITs until data / EOF        */
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
        /* FIN-after-data (RFC CLOSE, ADR-0032): queue the FIN and transition now
         * (BSD-immediate); tcp_output emits it once the sndq drains (immediately
         * if it is already empty -- byte-identical to the M4-2 teardown). */
        tcb->flags |= (UCHAR)(TCB_F_APPCLOSED | TCB_F_FINQ);
        tcb->state  = (UCHAR)TCP_FIN_WAIT_1;
        soc_complete(r, NSF_RETOK, 0);
        tcp_output(tcb);
        return NSF_CLOSE_OWNED;
    case TCP_CLOSE_WAIT:
        tcb->flags |= (UCHAR)(TCB_F_APPCLOSED | TCB_F_FINQ);
        tcb->state  = (UCHAR)TCP_LAST_ACK;
        soc_complete(r, NSF_RETOK, 0);
        tcp_output(tcb);
        return NSF_CLOSE_OWNED;
    default:
        /* Already closing (FIN_WAIT_*, CLOSING, LAST_ACK, TIME_WAIT): a second
         * close is a no-op; acknowledge it. */
        tcb->flags |= (UCHAR)TCB_F_APPCLOSED;
        soc_complete(r, NSF_RETOK, 0);
        return NSF_CLOSE_OWNED;
    }
}

/* SELECT readiness probe (M4-5, ADR-0035). Side-effect-free: report the subset of
 * `want` (SEL_READ|SEL_WRITE) the socket is ready for NOW -- never blocks, parks or
 * dequeues. Read-ready: buffered data, a pending connection on a listener (ACCEPT
 * counts as a read op), or EOF (a RECV would return 0 immediately). Write-ready: an
 * ESTABLISHED / CLOSE_WAIT connection with send-budget room (a connecting or closing
 * state is not). NSF has no exception source (TAKESOCKET unsupported), so SELECT
 * never reports one. */
static int tcp_poll(SOCKCB *s, int want)
{
    TCB *tcb = (TCB *)s->pcb;
    int  ready = 0;

    if ((want & (int)SEL_READ) != 0) {
        if (!Q_EMPTY(&s->rxq) || !Q_EMPTY(&s->acceptq)) {
            ready |= (int)SEL_READ;             /* buffered data, or pending accept*/
        } else if (tcb != NULL && (tcb->flags & (UCHAR)TCB_F_RCVFIN) != 0) {
            ready |= (int)SEL_READ;             /* EOF: recv returns 0 immediately */
        }
    }
    if ((want & (int)SEL_WRITE) != 0) {
        if (tcb != NULL &&
            (tcb->state == (UCHAR)TCP_ESTABLISHED ||
             tcb->state == (UCHAR)TCP_CLOSE_WAIT) &&
            tcb->sndq_bytes < (UINT)NSFTCP_SNDBUF) {
            ready |= (int)SEL_WRITE;            /* send would accept >= 1 byte     */
        }
    }
    return ready;
}

static PROTOPS g_tcp_ops = {
    tcp_attach,     /* SOCKET  -- alloc the TCB                                   */
    NULL,           /* BIND    -- framework records the name (protocol-independent)*/
    tcp_connect,    /* CONNECT -- active open (M4-2)                              */
    tcp_listen,     /* LISTEN  -- passive open enable (M4-2)                      */
    tcp_send,       /* SEND / SENDTO -- copy-on-transmit data path (M4-3)         */
    tcp_recv,       /* RECV / RECVFROM -- in-order stream delivery (M4-3)         */
    tcp_close,      /* CLOSE   -- graceful FIN teardown (M4-2)                    */
    tcp_detach,     /* final resource release -> tcp_destroy                      */
    tcp_accept,     /* ACCEPT  -- hand back an established child (M4-2)           */
    tcp_poll,       /* SELECT readiness probe (M4-5)                              */
    tcp_shutdown    /* SHUTDOWN -- directional close (M4-5)                       */
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
