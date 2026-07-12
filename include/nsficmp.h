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
 * Non-echo ICMP is counted and dropped; ICMP error GENERATION (dest unreachable,
 * time exceeded) is M2-4 (nsficmp_send_error, spec 11.2).
 *
 * OWNERSHIP. nsficmp_input TAKES OWNERSHIP of b (handed down from nsfip_input):
 * it either frees it or hands it to nsfip_output (which then owns it). The `ip`
 * pointer aliases b->data (the IP header at the front of the same PBUF), passed
 * so ICMP need not re-parse it.
 */

#include "nsf.h"
#include "nsfbuf.h"             /* PBUF                     */
#include "nsfdev.h"             /* NETDEV                   */
#include "nsfip.h"              /* IPHDR                    */

/* ICMP message types we handle in M2-3. */
#define NSFICMP_ECHO_REPLY    0
#define NSFICMP_ECHO_REQUEST  8

/* asm() external-symbol aliases (CLAUDE.md §3). Scheme NSFICM*, clear of NSFIP*:
 *   nsficmp_init NSFICMNI   nsficmp_input NSFICMIN
 */

/* Register the ICMP §11.7 counters (once per process; guarded). Call at startup,
 * after nsfip_init, before packets flow -- like the other components' inits. */
void nsficmp_init(void) asm("NSFICMNI");

/* Handle one inbound ICMP message. Verifies the ICMP checksum, and on an echo
 * request builds the echo reply in the same PBUF and sends it via nsfip_output.
 * TAKES OWNERSHIP of b. `ip` aliases the IP header at b->data. */
void nsficmp_input(NETDEV *dev, PBUF *b, const IPHDR *ip) asm("NSFICMIN");

#endif /* NSFICMP_H */
