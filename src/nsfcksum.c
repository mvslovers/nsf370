/*
 * nsfcksum.c -- the Internet checksum (see nsfcksum.h, spec 11.5 / RFC 1071).
 *
 * A one's-complement sum with end-around carry over a logical byte range of a
 * PBUF chain, treated as one contiguous big-endian 16-bit-word stream. The word
 * parity is tracked GLOBALLY (by `taken`, the count of bytes summed so far,
 * relative to `off`) rather than per segment, so a word straddling a segment
 * boundary -- an odd byte ending one PBUF and the next byte opening the next --
 * is paired correctly. Every byte is read individually and placed in the high or
 * low half of its word by parity, so the routine is byte-order-correct on the
 * little-endian test host as well as on big-endian S/370 (see the header).
 *
 * SEED / FOLD split (M3-3, ADR-0028). The accumulation is factored into
 * in_cksum_partial (seed in, unfolded 32-bit partial out) + in_cksum_fold (the
 * end-around carry + complement). in_cksum is the M2 routine expressed as
 * fold(partial(..., 0)), so its result is unchanged; the pseudo-header seed of a
 * UDP/TCP checksum is just a partial sum threaded into `seed`. There is no
 * second checksum routine -- the ONE `taken`-parity loop lives here.
 */
#include "nsfcksum.h"

UINT in_cksum_partial(const PBUF *chain, USHORT off, USHORT len, UINT seed)
{
    UINT        sum     = seed; /* running one's-complement sum (folded by caller)*/
    UINT        skipped = 0u;   /* bytes of the leading `off` still to skip     */
    UINT        taken   = 0u;   /* bytes summed so far == word-parity index     */
    const PBUF *p;

    for (p = chain; p != NULL && taken < (UINT)len; p = p->chain) {
        const UCHAR *d = p->data;
        UINT         seglen = (UINT)p->len;
        UINT         i = 0u;

        /* Skip forward until we have consumed `off` bytes of the logical stream.
         * A whole segment may lie entirely before `off`. */
        if (skipped < (UINT)off) {
            UINT need = (UINT)off - skipped;

            if (need >= seglen) {
                skipped += seglen;          /* this whole segment is before off  */
                continue;
            }
            i        = need;                /* off lands inside this segment      */
            skipped  = (UINT)off;
        }

        /* Sum the in-range bytes of this segment, high/low half by global parity.
         * `taken` even -> high byte of the current word; odd -> low byte. An odd
         * total `len` leaves the last byte in the high half with an implicit zero
         * low half (RFC 1071 padding). */
        for (; i < seglen && taken < (UINT)len; i++) {
            if ((taken & 1u) == 0u) {
                sum += (UINT)d[i] << 8;
            } else {
                sum += (UINT)d[i];
            }
            taken++;
        }
    }
    return sum;
}

USHORT in_cksum_fold(UINT sum)
{
    /* Fold the carries out of the top half (end-around carry) and complement. */
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (USHORT)(~sum & 0xFFFFu);
}

USHORT in_cksum(const PBUF *chain, USHORT off, USHORT len)
{
    return in_cksum_fold(in_cksum_partial(chain, off, len, 0u));
}
