/*
 * tstcksum.c -- in_cksum host unit tests (spec 11.5, RFC 1071), M2-1.
 *
 * Pins the Internet checksum against LITERAL vectors BEFORE any packet code
 * exists (spec 11.5): the RFC 1071 worked example; a known-good real IPv4 header
 * (checksum zeroed -> assert the computed value, full header -> assert the sum
 * verifies to 0); an odd total length (last byte padded); an odd starting offset
 * (word parity relative to `off`); and -- the case a naive per-segment sum gets
 * wrong -- a PBUF chain split so an ODD byte ends segment 1 and the word it
 * belongs to completes in segment 2 (the boundary carry).
 *
 * The vectors are transcribed and the expected checksums hand-computed, so a
 * byte-order or parity regression is caught by an EXACT value, not a round-trip
 * (which would be green on both host and MVS while emitting reversed bytes).
 *
 * PBUFs are fabricated on the stack (data/len/chain only) -- in_cksum walks just
 * those fields, so the test needs no NSFMM/NSFBUF pools.
 */
#include "nsfcksum.h"
#include "nsfbuf.h"
#include <mbtcheck.h>
#include <string.h>

/* Build a one-segment PBUF over [data, data+len) chaining to `next`. */
static PBUF seg(UCHAR *data, USHORT len, PBUF *next)
{
    PBUF p;

    memset(&p, 0, sizeof(p));
    p.data  = data;
    p.len   = len;
    p.chain = next;
    return p;
}

/* -- the RFC 1071 section 3 worked example -------------------------------------
 * Bytes 00 01 f2 03 f4 f5 f6 f7 sum (16-bit words, end-around carry) to 0xddf2,
 * so the checksum is ~0xddf2 = 0x220d. */
static UCHAR rfc[8] = { 0x00u, 0x01u, 0xf2u, 0x03u, 0xf4u, 0xf5u, 0xf6u, 0xf7u };

/* -- a real IPv4 header (20 bytes). The checksum field (bytes 10..11) is 0xb1e6;
 * with it zeroed in_cksum must compute 0xb1e6, and over the whole header (field
 * present) the sum must verify to 0. */
static UCHAR iphdr_full[20] = {
    0x45u, 0x00u, 0x00u, 0x3cu,   /* ver/ihl, tos, total length 60           */
    0x1cu, 0x46u, 0x40u, 0x00u,   /* id, flags/frag (DF)                     */
    0x40u, 0x06u, 0xb1u, 0xe6u,   /* ttl 64, proto 6 (TCP), checksum b1e6    */
    0xacu, 0x10u, 0x0au, 0x63u,   /* src 172.16.10.99                        */
    0xacu, 0x10u, 0x0au, 0x0cu    /* dst 172.16.10.12                        */
};

static void test_rfc_example(void)
{
    PBUF p = seg(rfc, (USHORT)sizeof(rfc), NULL);

    CHECK_EQ((long)in_cksum(&p, 0u, (USHORT)sizeof(rfc)), 0x220dL,
             "RFC 1071 worked example -> 0x220d");
}

static void test_ip_header(void)
{
    UCHAR z[20];
    PBUF  full = seg(iphdr_full, 20u, NULL);
    PBUF  zeroed;

    /* Full header (checksum present) verifies to 0. */
    CHECK_EQ((long)in_cksum(&full, 0u, 20u), 0L,
             "real IP header with checksum verifies to 0");

    /* Same header with the checksum field zeroed -> the stored value 0xb1e6. */
    memcpy(z, iphdr_full, sizeof(z));
    z[10] = 0x00u;
    z[11] = 0x00u;
    zeroed = seg(z, 20u, NULL);
    CHECK_EQ((long)in_cksum(&zeroed, 0u, 20u), 0xb1e6L,
             "IP header, checksum zeroed -> computes 0xb1e6");

    /* And storing that value back makes it verify to 0 (the build+verify cycle
     * the IP output path relies on). */
    z[10] = 0xb1u;
    z[11] = 0xe6u;
    CHECK_EQ((long)in_cksum(&zeroed, 0u, 20u), 0L,
             "recomputed checksum stored back verifies to 0");
}

static void test_odd_length(void)
{
    /* Bytes 00 01 f2: words 0x0001 + 0xf200 (last byte high, low half padded) =
     * 0xf201; checksum ~0xf201 = 0x0dfe. */
    PBUF p = seg(rfc, 3u, NULL);

    CHECK_EQ((long)in_cksum(&p, 0u, 3u), 0x0dfeL,
             "odd total length pads the last byte's low half");
}

static void test_odd_offset(void)
{
    /* From rfc starting at off=1, len=3: bytes 01 f2 03. Parity is relative to
     * off, so 01 is the HIGH byte of word 0: 0x01f2 + 0x0300 = 0x04f2; checksum
     * ~0x04f2 = 0xfb0d. */
    PBUF p = seg(rfc, (USHORT)sizeof(rfc), NULL);

    CHECK_EQ((long)in_cksum(&p, 1u, 3u), 0xfb0dL,
             "word parity is relative to off, not the segment start");
}

static void test_chain_boundary_carry(void)
{
    /* Split the 8-byte RFC vector as {00 01 f2} + {03 f4 f5 f6 f7}: the ODD byte
     * f2 ends segment 1 (global index 2, high half) and 03 opens segment 2
     * (index 3, low half), so the word 0xf203 STRADDLES the boundary. A per-
     * segment sum would pair f2 with a pad and restart 03 as a new high byte --
     * wrong. The chain sum must equal the contiguous sum, 0x220d. */
    PBUF s2 = seg(rfc + 3, 5u, NULL);
    PBUF s1 = seg(rfc, 3u, &s2);
    PBUF contiguous = seg(rfc, 8u, NULL);

    CHECK_EQ((long)in_cksum(&s1, 0u, 8u), 0x220dL,
             "chain sum carries the word across an odd segment boundary");
    CHECK_EQ((long)in_cksum(&s1, 0u, 8u), (long)in_cksum(&contiguous, 0u, 8u),
             "chain sum equals the equivalent contiguous sum");
}

static void test_ip_header_split_odd(void)
{
    /* The real IP header split at an ODD offset (9 | 11) must still verify to 0,
     * proving the boundary carry on a genuine header, not just the RFC vector. */
    PBUF s2 = seg(iphdr_full + 9, 11u, NULL);
    PBUF s1 = seg(iphdr_full, 9u, &s2);

    CHECK_EQ((long)in_cksum(&s1, 0u, 20u), 0L,
             "IP header split at an odd boundary still verifies to 0");
}

static void test_empty(void)
{
    PBUF p = seg(rfc, (USHORT)sizeof(rfc), NULL);

    /* len 0 sums nothing: folded sum 0, checksum ~0 = 0xffff. */
    CHECK_EQ((long)in_cksum(&p, 0u, 0u), 0xffffL,
             "zero-length range checksums to 0xffff");
}

int main(void)
{
    printf("=== nsf370 in_cksum (RFC 1071) tests ===\n");

    test_rfc_example();
    test_ip_header();
    test_odd_length();
    test_odd_offset();
    test_chain_boundary_carry();
    test_ip_header_split_odd();
    test_empty();

    return mbt_test_summary("TSTCKSUM");
}
