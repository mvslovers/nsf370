/*
 * nsfctcif.c -- the CTCI frame codec (see nsfctcif.h, spec 9.3 / ADR-0020).
 *
 * Pure, portable, allocation-free byte<->byte transform between the Hercules
 * 3088 block layout ([CTCIHDR] ([CTCISEG][IP]) ...) and raw IP packets. It has
 * NO dependency on NSFMM, PBUF, the channel or the DEVOPS contract, so it runs
 * identically host and MVS and is unit-tested with literal byte vectors.
 *
 * Every halfword is packed/unpacked BYTE BY BYTE (b[0] = hi, b[1] = lo). This is
 * deliberate: the wire is big-endian and so is S/370, so a USHORT store/load
 * would be correct ON MVS -- but it would emit host-endian bytes on the
 * little-endian test box while round-tripping green, hiding the defect. Byte-wise
 * is correct on both, and the tests assert literal bytes to lock it in.
 */
#include "nsfctcif.h"

/* Big-endian halfword accessors (byte-wise -- see the file header). */
static USHORT get16(const UCHAR *p)
{
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

static void put16(UCHAR *p, USHORT v)
{
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)v;
}

/* -- Decode -------------------------------------------------------------------
 * The block carries no 0x0000 terminator for the guest (ADR-0020): the walk
 * runs from sizeof(CTCIHDR) up to the leading hwOffset, and hwOffset is clamped
 * to the transferred length so a short read can never send the walk past the
 * data. */
void ctcif_dec_init(CTCIFDEC *d, const UCHAR *blk, UINT len)
{
    UINT end;

    d->blk       = blk;
    d->off       = (UINT)sizeof(CTCIHDR);   /* first CTCISEG follows the CTCIHDR */
    d->nonipv4   = 0u;
    d->malformed = 0u;

    if (len < (UINT)sizeof(CTCIHDR)) {
        d->end = 0u;                        /* runt block: no segments           */
        return;
    }
    end = (UINT)get16(blk);                 /* leading hwOffset = end-of-data     */
    if (end > len) {
        end = len;                          /* defensive: hwOffset beyond read    */
    }
    d->end = end;
}

const UCHAR *ctcif_dec_next(CTCIFDEC *d, USHORT *plen)
{
    while (d->off + (UINT)sizeof(CTCISEG) <= d->end) {
        USHORT       seglen = get16(d->blk + d->off);           /* hwLength       */
        UINT         paylen;
        const UCHAR *payload;

        if (seglen < (USHORT)sizeof(CTCISEG) ||
            d->off + (UINT)seglen > d->end) {
            /* hwLength underflows the 6-byte header (would not advance the walk),
             * or the segment runs past the end -- stop; the rest is unusable. */
            d->malformed = 1u;
            return NULL;
        }

        payload = d->blk + d->off + (UINT)sizeof(CTCISEG);
        paylen  = (UINT)seglen - (UINT)sizeof(CTCISEG);
        d->off += (UINT)seglen;                                 /* advance always */

        /* IP version comes from the packet, never from hwType (ADR-0020). v1 is
         * IPv4-only: drop + count anything else (empty segment included, since
         * its version cannot be read) and continue with the next segment. */
        if (paylen == 0u || (payload[0] >> 4) != 4u) {
            d->nonipv4++;
            continue;
        }

        if (plen != NULL) {
            *plen = (USHORT)paylen;
        }
        return payload;
    }
    return NULL;                                                /* block exhausted */
}

/* -- Encode -------------------------------------------------------------------
 * [CTCIHDR hwOffset=end][CTCISEG hwLength/0x0800/0][IP][CTCIHDR hwOffset=0].
 * `end` is the offset where the terminating block begins = end-of-data. This is
 * the framing validated live in M1-3 (ADR-0020); do not change it.
 *
 * ctcif_encode_hdr writes ONLY the framing halfwords, assuming the IP packet is
 * already at blk + 8; ctcif_encode copies the packet in first and then frames.
 * Both share the one framing implementation, so the literal-vector test locks it
 * for the driver's in-place path too. */
UINT ctcif_encode_hdr(UCHAR *blk, UINT blksize, UINT iplen)
{
    UINT hdr  = (UINT)sizeof(CTCIHDR);      /* 2: leading block header            */
    UINT seg  = (UINT)sizeof(CTCISEG);      /* 6: one segment header              */
    UINT data = hdr + seg;                  /* 8: IP payload sits here            */
    UINT end  = data + iplen;               /* offset of the terminating block    */
    UINT total = end + hdr;                 /* + terminating CTCIHDR              */

    if (iplen == 0u || total > blksize) {
        return 0u;                          /* nothing to send, or would overrun  */
    }

    put16(blk + 0u,       (USHORT)end);            /* leading CTCIHDR.hwOffset    */
    put16(blk + hdr,      (USHORT)(seg + iplen));  /* CTCISEG.hwLength            */
    put16(blk + hdr + 2u, (USHORT)CTCI_TYPE_IPV4); /* CTCISEG.hwType             */
    put16(blk + hdr + 4u, 0x0000u);                /* CTCISEG.hwRsvd             */
    put16(blk + end,      0x0000u);                /* terminating CTCIHDR = 0    */
    return total;
}

UINT ctcif_encode(UCHAR *blk, UINT blksize, const UCHAR *ip, UINT iplen)
{
    UINT data = (UINT)sizeof(CTCIHDR) + (UINT)sizeof(CTCISEG);   /* 8 */
    UINT total = data + iplen + (UINT)sizeof(CTCIHDR);
    UINT i;

    if (iplen == 0u || total > blksize) {
        return 0u;
    }
    for (i = 0u; i < iplen; i++) {
        blk[data + i] = ip[i];
    }
    return ctcif_encode_hdr(blk, blksize, iplen);
}
