/*
 * nsficmp.c -- ICMP echo responder (see nsficmp.h, spec ch. 11).
 *
 * The echo reply is built IN THE SAME PBUF that carried the request (single
 * owner, no allocation): recompute the ICMP checksum with type flipped 8->0,
 * strip the IP header, and hand the ICMP message to nsfip_output with the source
 * and destination swapped -- nsfip_output rebuilds a fresh IP header (new id/TTL,
 * options dropped from the reply, which is RFC-correct). Because the inbound PBUF
 * received the frame at `start` with no headroom, stripping the >=20-byte IP
 * header opens exactly the room nsfip_output needs to prepend its 20-byte header.
 *
 * Every multi-byte field is read/written BYTE BY BYTE (big-endian), never a cast
 * (see nsfip.c). The `ip` argument aliases b->data, so the IP header and the ICMP
 * message live in one contiguous PBUF.
 */
#include "nsficmp.h"
#include "nsfip.h"
#include "nsfcksum.h"
#include "nsfsts.h"
#include "nsftrc.h"

/* big-endian byte-wise accessors (local, as in the codec / nsfip.c). */
static USHORT get16(const UCHAR *p)
{
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

static UINT get32(const UCHAR *p)
{
    return ((UINT)p[0] << 24) | ((UINT)p[1] << 16) |
           ((UINT)p[2] << 8)  |  (UINT)p[3];
}

static void put16(UCHAR *p, USHORT v)
{
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

/* -- statistics (§11.7 ICMP subset, under component NSFICM) ------------------- */
static STSCTR *icmp_in, *icmp_inecho, *icmp_outecho, *icmp_errsent;
static STSCTR *icmp_badcksum, *icmp_indrop;
static int     icmp_stats_ready;

static void icc(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

void nsficmp_init(void)
{
    if (icmp_stats_ready) {
        return;
    }
    icmp_in       = sts_register("NSFICM", "in");
    icmp_inecho   = sts_register("NSFICM", "inecho");
    icmp_outecho  = sts_register("NSFICM", "outecho");
    icmp_errsent  = sts_register("NSFICM", "errsent");
    icmp_badcksum = sts_register("NSFICM", "badcksum");
    icmp_indrop   = sts_register("NSFICM", "indrop");
    icmp_stats_ready = 1;
}

void nsficmp_input(NETDEV *dev, PBUF *b, const IPHDR *ip)
{
    const UCHAR *iph;
    UCHAR       *d;
    UCHAR        hlen, type, code;
    USHORT       total, icmplen;
    UINT         reqsrc, reqdst;

    (void)dev;      /* unused (nsficmp_send_error takes no NETDEV); reserved for M5 */
    if (b == NULL || ip == NULL) {
        if (b != NULL) {
            buf_free(b);
        }
        return;
    }
    icc(icmp_in);

    /* The IP header and the ICMP message share this PBUF; ip aliases b->data. */
    iph  = (const UCHAR *)ip;
    d    = b->data;
    hlen = (UCHAR)((iph[0] & 0x0Fu) * 4u);
    total = get16(iph + 2);
    if (total < (USHORT)hlen) {                 /* nsfip_input already validated */
        icc(icmp_indrop);
        buf_free(b);
        return;
    }
    icmplen = (USHORT)(total - (USHORT)hlen);
    if (icmplen < 4u) {                         /* need type/code/checksum */
        icc(icmp_indrop);
        buf_free(b);
        return;
    }

    /* Verify the ICMP checksum over the whole ICMP message before trusting it. */
    if (in_cksum(b, (USHORT)hlen, icmplen) != 0u) {
        icc(icmp_badcksum);
        buf_free(b);
        return;
    }

    type = d[hlen];
    code = d[hlen + 1u];

    if (type != (UCHAR)NSFICMP_ECHO_REQUEST || code != 0u) {
        /* Non-echo (and echo replies to us): counted and dropped in M2-3. This
         * covers an INBOUND ICMP error message too (one addressed TO us, e.g. a
         * reply to something NSF sent) -- M2-4 only built the OUTBOUND error
         * GENERATOR (nsficmp_send_error); inbound error delivery to transports
         * is still M5. */
        icc(icmp_indrop);
        TRC(ICMP, "drop type %u code %u", (unsigned)type, (unsigned)code);
        buf_free(b);
        return;
    }

    icc(icmp_inecho);

    /* Swap comes from re-sourcing: reply src = request dst (us), reply dst =
     * request src (the pinger). Read the addresses before stripping the header. */
    reqsrc = get32(iph + 12);
    reqdst = get32(iph + 16);

    /* Turn the request into a reply in place: type 8 -> 0, then recompute the
     * ICMP checksum over the (unchanged-length) message. */
    d[hlen]      = (UCHAR)NSFICMP_ECHO_REPLY;
    d[hlen + 2u] = 0x00u;                        /* zero the checksum field */
    d[hlen + 3u] = 0x00u;
    put16(d + hlen + 2u, in_cksum(b, (USHORT)hlen, icmplen));

    /* Strip the IP header so the payload handed to nsfip_output is exactly the
     * ICMP message; nsfip_output prepends a fresh IP header into the room the
     * stripped header just opened. */
    if (buf_trim_head(b, (USHORT)hlen) != 0) {
        icc(icmp_indrop);
        buf_free(b);
        return;
    }

    TRC(ICMP, "echo reply to %u.%u.%u.%u len %u",
        (unsigned)((reqsrc >> 24) & 0xFFu), (unsigned)((reqsrc >> 16) & 0xFFu),
        (unsigned)((reqsrc >> 8) & 0xFFu),  (unsigned)(reqsrc & 0xFFu),
        (unsigned)icmplen);

    /* Hand off ownership. nsfip_output frees + counts on failure (no route /
     * build error); count a sent reply only on success so ownership and the
     * counter never double up. */
    if (nsfip_output(b, reqdst, reqsrc,
                     (UCHAR)NSFIP_PROTO_ICMP, (UCHAR)NSFIP_TTL_DEFAULT) == 0) {
        icc(icmp_outecho);
    }
}

/* -- error generation (M2-4, spec 11.2, RFC 792 + RFC 1122 §3.2.2) ------------- */

#define NSFICMP_ERR_HDRLEN   8      /* type + code + checksum + 4-byte unused  */
#define NSFICMP_QUOTE_DATA   8      /* RFC 792: 64 bits of the original's data */
#define NSFICMP_MAX_QUOTE    (60 + NSFICMP_QUOTE_DATA)   /* max IHL + 8        */

static int is_broadcast_addr(UINT ip) { return ip == 0xFFFFFFFFu; }
static int is_multicast_addr(UINT ip) { return (ip & 0xF0000000u) == 0xE0000000u; }

/* RFC 1122 §3.2.2's "ICMP error message" category: Destination Unreachable,
 * Source Quench, Redirect, Time Exceeded, Parameter Problem. Query/info types
 * (echo, timestamp, mask, ...) are NOT in this set -- an error IS generated in
 * response to an unanswerable echo request, for example. */
static int icmp_is_error_type(UCHAR type)
{
    switch (type) {
    case 3: case 4: case 5: case 11: case 12:
        return 1;
    default:
        return 0;
    }
}

void nsficmp_send_error(const PBUF *orig, UCHAR type, UCHAR code)
{
    UCHAR   quote[NSFICMP_MAX_QUOTE];   /* orig's IP header + 8 B of its payload */
    UCHAR   ehdr[NSFICMP_ERR_HDRLEN];
    UCHAR  *p;
    PBUF   *b;
    USHORT  got, ihl, quotedlen, msglen;
    UCHAR   proto;
    USHORT  fragword;
    UINT    origsrc, origdst;

    if (orig == NULL) {
        return;
    }
    /* Capture as much of orig's header + payload as we can read in one shot --
     * enough for the worst case (60 B of IHL options + 8 B of payload). A short
     * read just clamps `got`; the suppression checks below are defensive against
     * a capture too short to even hold the fixed header. */
    got = buf_copyout(orig, quote, (USHORT)sizeof(quote));
    if (got < (USHORT)NSFIP_HDR_MIN || (quote[0] >> 4) != 4u) {
        return;                          /* can't even quote a v4 header */
    }
    ihl = (USHORT)((quote[0] & 0x0Fu) * 4u);
    if (ihl < (USHORT)NSFIP_HDR_MIN || got < ihl) {
        return;                          /* malformed / truncated capture */
    }

    /* -- RFC 1122 §3.2.2 suppression (never chase an ICMP error storm) -------- */
    fragword = get16(quote + 6);
    if ((fragword & 0x1FFFu) != 0u) {
        return;                          /* non-initial fragment */
    }
    origsrc = get32(quote + 12);
    origdst = get32(quote + 16);
    if (origsrc == 0u || is_broadcast_addr(origsrc) || is_multicast_addr(origsrc)) {
        return;                          /* source does not identify a single host */
    }
    if (is_broadcast_addr(origdst) || is_multicast_addr(origdst)) {
        return;                          /* addressed to broadcast/multicast */
    }
    proto = quote[9];
    if (proto == (UCHAR)NSFIP_PROTO_ICMP && got > ihl &&
        icmp_is_error_type(quote[ihl])) {
        return;                          /* orig is itself an ICMP error message */
    }

    /* -- build the error in a fresh PBUF (orig is read-only, spec 3.4) -------- */
    quotedlen = (USHORT)(ihl + NSFICMP_QUOTE_DATA);
    if (quotedlen > got) {
        quotedlen = got;                 /* the capture ran out first */
    }
    msglen = (USHORT)(NSFICMP_ERR_HDRLEN + quotedlen);

    b = buf_alloc(msglen);
    if (b == NULL) {
        return;                          /* ENOBUFS: drop, not an ABEND (spec 3) */
    }
    ehdr[0] = type;
    ehdr[1] = code;
    ehdr[2] = 0u; ehdr[3] = 0u;           /* checksum, filled in below */
    ehdr[4] = 0u; ehdr[5] = 0u; ehdr[6] = 0u; ehdr[7] = 0u;   /* unused (RFC 792) */
    (void)buf_copyin(b, ehdr, (USHORT)sizeof(ehdr));
    (void)buf_copyin(b, quote, quotedlen);

    p = b->data;                         /* one fresh, contiguous buffer */
    put16(p + 2, in_cksum(b, 0u, msglen));

    TRC(ICMP, "error type %u code %u to %u.%u.%u.%u quoting %u bytes",
        (unsigned)type, (unsigned)code,
        (unsigned)((origsrc >> 24) & 0xFFu), (unsigned)((origsrc >> 16) & 0xFFu),
        (unsigned)((origsrc >> 8) & 0xFFu),  (unsigned)(origsrc & 0xFFu),
        (unsigned)quotedlen);
    nsftrc_hexdump(TRCF_ICMP, "ERR", p, msglen);

    /* nsfip_output takes ownership of b; count a sent error only on success, as
     * the echo responder does for outecho -- ownership and the counter never
     * double up. src = origdst (our own address orig was sent to). */
    if (nsfip_output(b, origdst, origsrc,
                     (UCHAR)NSFIP_PROTO_ICMP, (UCHAR)NSFIP_TTL_DEFAULT) == 0) {
        icc(icmp_errsent);
    }
}
