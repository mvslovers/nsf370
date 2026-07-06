#ifndef NSFBUF_H
#define NSFBUF_H
/*
 * nsfbuf.h -- the Buffer Manager (spec ch. 03).
 *
 * Packet buffers (PBUF) in two size classes, carved from the NSFMM pools
 * BUFSMALL (256 B data area) and BUFLARGE (2048 B data area). Two classes,
 * not one, because ACK-sized TCP segments and ICMP messages would waste ~87%
 * of a 2 KB buffer and 24-bit storage is the scarcest resource (ADR-0009).
 *
 * Ownership is single-owner, no reference counting (ADR-0008): a function
 * either KEEPS a PBUF or PASSES it on -- never both, never neither. Only the
 * executive task frees buffers; exits queue them for freeing. Each interface
 * below states which side of the handoff it is on.
 *
 * Layout of one pooled buffer object (spec 3.3):
 *
 *   +----------+---------------+----------------------+----------------+
 *   | MMOBJ 8B | PBUF hdr 32B  | headroom (64B deflt) |   data area    |
 *   +----------+---------------+----------------------+----------------+
 *   ^ mm object ^ pbuf          ^ start                ^ data (outbound)
 *
 * The MMOBJ header is owned by NSFMM and sits BELOW the pointer NSFMM hands
 * back; PBUF is at the very start of that payload, so mm_free(pool, pbuf)
 * routes a free straight through the NSFMM double-free / eyecatcher / wrong-
 * pool protection. `start` is the first byte of the B-byte data area,
 * `start = (UCHAR*)pbuf + sizeof(PBUF)`. Outbound buffers open `data` one
 * headroom in so IP (20 B) and any future link header can be PREPENDED into
 * the headroom without copying; inbound (M1) receives at `start` via
 * buf_reset_rx (nothing is prepended on the way up).
 *
 * Field `size` is the forward capacity from `data`: size == (start+B) - data,
 * so the standing invariant is simply `len <= size` (equivalently
 * `start <= data && data + len <= start + B`). buf_prepend grows size,
 * buf_trim_head shrinks it, buf_trim_tail leaves it (see nsfbuf.c).
 */

#include "nsf.h"
#include "nsfque.h"             /* QELEM (queue linkage embedded in PBUF) */

/* Data-area sizes per class and the default headroom (spec 2.5, 3.3).
 * The MM object payload is sizeof(PBUF) + one of these. NSFCFG overrides of
 * the counts arrive at M0-7; the sizes/headroom are fixed layout. */
#define NSFBUF_SMALL_DATA   256     /* B for BUFSMALL                        */
#define NSFBUF_LARGE_DATA   2048    /* B for BUFLARGE                        */
#define NSFBUF_HEADROOM     64      /* outbound prepend room ahead of data   */
#define NSFBUF_SMALL_COUNT  64      /* default BUFSMALL object count         */
#define NSFBUF_LARGE_COUNT  128     /* default BUFLARGE object count         */

/* Size class, stored in PBUF.class; selects the owning pool on free. */
#define NSFBUF_CLASS_SMALL  0
#define NSFBUF_CLASS_LARGE  1

/* Packet buffer header (spec 3.3). 32 bytes on the S/370 target. */
typedef struct pbuf PBUF;
struct pbuf {
    QELEM   q;            /* 8  queue linkage (socket rx q, dev q, ...)      */
    PBUF   *chain;        /* 4  next PBUF of the same packet, or NULL        */
    UCHAR  *data;         /* 4  first valid byte in the data area            */
    USHORT  len;          /* 2  valid bytes at *data (this buffer)           */
    USHORT  size;         /* 2  forward capacity from data == (start+B)-data */
    USHORT  chainlen;     /* 2  total valid bytes of the chain (head only)   */
    UCHAR   class;        /* 1  NSFBUF_CLASS_SMALL / _LARGE                  */
    UCHAR   flags;        /* 1  reserved                                     */
    UINT    allocseq;     /* 4  monotonic alloc sequence (leak diagnosis)    */
    void   *dbg_owner;    /* 4  last owner (trace builds; 0 in prod)         */
};                        /* 32 bytes */
NSF_SIZE_ASSERT(PBUF, 32);

/* Initialization (init window only -- calls mm_pool_create, so it must run
 * between mm_init and mm_init_complete). Creates the BUFSMALL and BUFLARGE
 * pools at the hardcoded default counts above and remembers them for
 * class->pool selection. Added beyond the published spec 3.2 interface at
 * M0-3 (see the spec 3.2 note). */
void    buf_init(void);

/* Allocate an outbound buffer sized by hint_len (the intended payload). Picks
 * BUFSMALL when HEADROOM + hint_len <= NSFBUF_SMALL_DATA (hint_len <= 192),
 * else BUFLARGE. Opens data one headroom in, len 0. Returns NULL when the
 * chosen pool is exhausted -- normal and expected; the caller handles it.
 * The caller becomes the owner. */
PBUF   *buf_alloc(USHORT hint_len);

/* Free a buffer AND its whole chain (single owner, no refcount). Routes every
 * element through NSFMM so a double free / wrong-pool free is caught in debug.
 * Only the executive task calls this (spec 3.4). */
void    buf_free(PBUF *b);

/* Claim n bytes of headroom at the front: data moves back by n, len grows by
 * n (the caller writes the new header into the reclaimed bytes). Rejects when
 * fewer than n headroom bytes remain. Returns 0 on success, nonzero on
 * reject; the buffer is unchanged on reject. */
int     buf_prepend(PBUF *b, USHORT n);

/* Consume n bytes from the front (data advances, len shrinks). Bounds-checked
 * against len. Returns 0 on success, nonzero (buffer unchanged) if n > len. */
int     buf_trim_head(PBUF *b, USHORT n);

/* Drop n bytes from the tail (len shrinks; data and capacity unchanged).
 * Bounds-checked against len. Returns 0 on success, nonzero if n > len. */
int     buf_trim_tail(PBUF *b, USHORT n);

/* Copy up to n bytes from src into the buffer/chain, appended after the
 * current valid data (app->stack). Clamped at n and at the remaining capacity
 * of the chain. Returns the number of bytes actually copied; maintains the
 * head's chainlen. */
USHORT  buf_copyin (PBUF *b, const void *src, USHORT n);

/* Copy up to n bytes of valid data out of the buffer/chain into dst
 * (stack->app), starting at the logical front. Clamped at n and at the total
 * valid length. Returns the number of bytes actually copied; does not mutate
 * the chain. */
USHORT  buf_copyout(const PBUF *b, void *dst, USHORT n);

/* Append the `tail` chain to the end of the `head` chain and fold tail's valid
 * bytes into head's chainlen. Returns head (or tail if head is NULL). */
PBUF   *buf_chain_append(PBUF *head, PBUF *tail);

/* Total valid bytes across the chain (authoritative walk-sum of each len). */
USHORT  buf_chain_len(const PBUF *head);

/* Reset a buffer for inbound receive: data == start (no headroom), len 0,
 * size == B -- the whole data area is available for a received frame. The
 * inbound seam consumed by the driver at M1. */
void    buf_reset_rx(PBUF *b);

#if NSF_DEBUG
/* Diagnostic for the leak gate under NSF_DEBUG (host tests): the NSFMM pool
 * backing a size class, so a test can read its inuse/hiwater via mm_stats and
 * prove both pools return to baseline. Outside the production interface,
 * mirroring mm_debug_live_regions. The full MMPOOL type lives in nsfmm.h; the
 * forward declaration keeps pools an NSFBUF implementation detail for normal
 * consumers. */
struct mmpool;
struct mmpool *buf_debug_pool(UCHAR class);
#endif

#endif /* NSFBUF_H */
