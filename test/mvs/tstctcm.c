/*
 * tstctcm.c -- NSFCTCI on-MVS validation (spec 9.3; ADR-0019). host = false.
 *
 * TWO parts, deliberately separated because M1-3 is BLOCKED on hardware:
 *
 *  1. SVC 99 seam proof -- RUNS UNDER test-mvs TODAY, no device required.
 *     It drives the shipped driver code (ctci_alloc_unit -> the RB99 build,
 *     the libc370 text-unit builders, the __svc99 linkage) against a
 *     deliberately NONEXISTENT unit ("ZZZZ"), and asserts it fails at unit
 *     resolution with a decoded S99ERROR/S99INFO. Nothing is allocated; the
 *     live system is untouched. This is the M1-3 CI-meaningful assertion.
 *
 *  2. EXCP channel path -- DEFERRED. It allocates + opens the CUU pair, EXCPs
 *     a hand-built block each way, and decodes completion. It CANNOT run yet:
 *     the Hercules side has no CTCI device and the CUU pair is in no UCB. So
 *     it is compiled (proving the whole driver cross-links) but only EXECUTED
 *     when built with -DNSFCTCI_CUU=0xNNNN and a live device present. See the
 *     PR runbook: post code X'7F', length = requested - residual, and a
 *     hwType 0x0800 chain terminating at hwOffset 0x0000.
 *
 * Do NOT allocate a real device speculatively (the system is live): part 1
 * uses only the invalid unit, part 2 stays off unless NSFCTCI_CUU is defined.
 */
#include "nsfctci.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <string.h>
#include <stdio.h>
#include <clibecb.h>            /* ecb_wait / ECB (the deferred device probe) */

/* Guest/host IP addresses for the crafted ICMP echo (part 2, deferred). The
 * runbook sets these to match the live PROFILE HOME + host TUN address. */
#ifndef NSFCTCI_SRC
#define NSFCTCI_SRC  0x0A000002u    /* 10.0.0.2 (guest HOME)                  */
#endif
#ifndef NSFCTCI_DST
#define NSFCTCI_DST  0x0A000001u    /* 10.0.0.1 (host TUN)                    */
#endif
#ifndef NSFCTCI_MTU
#define NSFCTCI_MTU  1500u
#endif

/* ---- helpers ------------------------------------------------------------ */

/* RFC 1071 ones-complement checksum over len bytes. */
static USHORT ip_cksum(const void *p, UINT len)
{
    const UCHAR *b = (const UCHAR *)p;
    UINT         sum = 0;
    UINT         i;

    for (i = 0; i + 1u < len; i += 2u) {
        sum += ((UINT)b[i] << 8) | (UINT)b[i + 1u];
    }
    if (i < len) {
        sum += (UINT)b[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (USHORT)(~sum & 0xFFFFu);
}

static void put16(UCHAR *p, USHORT v) { p[0] = (UCHAR)(v >> 8); p[1] = (UCHAR)v; }
static void put32(UCHAR *p, UINT v)
{
    p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);  p[3] = (UCHAR)v;
}

/* Build a minimal IPv4 + ICMP echo request into buf. Returns the IP length. */
static UINT build_icmp_echo(UCHAR *buf)
{
    UCHAR  *ip   = buf;
    UCHAR  *icmp = buf + 20;
    UINT    iplen = 28u;              /* 20 IP + 8 ICMP, no payload            */

    memset(buf, 0, iplen);
    ip[0]  = 0x45;                    /* version 4, IHL 5                      */
    ip[1]  = 0x00;                    /* TOS                                   */
    put16(&ip[2], (USHORT)iplen);     /* total length                         */
    put16(&ip[4], 0x1234);            /* id                                    */
    put16(&ip[6], 0x0000);            /* flags/frag                            */
    ip[8]  = 64;                      /* TTL                                   */
    ip[9]  = 1;                       /* protocol = ICMP                       */
    put32(&ip[12], NSFCTCI_SRC);
    put32(&ip[16], NSFCTCI_DST);
    put16(&ip[10], ip_cksum(ip, 20)); /* header checksum                       */

    icmp[0] = 8;                      /* type = echo request                   */
    icmp[1] = 0;                      /* code                                  */
    put16(&icmp[4], 0xABCD);          /* identifier                            */
    put16(&icmp[6], 0x0001);          /* sequence                              */
    put16(&icmp[2], ip_cksum(icmp, 8));

    return iplen;
}

/* Wrap an IP packet in one CTCI block: CTCIHDR (points at the terminator) +
 * CTCISEG (0x0800) + IP + terminating CTCIHDR (hwOffset 0). Returns the block
 * length. Big-endian halfwords = native S/370 order. */
static UINT build_ctci_block(UCHAR *blk, const UCHAR *ip, UINT iplen)
{
    UINT seg  = 2u;                   /* CTCISEG follows the leading CTCIHDR   */
    UINT data = seg + 6u;             /* IP follows the 6-byte CTCISEG         */
    UINT term = data + iplen;         /* terminating CTCIHDR                    */

    put16(&blk[0], (USHORT)term);     /* CTCIHDR.hwOffset -> next block         */
    put16(&blk[seg + 0], (USHORT)(6u + iplen));  /* CTCISEG.hwLength           */
    put16(&blk[seg + 2], (USHORT)CTCI_TYPE_IPV4);/* CTCISEG.hwType = 0x0800    */
    put16(&blk[seg + 4], 0x0000);                /* CTCISEG.reserved           */
    memcpy(&blk[data], ip, iplen);
    put16(&blk[term], 0x0000);        /* terminating CTCIHDR.hwOffset = 0      */
    return term + 2u;
}

/* Walk a received CTCI block: follow hwOffset to the 0x0000 terminator,
 * asserting every segment is hwType 0x0800. Returns the segment count, or -1
 * on a malformed chain. */
static int walk_ctci_block(const UCHAR *blk, UINT len)
{
    UINT off  = 0;
    int  segs = 0;

    for (;;) {
        USHORT next, type;
        if (off + 2u > len) {
            return -1;
        }
        next = (USHORT)((blk[off] << 8) | blk[off + 1u]);
        if (next == 0x0000) {
            return segs;              /* terminator reached                     */
        }
        if (off + 8u > len || next > len) {
            return -1;
        }
        type = (USHORT)((blk[off + 4u] << 8) | blk[off + 5u]);
        if (type != CTCI_TYPE_IPV4) {
            return -1;
        }
        segs++;
        off = next;
    }
}

static void hexdump(const char *tag, const UCHAR *p, UINT len)
{
    UINT i;
    if (len > 64u) {
        len = 64u;                    /* first 64 bytes are plenty for a probe */
    }
    printf("%s (%u bytes):", tag, (unsigned)len);
    for (i = 0; i < len; i++) {
        printf("%s%02X", (i % 16u) ? " " : "\n  ", (unsigned)p[i]);
    }
    printf("\n");
}

/* ---- part 1: SVC 99 seam proof (runs today) ----------------------------- */

static int run_svc99_proof(void)
{
    char  ddn[9];
    short err  = 0;
    short info = 0;
    int   rc;

    memset(ddn, 0, sizeof ddn);
    printf("--- SVC 99 seam proof: allocate nonexistent unit ZZZZ ---\n");
    rc = ctci_alloc_unit("ZZZZ", ddn, &err, &info);
    printf("ctci_alloc_unit(ZZZZ) rc=%d  S99 ERROR=%04X INFO=%04X  ddn=[%s]\n",
           rc, (unsigned)(err & 0xFFFF), (unsigned)(info & 0xFFFF), ddn);

    CHECK(rc != 0, "SVC 99 allocate of nonexistent unit ZZZZ fails");
    CHECK(err != 0, "S99ERROR is set (RB99/text-unit/__svc99 linkage works)");
    return 0;
}

/* ---- part 2: EXCP channel path (deferred; runs only with NSFCTCI_CUU) ---- */

#ifdef NSFCTCI_CUU
static int run_device_probe(void)
{
    static UCHAR block[64];
    UCHAR        ip[64];
    UINT         iplen, blklen, rlen = 0;
    UCHAR        post = 0;
    CTCIDEV     *d;
    int          segs;

    printf("--- EXCP channel path: CUU %04X ---\n", (unsigned)NSFCTCI_CUU);

    if (ctci_reserve(1u, CTCI_BUF_DEFAULT) != 0) {
        printf("ctci_reserve failed\n");
        return 1;
    }
    mm_init_complete();

    d = ctci_dev_open((USHORT)NSFCTCI_CUU, (USHORT)NSFCTCI_MTU);
    CHECK(d != NULL, "CUU pair allocated + opened");
    if (d == NULL) {
        return 1;
    }
    printf("device up: DD %s/%s\n", d->rddn, d->wddn);

    /* WRITE: hand-build one CTCIHDR+CTCISEG(0x0800)+ICMP-echo block. */
    iplen  = build_icmp_echo(ip);
    blklen = build_ctci_block(d->wbuf, ip, iplen);
    hexdump("WRITE block", d->wbuf, blklen);
    ctci_dev_write(d, d->wbuf, blklen);
    (void)ecb_wait((ECB *)&d->wecb);
    ctci_dev_status(d, 1, blklen, &rlen, &post);
    printf("WRITE post=%02X len=%u (requested %u)\n",
           (unsigned)post, (unsigned)rlen, (unsigned)blklen);
    CHECK(post == 0x7F, "WRITE completed with post code X'7F'");

    /* READ: host `ping <guest HOME>` triggers an inbound block. A READ with no
     * traffic BLOCKS inside Hercules -- that is expected (MIH is M1-4), so the
     * ping is the trigger. */
    printf("waiting for inbound frame (ping the guest HOME now)...\n");
    ctci_dev_read(d);
    (void)ecb_wait((ECB *)&d->recb);
    ctci_dev_status(d, 0, d->bufsize, &rlen, &post);
    printf("READ  post=%02X len=%u (residual arithmetic: %u - residual)\n",
           (unsigned)post, (unsigned)rlen, (unsigned)d->bufsize);
    CHECK(post == 0x7F, "READ completed with post code X'7F'");
    hexdump("READ block", d->rbuf0, rlen);
    segs = walk_ctci_block(d->rbuf0, rlen);
    printf("chain walk: %d segment(s), hwType 0x0800, terminates at 0x0000\n",
           segs);
    CHECK(segs >= 1, "received block chain walks to hwOffset 0 (all 0x0800)");

    ctci_dev_close(d);
    return 0;
}
#endif /* NSFCTCI_CUU */

int main(void)
{
    printf("=== nsf370 NSFCTCI MVS validation (M1-3) ===\n");

    mm_init(NULL);

    run_svc99_proof();

#ifdef NSFCTCI_CUU
    run_device_probe();
#else
    mm_init_complete();
    printf("--- EXCP channel path DEFERRED ---\n");
    printf("no CTCI device configured; rebuild with -DNSFCTCI_CUU=0xNNNN and a\n");
    printf("live 3088 pair to drive the channel path (see PR runbook).\n");
#endif

    mm_shutdown();
    return mbt_test_summary("TSTCTCM");
}
