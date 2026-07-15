#ifndef NSFUDP_H
#define NSFUDP_H
/*
 * nsfudp.h -- UDP: datagram send/receive, port demux, checksum (spec ch. 12).
 *
 * UDP is the end-to-end proof of the socket/request path (M3) precisely because
 * it is trivial: no timers, no state machine. It plugs into two seams the lower
 * layers already expose:
 *   - the SOCKET protocol vtable (PROTOPS, spec 10.2): nsfudp_protops() is
 *     registered with NSFREQ (nsfreq_register_proto(17, ...)) so an RQ_SOCKET of
 *     IP protocol 17 gets UDP's bind/send/recv/detach callbacks. The callbacks
 *     run on the executive task, run-to-completion, and either COMPLETE their
 *     NSFRQE (soc_complete) or PARK it (soc_park) -- never block (spec 10.3).
 *   - the IP transport-demux seam (spec 11.1): nsfudp_init registers
 *     nsfudp_input with NSFIP (nsfip_register_proto(17, ...)) so inbound
 *     protocol-17 datagrams demux to UDP. A datagram matching no bound pcb draws
 *     an ICMP port-unreachable (spec 11.2) -- the first live trigger of M2-4's
 *     dormant port-unreachable path.
 *
 * CONTROL BLOCK (spec 12.2). One UDPPCB per BOUND socket, created at bind time
 * and destroyed in soc_destroy via the `detach` callback (spec 12.3) -- NSFUDP
 * never adds a second teardown path; soc_destroy's checklist (10.5) already
 * calls detach. The pcbs form a bounded list (<=32); demux is a linear scan
 * matching (lport, laddr|INADDR_ANY), a specific laddr beating ANY.
 *
 * CHECKSUM (spec 12.1 / ADR-0028). Computed on output, verified on input, with
 * the IPv4 pseudo-header threaded in as a SEED to the shared in_cksum (no second
 * checksum routine, no PBUF overlay -- see nsfcksum.h). RFC 768 zero-checksum,
 * BOTH directions: a computed 0x0000 is transmitted as 0xFFFF (all-ones); a
 * RECEIVED 0x0000 means "sender computed none" and is accepted without check.
 *
 * OWNERSHIP (spec 3.4, single owner). nsfudp_input TAKES OWNERSHIP of b (frees
 * it, enqueues it on the socket rxq, or hands its payload to a parked RECV then
 * frees it). SENDTO allocates a fresh PBUF and hands it to nsfip_output (which
 * then owns it) or frees it on an early error -- never both, never neither.
 */

#include "nsf.h"
#include "nsfsoc.h"             /* SOCKCB, PROTOPS (the vtable UDP fills)     */
#include "nsfip.h"              /* IPHDR, NSFIP_PROTO_UDP, nsfip_output       */
#include "nsfque.h"             /* QELEM (pcb list linkage)                   */

/* Bound-pcb list depth (spec 12.2: "linear scan ... <=32 default") and the
 * pool object size (spec 12.2: "pool objsize 64 for growth" -- the struct is
 * 20 B on target; the extra room is reserved for later fields, exactly as
 * NSFRQE_OBJSIZE reserves growth for the request block). */
#define NSFUDP_MAX_PCB     32
#define NSFUDP_PCB_OBJSIZE 64

/* Ephemeral local-port range for a port-0 bind / an unbound SENDTO (IANA
 * dynamic range 49152-65535, RFC 6335). */
#define NSFUDP_EPHEM_LO    49152u
#define NSFUDP_EPHEM_HI    65535u

/* UDP fixed header length (source port, dest port, length, checksum). */
#define NSFUDP_HDRLEN      8

/* The per-bound-socket control block (spec 12.2). 20 bytes on the S/370 target
 * (pointers are 4 B); the SIZE_ASSERT fires under cc370 only (host pointers are
 * 8 B). Pool-allocated from the UDPPCB pool at bind, mm_freed by udp_detach. */
typedef struct udppcb {
    QELEM   q;                  /*  8  @0   bound-pcb list linkage             */
    SOCKCB *sock;               /*  4  @8   owning socket                      */
    UINT    laddr;              /*  4  @12  local address, 0 == INADDR_ANY     */
    USHORT  lport;              /*  2  @16  local port (host order)            */
    USHORT  flags;              /*  2  @18  reserved                           */
} UDPPCB;                       /* 20 bytes */
NSF_SIZE_ASSERT(UDPPCB, 20);

/* The RECVFROM address record (spec 12.3). udp_input trims the datagram to its
 * payload and PREPENDS this 8-byte record so a queued rxq PBUF is
 * [UDPADDR | payload]; RQ_RECVFROM reads the peer address/port from it, copies
 * the payload out, and frees the PBUF. An internal contract between udp_input
 * and udp_recv -- never seen by the application (EZASOKET builds the sockaddr
 * from the request's returned p1/p2 at M3-4). Pointer-free and fixed size, so it
 * embeds identically on host and target. */
typedef struct udpaddr {
    UINT   addr;                /*  4  peer (source) IP, octet-1 in the MSB   */
    USHORT port;                /*  2  peer (source) port, host order         */
    USHORT len;                 /*  2  payload bytes following this record    */
} UDPADDR;                      /* 8 bytes */
NSF_SIZE_ASSERT(UDPADDR, 8);

/* asm() external-symbol aliases (CLAUDE.md §3): cc370 folds an external to 8
 * chars (upcased, '_'->'@'), so every export pins a unique 8-char name, scheme
 * NSFU*, clear of NSFUDP-as-CSECT-name collisions:
 *   nsfudp_reserve NSFURSV   nsfudp_init NSFUINIT   nsfudp_input NSFUIN
 *   nsfudp_protops NSFUOPS   nsfudp_debug_inuse NSFUDBI
 * The PROTOPS callbacks (udp_attach/bind/send/recv/detach) are static -- reached
 * through the vtable pointer, never by external name -- so they need no alias.
 */

/* Create the UDPPCB pool (`count` pcbs, 0 => NSFUDP_MAX_PCB, capped at the list
 * depth). Init-window only (mm_pool_create). Returns 0 on success, non-zero if
 * the pool could not be created (the executive refuses to start). */
int      nsfudp_reserve(UINT count) asm("NSFURSV");

/* Register the NSFUDP counters, reset the bound-pcb list, and register the
 * inbound demux handler with NSFIP (nsfip_register_proto(17, nsfudp_input)).
 * Idempotent. MUST run after nsfip_init/nsfip_config (which do not touch the IP
 * handler table) and before any datagram flows. The SOCKET-side registration
 * (nsfreq_register_proto(17, nsfudp_protops())) is the caller's, so NSFUDP takes
 * no upward dependency on NSFREQ. */
void     nsfudp_init(void) asm("NSFUINIT");

/* Inbound demux target (registered with NSFIP). Verifies length + checksum
 * (RFC 768 zero-checksum accepted), demuxes to a bound pcb, and delivers the
 * payload to the socket rxq (or a parked RECV). No pcb -> ICMP port unreachable
 * + count. TAKES OWNERSHIP of b. `ip` aliases the IP header at b->data. */
void     nsfudp_input(NETDEV *dev, PBUF *b, const IPHDR *ip) asm("NSFUIN");

/* The UDP PROTOPS vtable, for nsfreq_register_proto(NSFIP_PROTO_UDP, ...). */
PROTOPS *nsfudp_protops(void) asm("NSFUOPS");

#if NSF_DEBUG
/* Leak-gate diagnostic (host tests): UDPPCB-pool objects currently in use,
 * which returns to baseline after every bound socket is destroyed. Mirrors
 * soc_debug_inuse / buf_debug_pool; outside the production interface. */
UINT     nsfudp_debug_inuse(void) asm("NSFUDBI");
#endif

#endif /* NSFUDP_H */
