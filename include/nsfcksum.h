#ifndef NSFCKSUM_H
#define NSFCKSUM_H
/*
 * nsfcksum.h -- the Internet checksum (spec ch. 11.5, RFC 1071).
 *
 * One shared, portable, allocation-free routine used by IP (header checksum) and
 * ICMP/UDP/TCP (message checksum). Deliberately its own tiny module so it is unit
 * tested against literal RFC/packet vectors BEFORE any packet code exists -- the
 * checksum is the one primitive every protocol layer trusts blindly, so it is
 * pinned first (spec 11.5, "Host-build unit tests pin it against known vectors
 * before any packet flies").
 *
 * THE CHAIN CONTRACT. in_cksum sums a logical byte range of a PBUF CHAIN as one
 * contiguous stream: it starts `off` bytes into the chain and sums `len` bytes,
 * following ->chain across segment boundaries. The 16-bit word parity is RELATIVE
 * TO `off` (the byte at logical offset `off` is the HIGH byte of the first word),
 * NOT relative to a segment start -- so an ODD trailing byte of one segment pairs
 * with the FIRST byte of the next. A naive per-segment sum is green on even
 * segment lengths and WRONG at an odd boundary; the tests pin exactly that case.
 *
 * ENDIANNESS. The sum is defined on big-endian 16-bit words (= native S/370
 * order), but this routine reads the stream BYTE BY BYTE and forms each word as
 * (hi << 8) | lo, so it is byte-order-correct on the little-endian test host too
 * (never a USHORT load, which would sum host-endian words there while round-
 * tripping green). The result halfword is likewise returned as a host-native
 * USHORT value (the caller stores it into a header byte-wise).
 *
 * An HLASM drop-in with the same signature may replace this later if profiling
 * justifies it (Project Brief 2.2); the vector tests lock the contract for both.
 *
 * PSEUDO-HEADER SEED (M3-3, ADR-0028). UDP and TCP checksum a 12-byte IPv4
 * pseudo-header (src addr, dst addr, zero, protocol, transport length) that is
 * NOT in the PBUF, plus the transport header and payload. Rather than overlay
 * the pseudo-header into the PBUF -- impossible on the INPUT path, where a
 * received PBUF has no headroom ahead of the data -- the sum is SEEDED: the
 * pseudo-header's partial one's-complement sum is computed once (over a small
 * stack buffer) and carried into the chain sum. This works BECAUSE the
 * pseudo-header is a whole number of 16-bit words (12 bytes = 6 words, even), so
 * the transport region still begins on a HIGH byte in both the standalone and
 * the seeded framings -- the `taken` word parity (relative to `off`) is
 * identical either way. in_cksum stays EXACTLY as before (== fold(partial(0))),
 * so the RFC-1071 vectors are unchanged, and no second checksum routine exists.
 */

#include "nsf.h"
#include "nsfbuf.h"             /* PBUF (chain walked by in_cksum) */

/* asm() external-symbol aliases (CLAUDE.md §3). Distinct from the codec's NSFCK*
 * decode/encode names (NSFCKDI/NSFCKDN/NSFCKEN/NSFCKEH):
 *   in_cksum NSFCKSUM   in_cksum_partial NSFCKPAR   in_cksum_fold NSFCKFLD
 */

/* Accumulate the one's-complement sum of `len` bytes of `chain` starting at
 * logical offset `off` INTO `seed`, WITHOUT folding or complementing -- so a
 * caller can chain several regions (e.g. a UDP/TCP pseudo-header summed
 * separately) into one checksum. Word parity is relative to `off` exactly as
 * in_cksum (the byte at `off` is the high byte of the first word); `seed` must
 * be the running 32-bit partial sum of a region that ended on an EVEN byte
 * boundary (a whole number of 16-bit words), or the parity of `chain` shifts.
 * `chain` is const. A range past the end of the chain sums only the bytes that
 * exist (len clamped by available data). Returns the updated 32-bit partial. */
UINT   in_cksum_partial(const PBUF *chain, USHORT off, USHORT len,
                        UINT seed) asm("NSFCKPAR");

/* Fold the end-around carries out of a 32-bit partial sum and complement it into
 * the 16-bit checksum ready to store in a header field. fold(partial(...)) over
 * a region that already carries its checksum yields 0 when it verifies. */
USHORT in_cksum_fold(UINT sum) asm("NSFCKFLD");

/* One's-complement Internet checksum (RFC 1071) over `len` bytes of `chain`
 * starting at logical offset `off`. Returns the 16-bit checksum ready to store
 * in a header field (the complement of the folded one's-complement sum). To
 * VERIFY a region that already carries its checksum, compute over the whole
 * region and check the result is 0. `chain` is const -- the checksum never
 * mutates the buffer. A range that runs past the end of the chain simply sums
 * the bytes that exist (len is clamped by the available data). Defined as
 * in_cksum_fold(in_cksum_partial(chain, off, len, 0)), so it is byte-for-byte
 * the M2 routine. */
USHORT in_cksum(const PBUF *chain, USHORT off, USHORT len) asm("NSFCKSUM");

#endif /* NSFCKSUM_H */
