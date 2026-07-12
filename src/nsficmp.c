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
    (void)icmp_errsent;         /* ICMP error generation is M2-4 */
}

void nsficmp_input(NETDEV *dev, PBUF *b, const IPHDR *ip)
{
    const UCHAR *iph;
    UCHAR       *d;
    UCHAR        hlen, type, code;
    USHORT       total, icmplen;
    UINT         reqsrc, reqdst;

    (void)dev;                                  /* reserved (error path, M2-4) */
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
        /* Non-echo (and echo replies to us): counted and dropped in M2-3; ICMP
         * error handling / delivery to transports is M2-4/M5. */
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
