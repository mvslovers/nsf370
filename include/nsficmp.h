#ifndef NSFICMP_H
#define NSFICMP_H
/*
 * nsficmp.h -- ICMP for IPv4 (spec ch. 11, the ICMP half of NSFIP/NSFICM).
 *
 * M2-3 scope: the ECHO responder. An inbound echo request (type 8) is answered
 * IN PLACE -- the SAME PBUF becomes the echo reply (type 0): the ICMP checksum is
 * recomputed, the IP header is stripped, and nsfip_output rebuilds a fresh IP
 * header with the source/destination swapped. One PBUF flows through the whole
 * request->reply path: no allocation, no copy, single owner, no leak (spec 3.4).
 * Non-echo ICMP is counted and dropped.
 *
 * M2-4 scope: ICMP error GENERATION (nsficmp_send_error, spec 11.2). Unlike the
 * echo responder, an error is built in a FRESHLY ALLOCATED PBUF -- `orig` is
 * const and stays owned by its caller (single-owner, spec 3.4), and the error
 * quotes bytes out of it rather than repurposing it. Only ONE trigger is live in
 * v1: `nsfip_input`'s existing `noproto` path (an inbound TCP/UDP segment or any
 * other non-ICMP protocol NSF does not yet implement) also calls
 * `nsficmp_send_error(orig, 3, 2)` (destination unreachable / protocol
 * unreachable). Port unreachable (type 3 code 3) has no trigger until UDP/TCP
 * closed-port handling exists (M4); time exceeded (type 11) has no trigger
 * because v1 is a host, not a router, so `ttlexp` never fires (spec 11.1) -- see
 * nsficmp_send_error's own comment for why both are still fully implemented.
 *
 * OWNERSHIP. nsficmp_input TAKES OWNERSHIP of b (handed down from nsfip_input):
 * it either frees it or hands it to nsfip_output (which then owns it). The `ip`
 * pointer aliases b->data (the IP header at the front of the same PBUF), passed
 * so ICMP need not re-parse it. nsficmp_send_error does NOT take ownership of
 * `orig` -- it only reads it; the caller (which already owns it) still frees it.
 */

#include "nsf.h"
#include "nsfbuf.h"             /* PBUF                     */
#include "nsfdev.h"             /* NETDEV                   */
#include "nsfip.h"              /* IPHDR                    */

/* ICMP message types we handle. */
#define NSFICMP_ECHO_REPLY       0
#define NSFICMP_DEST_UNREACH     3
#define NSFICMP_ECHO_REQUEST     8
#define NSFICMP_TIME_EXCEEDED   11

/* Destination Unreachable codes (RFC 792). */
#define NSFICMP_UNREACH_PROTO    2   /* protocol unreachable -- LIVE in v1     */
#define NSFICMP_UNREACH_PORT     3   /* port unreachable -- infrastructure-only,
                                       * no trigger until M4 closed ports       */

/* asm() external-symbol aliases (CLAUDE.md §3). Scheme NSFICM*, clear of NSFIP*:
 *   nsficmp_init NSFICMNI   nsficmp_input NSFICMIN   nsficmp_send_error NSFICMSE
 */

/* Register the ICMP §11.7 counters (once per process; guarded). Call at startup,
 * after nsfip_init, before packets flow -- like the other components' inits. */
void nsficmp_init(void) asm("NSFICMNI");

/* Handle one inbound ICMP message. Verifies the ICMP checksum, and on an echo
 * request builds the echo reply in the same PBUF and sends it via nsfip_output.
 * TAKES OWNERSHIP of b. `ip` aliases the IP header at b->data. */
void nsficmp_input(NETDEV *dev, PBUF *b, const IPHDR *ip) asm("NSFICMIN");

/* Generate an ICMP error (dest unreachable / time exceeded, spec 11.2) in
 * response to `orig`, a datagram this stack could not deliver. Builds a FRESH
 * PBUF: a new IP header (src = our own address that received `orig`, dst =
 * `orig`'s source) around an ICMP `type`/`code` header quoting `orig`'s own IP
 * header plus the first 8 bytes of its payload (RFC 792). Silently drops the
 * request (no error sent, nothing counted -- spec 11.7 has no counter for a
 * suppressed send) when `orig` fails any RFC 1122 §3.2.2 suppression rule: it is
 * itself an ICMP error message, its destination is a broadcast/multicast
 * address, it is a non-initial fragment, or its source does not identify a
 * single host (zero, broadcast, or multicast). Does NOT take ownership of
 * `orig` -- READS it only; the caller still owns and frees it. Counts
 * `icmp_errsent` only when the error is actually transmitted (mirrors
 * `icmp_outecho`'s success-only counting in nsficmp_input). */
void nsficmp_send_error(const PBUF *orig, UCHAR type, UCHAR code) asm("NSFICMSE");

#endif /* NSFICMP_H */
