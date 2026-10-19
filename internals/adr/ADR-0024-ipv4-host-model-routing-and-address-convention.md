# ADR-0024 — IPv4 host model: routing, not-for-us handling, TTL, and the address convention

**Status:** Accepted (M2-2/M2-3)
**Context:** spec §11 (IPv4/ICMP), §11.4 (routing), §11.7 (statistics),
CLAUDE.md §3 (single-owner PBUFs). Supersedes nothing; refines §11.

## Context

M2 turns bytes on the wire (M1) into an answered `ping`. Building IP input,
output, routing and the ICMP echo responder forced four decisions the frozen
Project Brief and the pre-M2 spec left implicit. Each is small, but each is
load-bearing for correctness on a 24-bit big-endian target that is unit-tested
on a little-endian host.

## Decision

### 1. NSF is a host, not a router — not-for-us is `inaddrerr`, dropped

A packet whose destination is not one of our HOME addresses is **dropped and
counted**, never forwarded. §11.7's published counter set had no name for this
because it is a router concept; we add **`inaddrerr`** (RFC 1213
`ipInAddrErrors`) and declare §11.7 a **minimum** set aligned with the IP MIB,
extensible per component. The local-address test is a small explicit list
(`nsfip_is_local`) built from HOME, not a routing-table reverse lookup — a HOME
address and a route to reach it are different questions.

### 2. TTL is parsed but is **not** a delivery gate

RFC 1122 §3.2.1.7: a destination host MUST NOT discard a datagram addressed to
it because it arrived with a low TTL — TTL expiry is a **forwarding** decision,
and NSF does not forward. So `nsfip_input` reads TTL but never drops on it, and
**`ttlexp` stays 0 in v1**, reserved for a future forward path (M6+). Honouring
the spec's terse "TTL" as "gate delivery on it" would have been an RFC
violation; it means "parse it."

### 3. Routing v1: HOME (classful on-link) + GATEWAY, longest match, peer = next-hop 0

The fixed 16-entry table (§11.4) is built by `nsfip_config` **after** the
interfaces register (so each route resolves its device by LINK name):
- each `HOME ip link` → `ip` in the local list **and** an on-link route for
  `ip`'s classful network out that link's device, next hop 0 (direct);
- each `GATEWAY` → a network route (mask defaulting to classful), or the
  default route for `DEFAULTNET`, next hop = firsthop.

Lookup is longest-prefix (largest mask) linear scan returning
`(device, next-hop)`; next hop 0 means on-link. On a **point-to-point** CTCI
link the driver ignores the next hop and writes to the peer unconditionally
(§11.6, ARP is M6), so a `GATEWAY DEFAULTNET <peer> <link>` line is what makes
an echo reply flow. A route whose device is absent is skipped, never fatal.
`nsfip_route_add`/`nsfip_local_add` are the primitives and the test seam.

### 4. Addresses are `UINT` (octet-1 in the MSB); the wire is touched byte-wise

Every IPv4 address inside NSF is a `UINT` with the first octet in the
most-significant byte (10.1.1.2 == 0x0A010102) — the form NSFCFG parses and
`NETDEV.ipaddr` holds. **All** routing / is-local logic is `UINT` arithmetic,
identical on host and target. Byte order matters **only** at the packet
boundary: `nsfip.c`/`nsficmp.c` read and write every multi-byte header field
(addresses, lengths, checksums, id, fragment word) **byte by byte** (`p[0] =
v>>24 …`), never a struct overlay or a `UINT`/`USHORT` cast. A cast round-trips
green on both host and target while emitting host-endian bytes on the
little-endian test box — the exact defect the CTCI codec avoided the same way
(§9.3, ADR-0020). The host tests therefore assert **literal wire bytes**, not
only round-trips.

## Consequences

- The echo responder reuses **one PBUF** for request→reply (single owner, no
  allocation): verify the ICMP checksum, flip type 8→0, recompute, strip the IP
  header (which opens exactly the headroom `nsfip_output` needs to prepend a
  fresh 20-byte header), and `nsfip_output` with source/destination swapped.
  `icmp_outecho` is counted only when `nsfip_output` returns success, so
  ownership and the counter never double up.
- `in_cksum` sums a **PBUF chain** with parity relative to `off`, so a word
  straddling an odd segment boundary is correct — pinned by TSTCKSUM before any
  packet code (spec 11.5).
- §11.7 is now a documented minimum, not a closed set; later components add
  their own counters under their component name.

## Alternatives rejected

- **Drop not-for-us silently / fold into `hdrerr`:** loses the MIB-standard
  signal a `DISPLAY,STATS` needs to explain "pings vanish" (wrong HOME).
- **Gate delivery on TTL:** violates RFC 1122; would drop legitimate pings from
  a distant host.
- **Struct-overlay header access:** green-and-wrong on the little-endian host,
  the single most likely "works in CI, broken on MVS" failure for this code.
