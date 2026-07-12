#ifndef NSFCTCIF_H
#define NSFCTCIF_H
/*
 * nsfctcif.h -- the CTCI frame codec (CTCIHDR/CTCISEG <-> raw IP), spec 9.3 as
 * corrected by ADR-0020. Its OWN module, driver-independent: it walks / builds
 * the on-the-wire block layout and knows nothing of NSFMM, PBUFs, DEVOPS or the
 * channel. NSFCTCI (src/nsfctcib.c) drives it -- decode each IPv4 payload into a
 * PBUF, encode a PBUF into the write buffer -- so the framing is host-testable
 * with plain byte arrays, with no device (test/tstctcif.c).
 *
 * READ/WRITE ASYMMETRY (normative, verified live against Hercules ctc_ctci.c,
 * issue #16 -- ADR-0020). Read this before touching the decoder:
 *
 *  - INBOUND (decode, host->guest): the guest receives ONE block of many
 *    segments. The leading CTCIHDR.hwOffset is the END-of-data offset. Hercules
 *    writes a terminating hwOffset = 0x0000 into ITS OWN buffer but does NOT
 *    transfer it to the guest, so the decoder MUST walk segments by hwLength
 *    from sizeof(CTCIHDR) up to hwOffset and STOP there -- it must NEVER look for
 *    a 0x0000 halfword (there is none). CTCISEG.hwType is a CONSTANT 0x0800
 *    marker Hercules stamps on every frame (an IPv6 MLD packet arrived stamped
 *    0x0800 on the first live READ), so the IP version is read from the packet
 *    (first nibble), never from hwType. v1 is IPv4-only: a non-IPv4 segment is
 *    dropped and counted, and the walk continues with the rest.
 *
 *  - OUTBOUND (encode, guest->host): the guest builds [CTCIHDR hwOffset=end]
 *    [CTCISEG hwType=0x0800][IP] and APPENDS a terminating [CTCIHDR
 *    hwOffset=0x0000] block; Hercules reads up to that zero. This framing wrote
 *    post X'7F' and reached the host TUN in M1-3 -- it is validated, do not
 *    "fix" it. v1 emits ONE segment per block (batching several PBUFs into one
 *    block is a later optimisation).
 *
 * ENDIANNESS. All halfwords are big-endian = native S/370 order, so ON MVS there
 * is no conversion. But this codec is host-tested on a little-endian box, so it
 * packs/unpacks every halfword BYTE BY BYTE (b[0] = hi, b[1] = lo) -- never via a
 * USHORT store/load, which would round-trip green on both host and MVS while
 * emitting different wire bytes on each. The host tests therefore anchor on
 * LITERAL byte vectors transcribed from spec 9.3, not only on encode->decode
 * round-trips.
 */

#include "nsf.h"

/* -- Wire format (spec 9.3; normative). A block is [CTCIHDR] ([CTCISEG][IP]) ...
 * All halfwords big-endian. These live here (the codec owns the wire layout);
 * nsfctci.h includes this header for them. */
typedef struct ctcihdr {
    USHORT hwOffset;             /* block header: END-of-data offset (READ);   */
                                 /* 0x0000 terminates a WRITE block            */
} CTCIHDR;                       /* 2 bytes */
NSF_SIZE_ASSERT(CTCIHDR, 2);

typedef struct ctciseg {
    USHORT hwLength;             /* segment length INCLUDING this 6-B header    */
    USHORT hwType;               /* constant 0x0800 marker (NOT a v4/v6 tag)    */
    USHORT hwRsvd;               /* always 0x0000                               */
} CTCISEG;                       /* 6 bytes */
NSF_SIZE_ASSERT(CTCISEG, 6);

#define CTCI_TYPE_IPV4     0x0800u    /* CTCISEG.hwType Hercules stamps on all  */

/* Largest IP frame that fits one block of `buf` bytes (leading CTCIHDR +
 * CTCISEG + IP + terminating CTCIHDR). */
#define CTCI_MAX_FRAME(buf) \
    ((UINT)(buf) - (UINT)sizeof(CTCIHDR) - (UINT)sizeof(CTCISEG) - 2u)

/* asm() external-symbol aliases (CLAUDE.md §3). The codec uses a distinct
 * 8-char scheme NSFCK* (CtcKodec), unique across the load module and clear of
 * the NSFCI* CTCI-driver names (e.g. NSFCIFDN = ctci_free_ddn):
 *   ctcif_dec_init NSFCKDI   ctcif_dec_next NSFCKDN   ctcif_encode NSFCKEN
 *   ctcif_encode_hdr NSFCKEH
 */

/* -- Decode (inbound): a stateful walk over ONE received block ---------------
 * ctcif_dec_init pins the block and clamps the walk end to min(hwOffset, len).
 * ctcif_dec_next returns the next IPv4 payload (pointer INTO blk) and its length
 * in *plen, or NULL when the block is exhausted. Non-IPv4 segments are skipped
 * and counted in `nonipv4`; a malformed segment (hwLength < 6, or running past
 * the walk end) stops the walk and sets `malformed`. The caller adds nonipv4 +
 * malformed to its inbound-error counter. The codec never allocates and never
 * abends. */
typedef struct ctcifdec {
    const UCHAR *blk;            /* the received block                          */
    UINT         end;           /* min(hwOffset, len): where segments stop      */
    UINT         off;           /* offset of the next segment header            */
    UINT         nonipv4;       /* non-IPv4 segments dropped (running)          */
    UINT         malformed;     /* 1 once the walk stopped on a bad segment     */
} CTCIFDEC;

void         ctcif_dec_init(CTCIFDEC *d, const UCHAR *blk, UINT len) asm("NSFCKDI");
const UCHAR *ctcif_dec_next(CTCIFDEC *d, USHORT *plen) asm("NSFCKDN");

/* -- Encode (outbound): build one WRITE block from one IP packet -------------
 * Writes [CTCIHDR hwOffset=end][CTCISEG 0x0800][IP][CTCIHDR 0x0000] into `blk`
 * (capacity `blksize`). Returns the block length, or 0 if it would not fit
 * (the caller then drops + counts). Copies `ip`/`iplen` verbatim. */
UINT         ctcif_encode(UCHAR *blk, UINT blksize,
                          const UCHAR *ip, UINT iplen) asm("NSFCKEN");

/* Like ctcif_encode but ASSUMES the IP packet is ALREADY at blk + sizeof(CTCIHDR)
 * + sizeof(CTCISEG) (offset 8) -- it writes only the framing halfwords (leading
 * CTCIHDR, CTCISEG, terminating CTCIHDR) around it. The driver copies the PBUF's
 * IP bytes straight into the write buffer (buf_copyout), then frames in place,
 * avoiding a second copy and a large executive-stack temp. Returns the block
 * length, or 0 if iplen == 0 or it would overrun blksize. */
UINT         ctcif_encode_hdr(UCHAR *blk, UINT blksize, UINT iplen) asm("NSFCKEH");

#endif /* NSFCTCIF_H */
