# ADR-0028 — UDP checksum via a pseudo-header seed; IP demux via registration

**Status:** Accepted (2026-07-15). Introduced with M3-3 (NSFUDP). **Relates to:**
§11.5 (checksum), §12 (UDP), §11.1/11.2 (IP input demux), ADR-0024 (byte-wise
big-endian discipline), ADR-0008 (single-owner buffers).

Two small but load-bearing choices M3-3 forced. Both were "pick one and justify
it in the source"; recording them here so a later reader does not "simplify" them
back into a subtle bug.

## 1. UDP/TCP checksum pseudo-header: SEED the shared checksum, do not overlay

### Context

The UDP checksum (RFC 768) covers a 12-byte IPv4 **pseudo-header** (src addr, dst
addr, zero, protocol, UDP length) that is NOT present in the PBUF, plus the UDP
header and payload. `in_cksum(chain, off, len)` sums a byte range of a PBUF
chain; the pseudo-header is not in the chain. Two ways to include it:

- **(a) Overlay** — build the pseudo-header into the PBUF headroom immediately
  below the UDP header, checksum contiguously, then write the real IP header over
  it. Headroom is 64 B, IP header 20 B, pseudo-header 12 B — the arithmetic fits
  on OUTPUT.
- **(b) Seed** — sum the pseudo-header separately into a partial one's-complement
  sum and carry it into the chain sum.

### Decision

**Option (b): a seeded partial-sum API.** `in_cksum` is refactored into
`in_cksum_partial(chain, off, len, seed)` (the M2 accumulation loop, seeded, that
returns the UNFOLDED 32-bit partial) + `in_cksum_fold(sum)` (end-around carry +
complement), with `in_cksum(chain, off, len) ≡ in_cksum_fold(in_cksum_partial(
chain, off, len, 0))`. UDP sums the 12-byte pseudo-header (built on a stack
buffer wrapped in a throwaway one-segment PBUF — reusing the ONE routine, so no
second checksum exists) into a seed, then folds `partial(datagram, seed)`.

### Why (b) over (a)

- **The overlay is impossible on INPUT.** A received PBUF has no headroom — it is
  filled from `start` (`buf_reset_rx`), so there is nowhere below the UDP header
  to build the pseudo-header. Overlay would need a *second*, different code path
  for verification. The seed is symmetric: output and input both compute
  `fold(partial(datagram, pseudo_seed))`.
- **Correctness is parity-preserving and provable.** The pseudo-header is a whole
  number of 16-bit words (12 B = 6, even), so after summing it the transport
  region still opens on a HIGH byte — the global `taken` word parity (relative to
  `off`, the M2-1 boundary-carry invariant) is identical whether the pseudo-header
  is summed standalone or concatenated. The seed is a pure carry-in.
- **The M2 vectors are untouched.** `in_cksum ≡ fold(partial(…, 0))`, so every
  RFC-1071 literal vector in `tstcksum.c` passes byte-identical; a new literal
  UDP vector (src 10.1.1.2 → dst 10.1.1.1, sport 0x1234, dport 0x0035, a 6-byte
  payload → checksum **0x9371**, independently hand-computed) pins the seeded path.

### RFC 768 zero-checksum, both directions (the classic trap)

- **Output:** if the computed checksum is 0x0000, transmit **0xFFFF** (the
  all-ones one's-complement equivalent). A transmitted 0x0000 means "no checksum"
  and would disable verification at the peer.
- **Input:** a received checksum of 0x0000 means the sender computed none —
  **accept without verification**. A non-zero checksum that fails → drop + count.

## 2. IP input demux: registration table, not an explicit `case`

### Context

`nsfip_input` demuxes by IP protocol number. ICMP is a hardcoded `case` calling
`nsficmp_input` directly. UDP (M3-3) and TCP (M4) need to join.

### Decision

**A registration seam:** `nsfip_register_proto(proto, fn)` fills a small fixed
table (idempotent-replace); `nsfip_input` looks up the handler before the
noproto/protocol-unreachable fallback. `nsfudp_init` registers proto 17. ICMP
stays a direct case. `nsfip_init` does NOT reset the table (handler wiring is
static, not per-config), so registration is order-independent w.r.t.
`nsfip_config`.

### Why registration, not a direct `case nsfudp_input()`

This is **forced, not stylistic.** `src/nsfip.c` is in the production `NSF` load
module (`[[module]]` sources); `src/nsfudp.c` is NOT — UDP is unreachable until
EZASOKET (M3-4), exactly as M3-1/M3-2 kept `nsfsoc.c`/`nsfreq.c` out of the
module. An explicit `case NSFIP_PROTO_UDP: nsfudp_input(…)` would be an
unresolved external at the NSF link (`ld370` reports it; the module would
`S0C4`). Registration keeps `nsfip` free of any direct symbol dependency on
`nsfudp`/`nsftcp` — the same decoupling `evt_set_devices`/`evt_set_request` use.
TCP joins by registering, with zero `nsfip.c` change. ICMP is exempt because it
is IP-intrinsic serviceability (echo/errors) and `nsficmp.c` is always linked
into the module (`nsfip.c` already calls `nsficmp_send_error` directly).

## Consequences

- Any future HLASM checksum drop-in (Project Brief §2.2) must expose the same
  `partial`/`fold` split, or UDP/TCP lose the seed path.
- A protocol with no registered handler still draws protocol-unreachable (code 2);
  a bound-but-unmatched UDP port draws port-unreachable (code 3) from inside
  `nsfudp_input`. The two unreachable codes stay distinct and correct.
- The demux table is fixed-size (4 entries: ICMP-direct + UDP + TCP + slack); a
  larger protocol set would need a bigger table, logged, not silently truncated.
