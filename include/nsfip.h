#ifndef NSFIP_H
#define NSFIP_H
/*
 * nsfip.h -- IPv4 input, output and routing (spec ch. 11).
 *
 * NSFIP is the network layer: it validates and demultiplexes inbound IP packets
 * to the transports (ICMP now; UDP/TCP at M3/M4), builds the IP header for
 * outbound packets, and owns the routing table. It is a HOST stack, not a router
 * -- a packet not addressed to one of our HOME addresses is dropped, never
 * forwarded (spec 11.1). v1 does not fragment or reassemble (spec 11.3): inbound
 * fragments are dropped and counted.
 *
 * ADDRESS CONVENTION (critical, endian-safe). An IPv4 address is carried EVERY-
 * WHERE inside NSF as a UINT with the first octet in the most-significant byte
 * (10.1.1.2 == 0x0A010102) -- the same form NSFCFG parses and NETDEV.ipaddr
 * holds. All address logic (routing match, is-local) is UINT arithmetic, which
 * is identical on host and target. Byte order matters ONLY at the packet
 * boundary, where nsfip.c reads/writes the four address bytes BYTE BY BYTE
 * (p[0]=addr>>24 ... p[3]=addr) -- never a UINT store/load -- so the wire bytes
 * are network order on both the big-endian target and the little-endian test
 * host (the same discipline as the CTCI codec, nsfctcif.c).
 *
 * OWNERSHIP (spec 3.4, single-owner PBUFs). nsfip_input and nsfip_output each
 * TAKE OWNERSHIP of the PBUF: every path frees it or hands it to exactly one
 * next owner (a transport on input, dev_send on output). No PBUF is owned twice
 * or neither.
 */

#include "nsf.h"
#include "nsfbuf.h"             /* PBUF                                */
#include "nsfdev.h"             /* NETDEV (routing target, input dev)  */
#include "nsfcfg.h"             /* NSFCFG (nsfip_config from PROFILE)   */

/* IP protocol numbers we demux (spec 11.1). */
#define NSFIP_PROTO_ICMP    1
#define NSFIP_PROTO_TCP     6
#define NSFIP_PROTO_UDP    17

/* Fixed IPv4 header size (no options) and the default outbound TTL. */
#define NSFIP_HDR_MIN      20
#define NSFIP_TTL_DEFAULT  64

/* Routing / local-address table capacities (spec 11.4: "default 16 entries"). */
#define NSFIP_MAX_ROUTES   16
#define NSFIP_MAX_LOCAL     8

/* The IPv4 fixed header, addressed as raw bytes. A distinct type so the ICMP
 * interface signature (spec 11.2) reads clearly, but every multi-byte field is
 * read BYTE BY BYTE by the consumer (nsfip.c / nsficmp.c each carry their own
 * static big-endian readers) -- NEVER a struct-field load, which would be wrong
 * on the little-endian host. Options, when present (IHL > 5), follow these 20
 * bytes in the same PBUF. */
typedef struct iphdr {
    UCHAR b[NSFIP_HDR_MIN];
} IPHDR;
NSF_SIZE_ASSERT(IPHDR, 20);

/* Transport demux handler (spec 11.1). A registered handler receives an inbound
 * datagram of its IP protocol number, exactly as nsficmp_input does: it TAKES
 * OWNERSHIP of `b` (frees it or hands it on), and `ip` aliases the IP header at
 * b->data so it need not re-parse it. UDP (M3-3) and TCP (M4) register through
 * this seam. */
typedef void (*NSFIP_PROTO_FN)(NETDEV *dev, PBUF *b, const IPHDR *ip);

/* asm() external-symbol aliases (CLAUDE.md §3). cc370 folds an external to 8
 * chars after upcasing and '_'->'@', so e.g. nsfip_input and nsfip_init would
 * BOTH fold to NSFIP@IN and ld370 would silently keep one -- a wrong-function
 * dispatch on MVS a host build cannot see. Every export pins a unique 8-char
 * name (scheme NSFIP*), clear of the codec's NSFCK* and NSFDEV's NSFD*:
 *   nsfip_input NSFIPIN    nsfip_output NSFIPOUT   nsfip_init NSFIPINI
 *   nsfip_config NSFIPCFG  nsfip_route_add NSFIPRTA  nsfip_local_add NSFIPLCA
 *   nsfip_is_local NSFIPISL  nsfip_route NSFIPRT   nsfip_register_proto NSFIPRGP
 */

/* Reset the routing table and local-address list to empty, and (once per
 * process) register the §11.7 IP counters. Call before adding routes -- the STC
 * startup via nsfip_config, a test directly. Idempotent (counters register only
 * once; the table/local list are re-zeroed each call). */
void    nsfip_init(void) asm("NSFIPINI");

/* Build the routing table and local-address list from a validated PROFILE: each
 * HOME address becomes a local address plus an on-link (classful) network route
 * out its LINK's device; each GATEWAY becomes a network route or the default
 * route. Devices are resolved by LINK name (dev_find), so this MUST run AFTER
 * the devices are registered. Returns 0 (routes that cannot resolve a device are
 * skipped, not fatal -- an interface may be down). Resets the table first. */
int     nsfip_config(const NSFCFG *cfg) asm("NSFIPCFG");

/* Add a route: destinations matching (dst & mask) == (net & mask) reach `dev`,
 * with next hop `gw` (0 == on-link/direct == "the peer" on a point-to-point
 * link, §11.6). mask 0 with net 0 is the default route. Returns 0, or non-zero
 * if the table is full. Read-only after init (spec 11.4). */
int     nsfip_route_add(UINT net, UINT mask, NETDEV *dev, UINT gw) asm("NSFIPRTA");

/* Record `ip` as one of this stack's addresses (a packet whose destination is
 * one of these is "for us"). Returns 0, or non-zero if the list is full. */
int     nsfip_local_add(UINT ip) asm("NSFIPLCA");

/* 1 if `ip` is one of this stack's local (HOME) addresses, else 0. */
int     nsfip_is_local(UINT ip) asm("NSFIPISL");

/* Longest-prefix-match route lookup for destination `dst`. On a match sets
 * *nexthop to the resolved next-hop address (the configured gateway, or `dst`
 * itself for an on-link route) and returns the outbound device; returns NULL on
 * no route. On a point-to-point link the driver ignores *nexthop and writes to
 * the peer unconditionally (§11.6); *nexthop is meaningful once ARP/LCS lands
 * (M6). */
NETDEV *nsfip_route(UINT dst, UINT *nexthop) asm("NSFIPRT");

/* Register `fn` as the demux handler for inbound datagrams of IP protocol
 * `proto` (spec 11.1). Idempotent-replace (a second call for the same proto
 * swaps the handler), so it is order-independent w.r.t. nsfip_init/nsfip_config
 * (which do NOT touch the handler table -- handler wiring is static, not
 * per-config). This is the clean seam that keeps NSFIP free of any direct
 * symbol dependency on NSFUDP/NSFTCP: nsfip.c is in the production NSF load
 * module, nsfudp.c/nsftcp.c are not (transports are unreachable until EZASOKET),
 * so an explicit `case nsfudp_input()` would be an unresolved external at the
 * NSF link -- registration avoids that (the evt_set_* decoupling pattern). ICMP
 * stays a hardcoded case: it is IP-intrinsic serviceability and nsficmp.c is
 * always linked into the module. Returns 0, or non-zero if the small handler
 * table is full. NSF_(register) M3-3. */
int     nsfip_register_proto(UCHAR proto, NSFIP_PROTO_FN fn) asm("NSFIPRGP");

/* Inbound: validate the IP header (version 4, IHL, length, checksum), drop and
 * count fragments / not-for-us / malformed packets (spec 11.7), and demux by
 * protocol to the transport (ICMP direct; UDP/TCP via nsfip_register_proto; an
 * unregistered protocol -> noproto + protocol-unreachable, spec 11.2). TAKES
 * OWNERSHIP of b: it is freed here or handed to exactly one transport. Called
 * from the EV_PACKET_RECEIVED handler. */
void    nsfip_input(NETDEV *dev, PBUF *b) asm("NSFIPIN");

/* Outbound: prepend and fill the IP header into b's headroom (monotonic id,
 * computed checksum), resolve the route for `dst`, and hand the packet to
 * dev_send. `src`/`dst` are UINT addresses (octet-1 in the MSB). TAKES OWNERSHIP
 * of b: on success it passes to dev_send; on no route / build failure it is
 * freed and counted. Returns 0 if sent (queued), non-zero if dropped. */
int     nsfip_output(PBUF *b, UINT src, UINT dst,
                     UCHAR proto, UCHAR ttl) asm("NSFIPOUT");

#endif /* NSFIP_H */
