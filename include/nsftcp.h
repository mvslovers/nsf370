#ifndef NSFTCP_H
#define NSFTCP_H
/*
 * nsftcp.h -- TCP: connection state machine, sequence processing, segment
 * send/receive (spec ch. 13).
 *
 * M4-1 SCOPE IS STRUCTURE, NOT BEHAVIOR. This milestone lays down the TCB, the
 * sequence arithmetic, the RFC 793 "SEGMENT ARRIVES" event skeleton and the ONE
 * real emitter (a RST), so every crafted segment already gets the RFC-correct
 * REACTION SHAPE (RST, drop, count) -- but no connection can be established yet
 * (that is M4-2: LISTEN/CONNECT; M4-3: the data path). tcp_input is written to
 * be read line-by-line next to RFC 793 pp. 64-76.
 *
 * TCP plugs into the same two seams UDP does (nsfudp.h), so it stays out of the
 * production NSF load module until the EZASOKET M4 set makes it reachable:
 *   - the SOCKET protocol vtable (PROTOPS, spec 10.2): nsftcp_protops() is
 *     registered with NSFREQ (nsfreq_register_proto(6, ...)) so an RQ_SOCKET of
 *     IP protocol 6 gets TCP's callbacks. In M4-1 the vtable surface is just
 *     attach (alloc the TCB) and detach (tcp_destroy); every other op is NULL,
 *     so the request dispatcher completes those verbs with NSF_EOPNOTSUPP and
 *     never abends (M4-2 fills connect/listen, M4-3 send/recv).
 *   - the IP transport-demux seam (spec 11.1): nsftcp_init registers
 *     nsftcp_input with NSFIP (nsfip_register_proto(6, ...)) so inbound
 *     protocol-6 segments demux to TCP. Until then nsfip_input's noproto path
 *     answers protocol-6 with an ICMP protocol-unreachable, which is correct --
 *     the production stack does not carry TCP in M4-1.
 *
 * CONTROL BLOCK (spec 13.2). One TCB per TCP socket, allocated in the attach
 * callback (unlike UDP, which defers its pcb to bind -- a TCP socket needs its
 * sequence/timer state from creation) and destroyed by soc_destroy via the
 * detach callback. The TCBs form a bounded list scanned by tcp_input's demux
 * (spec 13.3): the 4-tuple (foreign addr, foreign port, local addr, local port)
 * for an established connection, then a listener match on (lport, laddr|ANY).
 *
 * CHECKSUM (spec 11.5 / ADR-0028). Like UDP, the IPv4 pseudo-header is threaded
 * into the shared in_cksum as a SEED (in_cksum_partial/in_cksum_fold, no second
 * checksum routine, no PBUF overlay) -- proto 6, transport length = the whole
 * TCP segment (ip.total - ihl). UNLIKE UDP there is NO zero-checksum exemption:
 * a TCP checksum is mandatory in both directions and is always verified.
 *
 * OWNERSHIP (spec 3.4, single owner, no refcount). nsftcp_input TAKES OWNERSHIP
 * of b (frees it, or -- once the data path exists -- hands its payload on then
 * frees it). tcp_output_rst allocates a FRESH PBUF and hands it to nsfip_output
 * (which then owns it) or frees it on an early error -- never both, never
 * neither.
 *
 * SEND-QUEUE DIRECTION PIN (the copy-on-transmit rule, ADR lands with M4-3 when
 * the code exists to pin it). sndq holds application payload PBUFs until they
 * are ACKed; every (re)transmission builds a FRESH wire PBUF and COPIES the
 * slice being sent -- PBUFs stay single-owner, no reference counting (spec 3).
 * Nothing in M4-1 may be designed to hand an sndq PBUF straight to dev_send.
 */

#include "nsf.h"
#include "nsfque.h"             /* QUEUE / QELEM (tcb list, sndq, oooq)        */
#include "nsftmr.h"             /* TMR (four timers embedded in the TCB)       */
#include "nsfsoc.h"             /* SOCKCB, PROTOPS (the vtable TCP fills)       */
#include "nsfip.h"              /* IPHDR, NETDEV, NSFIP_PROTO_TCP, nsfip_output */

/* TCB list depth (spec 13.2: "default modest") and the pool object size
 * (spec 13.2: "256-byte pool object"). The struct is 188 B on target today; the
 * extra room reserves growth (M5 options: MSS/window-scale/SACK/timestamps),
 * exactly as NSFRQE_OBJSIZE reserves growth over the 64-byte frozen core. */
#define NSFTCP_MAX_TCB      32
#define NSFTCP_TCB_OBJSIZE  256

/* Fixed TCP header length (no options): ports, seq, ack, offset/flags, window,
 * checksum, urgent. */
#define NSFTCP_HDRLEN       20

/* Default MSS when no MSS option is negotiated (RFC 1122 §4.2.2.6 / RFC 879:
 * 536 = 576-byte minimum reassembly buffer - 40 bytes of IP+TCP headers) and
 * the default receive window (spec 13.2: "default rcv_wnd 4096"). */
#define NSFTCP_MSS_DEFAULT     536
#define NSFTCP_RCVWND_DEFAULT  4096

/* MSS clamp (M4-2). A peer-announced MSS is bounded to a sane range: never below
 * the RFC 879 floor, never above what one BUFLARGE data area can carry as a TCP
 * payload (BUFLARGE 2048 - IP 20 - TCP 20). The data path (M4-3) enforces it;
 * M4-2 parses + stores it. */
#define NSFTCP_MSS_MIN         88
#define NSFTCP_MSS_MAX         (NSFBUF_LARGE_DATA - NSFIP_HDR_MIN - NSFTCP_HDRLEN)

/* TCP option kinds we handle (RFC 793 §3.1). All other options are skipped by
 * their length byte; a malformed length drops the segment (never an overrun). */
#define NSFTCP_OPT_END         0    /* end of option list                       */
#define NSFTCP_OPT_NOP         1    /* no-op (single byte, no length)            */
#define NSFTCP_OPT_MSS         2    /* maximum segment size (kind, len=4, 2-byte)*/
#define NSFTCP_OPT_MSS_LEN     4

/* TIME_WAIT hold = 2*MSL. RFC 793 MSL is 2 minutes; a small-memory host stack
 * uses a shorter, TCPCONFIG-tunable MSL (M4-4/M5). Here 2MSL = 60 s at the
 * 100 ms tick (spec 6.3) -- a named constant, tunable later. */
#define NSFTCP_MSL_TICKS       300  /* 30 s per MSL (100 ms ticks)              */
#define NSFTCP_2MSL_TICKS      (2 * NSFTCP_MSL_TICKS)

/* Ephemeral local-port range for an active open without an explicit BIND (the
 * IANA dynamic range, as NSFUDP uses). */
#define NSFTCP_EPHEM_LO        49152u
#define NSFTCP_EPHEM_HI        65535u

/* TCB.flags bits (NSF-internal connection topology / lifecycle -- distinct from
 * the M5 socket options NODELAY/KEEPALIVE, which will take the high bits). */
#define TCB_F_APPCLOSED  0x01u  /* app issued CLOSE: the TCB owns its own death  */
#define TCB_F_ONACCEPTQ  0x02u  /* linked on a listener's acceptq (via acceptlink)*/

/* TCP connection states (TCB.state), RFC 793 §3.2 order. CLOSED is 0 so a
 * freshly memset TCB is CLOSED. */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RCVD     3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT   10

/* TCP control-bit flags (the low 6 bits of the data-offset/flags word, RFC 793
 * §3.1). */
#define TCP_FL_FIN  0x01u
#define TCP_FL_SYN  0x02u
#define TCP_FL_RST  0x04u
#define TCP_FL_PSH  0x08u
#define TCP_FL_ACK  0x10u
#define TCP_FL_URG  0x20u

/* Sequence-space arithmetic (RFC 793 §3.3, the classic BSD signed-difference
 * trick). Sequence numbers are unsigned 32-bit and wrap; comparisons are made
 * on the SIGNED 32-bit difference, so they are correct across the 2^32 wrap as
 * long as the two operands are within 2^31 of each other (always true for a
 * live connection's window). INT is exactly 32-bit signed on host and target
 * (nsf.h), and both are two's-complement, so the (UINT)-then-(INT) cast has the
 * same modular-then-signed meaning everywhere. Pinned by literal wrap vectors
 * (test/tsttcp.c) before anything uses them. */
#define TCP_SEQ_LT(a, b)   ((INT)((UINT)(a) - (UINT)(b)) <  0)
#define TCP_SEQ_LEQ(a, b)  ((INT)((UINT)(a) - (UINT)(b)) <= 0)
#define TCP_SEQ_GT(a, b)   ((INT)((UINT)(a) - (UINT)(b)) >  0)
#define TCP_SEQ_GEQ(a, b)  ((INT)((UINT)(a) - (UINT)(b)) >= 0)

/* The Transmission Control Block (spec 13.2). 188 bytes on the S/370 target;
 * the SIZE_ASSERT fires under cc370 only (host pointers are 8 B). Pool-allocated
 * from the TCPTCB pool at attach, mm_freed by tcp_destroy. Field order is RFC
 * 793 / spec 13.2 VERBATIM so an auditor can read it against the RFC. */
typedef struct tcb {
    QELEM    q;                 /*   8  @0    tcb list linkage                  */
    SOCKCB  *sock;              /*   4  @8    owning socket (back-pointer)      */
    UCHAR    state;             /*   1  @12   TCP_CLOSED .. TCP_TIME_WAIT       */
    UCHAR    flags;             /*   1  @13   NODELAY, KEEPALIVE, ... (M5)      */
    USHORT   mss;               /*   2  @14   negotiated send MSS               */
    /* send sequence space (RFC 793 §3.3 names) */
    UINT     snd_una;           /*   4  @16   oldest unacknowledged             */
    UINT     snd_nxt;           /*   4  @20   next sequence number to send      */
    UINT     snd_wnd;           /*   4  @24   send window                       */
    UINT     snd_wl1;           /*   4  @28   seq of last window update         */
    UINT     snd_wl2;           /*   4  @32   ack of last window update         */
    UINT     iss;               /*   4  @36   initial send sequence             */
    /* receive sequence space */
    UINT     rcv_nxt;           /*   4  @40   next sequence number expected     */
    UINT     rcv_wnd;           /*   4  @44   receive window                    */
    UINT     irs;               /*   4  @48   initial receive sequence          */
    /* queues */
    QUEUE    sndq;              /*  12  @52   unsent + unacked PBUF chain       */
    UINT     sndq_bytes;        /*   4  @64   bytes held in sndq                */
    QUEUE    oooq;              /*  12  @68   out-of-order segments (M5; <=4)   */
    /* timers -- EMBEDDED, never allocated (spec 6.2) */
    TMR      t_rexmit;          /*  24  @80   retransmission                    */
    TMR      t_persist;         /*  24  @104  window probe                      */
    TMR      t_keep;            /*  24  @128  keepalive                         */
    TMR      t_2msl;            /*  24  @152  TIME_WAIT / FIN_WAIT_2            */
    /* RTO state (M4-4) */
    USHORT   rto;               /*   2  @176  retransmission timeout (ticks)    */
    USHORT   srtt;              /*   2  @178  smoothed round-trip time          */
    USHORT   rttvar;            /*   2  @180  round-trip variance               */
    UCHAR    backoff;           /*   1  @182  exponential-backoff shift         */
    UCHAR    dupacks;           /*   1  @183  duplicate-ACK run length          */
    /* connection topology (M4-2, NSF-internal -- not RFC 793): a passively-opened
     * child points back at its listener until it is ACCEPTed; an established but
     * un-ACCEPTed child is also queued on the listener's acceptq through
     * `acceptlink` (a SECOND linkage -- `q` above always links the demux list). */
    struct tcb *listener;       /*   4  @184  child -> listening TCB, else NULL  */
    QELEM    acceptlink;        /*   8  @188  linkage on the listener's acceptq  */
    USHORT   rsvd;              /*   2  @196  pad to a fullword boundary        */
} TCB;                          /* 200 bytes (padded to a fullword) */
NSF_SIZE_ASSERT(TCB, 200);

/* asm() external-symbol aliases (CLAUDE.md §3): cc370 folds an external to 8
 * chars (upcased, '_'->'@'), so every export pins a unique 8-char name, scheme
 * NSFTC*, clear of NSFTM* (timer) and NSFTR* (trace):
 *   nsftcp_reserve NSFTCRSV   nsftcp_init NSFTCINI   nsftcp_input NSFTCIN
 *   nsftcp_protops NSFTCOPS   nsftcp_debug_inuse NSFTCDBI
 * The PROTOPS callbacks (tcp_attach/tcp_detach) and the internal machinery
 * (tcp_destroy, tcp_output_rst, the per-state handlers) are static -- reached
 * through the vtable pointer or by direct call within nsftcp.c, never by
 * external name -- so they need no alias.
 */

/* Create the TCPTCB pool (`count` TCBs, 0 => NSFTCP_MAX_TCB, capped at the list
 * depth). Init-window only (mm_pool_create). Returns 0 on success, non-zero if
 * the pool could not be created (the executive refuses to start). */
int      nsftcp_reserve(UINT count) asm("NSFTCRSV");

/* Register the NSFTCP counters (spec 13.5), reset the TCB list, and register the
 * inbound demux handler with NSFIP (nsfip_register_proto(6, nsftcp_input)).
 * Idempotent. MUST run after nsfip_init/nsfip_config (which do not touch the IP
 * handler table) and before any segment flows. The SOCKET-side registration
 * (nsfreq_register_proto(6, nsftcp_protops())) is the caller's, so NSFTCP takes
 * no upward dependency on NSFREQ. */
void     nsftcp_init(void) asm("NSFTCINI");

/* Inbound demux target (registered with NSFIP). Verifies length/offset and the
 * mandatory checksum, demuxes on the 4-tuple / listener, and drives the RFC 793
 * SEGMENT ARRIVES skeleton -- in M4-1 a segment to a closed port draws a RST per
 * RFC 793 §3.4 (never a RST in response to a RST). TAKES OWNERSHIP of b. `ip`
 * aliases the IP header at b->data. */
void     nsftcp_input(NETDEV *dev, PBUF *b, const IPHDR *ip) asm("NSFTCIN");

/* The TCP PROTOPS vtable, for nsfreq_register_proto(NSFIP_PROTO_TCP, ...). */
PROTOPS *nsftcp_protops(void) asm("NSFTCOPS");

#if NSF_DEBUG
/* Leak-gate diagnostic (host tests): TCPTCB-pool objects currently in use,
 * which returns to baseline after every TCB is destroyed. Mirrors
 * soc_debug_inuse / nsfudp_debug_inuse; outside the production interface. */
UINT     nsftcp_debug_inuse(void) asm("NSFTCDBI");
#endif

#endif /* NSFTCP_H */
