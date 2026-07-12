/*
 * tstctcif.c -- CTCI frame codec unit tests (spec 9.3 / ADR-0020). Pure and
 * portable: no device, no NSFMM, no PBUFs -- just the byte<->byte transform.
 *
 * The decisive tests anchor on LITERAL byte vectors transcribed from the
 * corrected spec 9.3 (READ = one block of segments walked by hwLength up to the
 * leading hwOffset, NO 0x0000 terminator to the guest, hwType a constant 0x0800
 * marker; WRITE = the same plus an appended hwOffset=0x0000 block). Round-trips
 * sit on top, but a round-trip alone cannot catch a USHORT-vs-byte-wise
 * endianness regression (it is self-consistent on either box) -- the literal
 * assertions are what lock the wire order.
 */
#include "nsfctcif.h"
#include <mbtcheck.h>
#include <string.h>
#include <stdio.h>

/* A recognizable 20-byte IPv4 header (version nibble 4 -> 0x45). Contents are
 * irrelevant to the codec (it only reads the first nibble); the fixed bytes let
 * a test assert an exact payload came back. */
static const UCHAR IP20[20] = {
    0x45, 0x00, 0x00, 0x14, 0xAB, 0xCD, 0x00, 0x00,
    0x40, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x02,
    0x0A, 0x00, 0x00, 0x01
};

/* An 8-byte "IPv6" packet: first nibble 6 -> version 6. Hercules would still
 * stamp its CTCISEG hwType 0x0800 (ADR-0020), so the decoder must reject it by
 * the packet's own version, not by hwType. */
static const UCHAR IP6_8[8] = {
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3A, 0xFF
};

/* ---- decode: one block, one IPv4 segment (literal vector) ---- */
static void t_decode_one(void)
{
    /* [CTCIHDR 001C][CTCISEG 001A 0800 0000][IP20]  -- 28 bytes, NO terminator. */
    static const UCHAR blk[28] = {
        0x00, 0x1C,                         /* hwOffset = 28 (end of data)       */
        0x00, 0x1A, 0x08, 0x00, 0x00, 0x00, /* hwLength=26 hwType=0800 rsvd=0    */
        0x45, 0x00, 0x00, 0x14, 0xAB, 0xCD, 0x00, 0x00,
        0x40, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x02,
        0x0A, 0x00, 0x00, 0x01              /* IP20                              */
    };
    CTCIFDEC     d;
    const UCHAR *p;
    USHORT       plen = 0;

    ctcif_dec_init(&d, blk, sizeof(blk));
    p = ctcif_dec_next(&d, &plen);
    CHECK(p != NULL, "decode one: first segment returned");
    CHECK_EQ((long)plen, 20, "decode one: payload length is 20");
    CHECK(p != NULL && memcmp(p, IP20, 20) == 0, "decode one: exact IP payload");
    CHECK(p == blk + 8, "decode one: payload points into the block (no copy)");
    CHECK(ctcif_dec_next(&d, &plen) == NULL, "decode one: no second segment");
    CHECK_EQ((long)d.nonipv4, 0, "decode one: no non-IPv4 drops");
    CHECK_EQ((long)d.malformed, 0, "decode one: not malformed");
}

/* ---- decode: many segments, an IPv6-stamped-0x0800 dropped mid-run ---- */
static void t_decode_many(void)
{
    /* seg A = IP20, seg B = IP6_8 (version 6, dropped), seg C = IP20. The
     * leading hwOffset spans all three; there is no 0x0000 terminator. */
    UCHAR  blk[128];
    UINT   off = 2u;                        /* first CTCISEG after the CTCIHDR    */
    CTCIFDEC     d;
    const UCHAR *p;
    USHORT       plen = 0;
    int          got = 0;

    /* seg A */
    blk[off + 0] = 0x00; blk[off + 1] = (UCHAR)(6 + 20);   /* hwLength 26        */
    blk[off + 2] = 0x08; blk[off + 3] = 0x00;              /* hwType 0x0800      */
    blk[off + 4] = 0x00; blk[off + 5] = 0x00;
    memcpy(&blk[off + 6], IP20, 20); off += 26u;
    /* seg B (IPv6, but stamped 0x0800) */
    blk[off + 0] = 0x00; blk[off + 1] = (UCHAR)(6 + 8);    /* hwLength 14        */
    blk[off + 2] = 0x08; blk[off + 3] = 0x00;              /* hwType 0x0800      */
    blk[off + 4] = 0x00; blk[off + 5] = 0x00;
    memcpy(&blk[off + 6], IP6_8, 8); off += 14u;
    /* seg C */
    blk[off + 0] = 0x00; blk[off + 1] = (UCHAR)(6 + 20);   /* hwLength 26        */
    blk[off + 2] = 0x08; blk[off + 3] = 0x00;
    blk[off + 4] = 0x00; blk[off + 5] = 0x00;
    memcpy(&blk[off + 6], IP20, 20); off += 26u;
    /* leading CTCIHDR.hwOffset = end of the last segment */
    blk[0] = (UCHAR)(off >> 8); blk[1] = (UCHAR)off;

    ctcif_dec_init(&d, blk, off);
    while ((p = ctcif_dec_next(&d, &plen)) != NULL) {
        CHECK_EQ((long)plen, 20, "decode many: each delivered segment is IPv4/20");
        CHECK(memcmp(p, IP20, 20) == 0, "decode many: exact IPv4 payload");
        got++;
    }
    CHECK_EQ((long)got, 2, "decode many: two IPv4 segments delivered");
    CHECK_EQ((long)d.nonipv4, 1, "decode many: the IPv6-stamped-0x0800 dropped+counted");
    CHECK_EQ((long)d.malformed, 0, "decode many: not malformed");
}

/* ---- decode: stop at hwOffset even with trailing bytes past it ---- */
static void t_decode_stops_at_offset(void)
{
    /* Real block is the 28-byte one-segment case, but we hand the decoder a
     * LONGER buffer whose tail past hwOffset looks like another valid segment.
     * A decoder that chased a 0x0000 terminator (or ignored hwOffset) would walk
     * into it; the correct decoder stops at hwOffset = 28. */
    UCHAR   blk[48];
    CTCIFDEC     d;
    USHORT       plen = 0;
    UINT         off;

    memset(blk, 0, sizeof(blk));
    blk[0] = 0x00; blk[1] = 0x1C;                          /* hwOffset = 28      */
    blk[2] = 0x00; blk[3] = 0x1A; blk[4] = 0x08; blk[5] = 0x00; /* seg 26/0800   */
    memcpy(&blk[8], IP20, 20);
    /* trailing "segment" AFTER hwOffset that must be ignored */
    off = 28u;
    blk[off + 0] = 0x00; blk[off + 1] = (UCHAR)(6 + 8);
    blk[off + 2] = 0x08; blk[off + 3] = 0x00;
    memcpy(&blk[off + 6], IP20, 8);

    ctcif_dec_init(&d, blk, sizeof(blk));
    CHECK(ctcif_dec_next(&d, &plen) != NULL, "decode stop: first segment ok");
    CHECK(ctcif_dec_next(&d, &plen) == NULL,
          "decode stop: nothing walked past hwOffset (no 0x0000 chase)");
    CHECK_EQ((long)d.malformed, 0, "decode stop: clean stop, not malformed");
}

/* ---- decode: malformed segments are drop+count, never a crash ---- */
static void t_decode_malformed(void)
{
    CTCIFDEC     d;
    USHORT       plen = 0;

    /* (a) hwLength < 6 (here 3) cannot advance the walk -> malformed, stop. */
    {
        UCHAR blk[12] = { 0x00, 0x0C, 0x00, 0x03, 0x08, 0x00, 0, 0, 0, 0, 0, 0 };
        ctcif_dec_init(&d, blk, sizeof(blk));
        CHECK(ctcif_dec_next(&d, &plen) == NULL, "malformed: hwLength<6 yields nothing");
        CHECK_EQ((long)d.malformed, 1, "malformed: hwLength<6 flagged");
    }
    /* (b) segment runs past hwOffset (hwLength 40 but only ~10 bytes to end). */
    {
        UCHAR blk[12] = { 0x00, 0x0C, 0x00, 0x28, 0x08, 0x00, 0x45, 0, 0, 0, 0, 0 };
        ctcif_dec_init(&d, blk, sizeof(blk));
        CHECK(ctcif_dec_next(&d, &plen) == NULL, "malformed: seg past hwOffset yields nothing");
        CHECK_EQ((long)d.malformed, 1, "malformed: over-long segment flagged");
    }
    /* (c) hwOffset beyond the transferred length is clamped, not chased. A block
     *     claiming hwOffset 0x00FF but only 10 bytes read: end clamps to 10, and
     *     the single short seg is walked or safely stopped -- never past byte 10. */
    {
        UCHAR blk[10] = { 0x00, 0xFF, 0x00, 0x0A, 0x08, 0x00, 0x45, 0x00, 0x00, 0x14 };
        ctcif_dec_init(&d, blk, sizeof(blk));
        CHECK_EQ((long)d.end, 10, "malformed: hwOffset clamped to transferred length");
        /* seg hwLength 10, off 2 -> off+10 = 12 > end(10) -> malformed stop. */
        CHECK(ctcif_dec_next(&d, &plen) == NULL, "malformed: clamped walk stops safely");
        CHECK_EQ((long)d.malformed, 1, "malformed: clamped over-long seg flagged");
    }
    /* (d) a runt block (< CTCIHDR) yields nothing and is not malformed. */
    {
        UCHAR blk[1] = { 0x00 };
        ctcif_dec_init(&d, blk, 1u);
        CHECK(ctcif_dec_next(&d, &plen) == NULL, "malformed: runt block yields nothing");
        CHECK_EQ((long)d.malformed, 0, "malformed: runt block not flagged malformed");
    }
}

/* ---- encode: exact literal output vector, including the 0x0000 terminator ---- */
static void t_encode_literal(void)
{
    /* Expected: [00 1C][00 1A 08 00 00 00][IP20][00 00]  -- 30 bytes. */
    static const UCHAR want[30] = {
        0x00, 0x1C,                         /* leading hwOffset = 28             */
        0x00, 0x1A, 0x08, 0x00, 0x00, 0x00, /* CTCISEG 26/0800/0                 */
        0x45, 0x00, 0x00, 0x14, 0xAB, 0xCD, 0x00, 0x00,
        0x40, 0x01, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x02,
        0x0A, 0x00, 0x00, 0x01,             /* IP20                              */
        0x00, 0x00                          /* terminating CTCIHDR.hwOffset = 0  */
    };
    UCHAR blk[64];
    UINT  n;

    memset(blk, 0xEE, sizeof(blk));
    n = ctcif_encode(blk, sizeof(blk), IP20, 20u);
    CHECK_EQ((long)n, 30, "encode: block length is 30 (8 + 20 + 2)");
    CHECK(memcmp(blk, want, 30) == 0, "encode: exact literal wire bytes incl. 0x0000 term");
}

/* ---- encode: refuses to overrun a too-small buffer ---- */
static void t_encode_toobig(void)
{
    UCHAR blk[16];
    CHECK_EQ((long)ctcif_encode(blk, sizeof(blk), IP20, 20u), 0,
             "encode: returns 0 when the block would overrun");
    CHECK_EQ((long)ctcif_encode(blk, sizeof(blk), IP20, 0u), 0,
             "encode: returns 0 for an empty packet");
}

/* ---- max-size frame + round-trip ---- */
static void t_roundtrip_max(void)
{
    enum { BUFSZ = 0x5000u };               /* default CTCI I/O buffer            */
    static UCHAR big[CTCI_MAX_FRAME(BUFSZ) > 1500u ? 1500u : 1500u]; /* 1500-B IP */
    UCHAR        blk[BUFSZ];
    CTCIFDEC     d;
    const UCHAR *p;
    USHORT       plen = 0;
    UINT         n, i;

    for (i = 0u; i < sizeof(big); i++) {
        big[i] = (UCHAR)(i & 0xFFu);
    }
    big[0] = 0x45;                          /* make it decode as IPv4            */

    n = ctcif_encode(blk, sizeof(blk), big, (UINT)sizeof(big));
    CHECK_EQ((long)n, (long)(8u + sizeof(big) + 2u), "roundtrip: encoded length");
    ctcif_dec_init(&d, blk, n - 2u);        /* the guest never receives the term  */
    p = ctcif_dec_next(&d, &plen);
    CHECK(p != NULL, "roundtrip: decode returns the segment");
    CHECK_EQ((long)plen, (long)sizeof(big), "roundtrip: payload length preserved");
    CHECK(p != NULL && memcmp(p, big, sizeof(big)) == 0, "roundtrip: payload bytes preserved");
    CHECK(ctcif_dec_next(&d, &plen) == NULL, "roundtrip: exactly one segment");
    CHECK_EQ((long)d.nonipv4, 0, "roundtrip: no drops");
    CHECK_EQ((long)d.malformed, 0, "roundtrip: not malformed");
}

int main(void)
{
    printf("=== nsf370 NSFCTCI codec (nsfctcif) tests ===\n");
    t_decode_one();
    t_decode_many();
    t_decode_stops_at_offset();
    t_decode_malformed();
    t_encode_literal();
    t_encode_toobig();
    t_roundtrip_max();
    return mbt_test_summary("TSTCTCIF");
}
