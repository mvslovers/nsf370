/*
 * nsfbuf.c -- the Buffer Manager (see nsfbuf.h, spec ch. 03).
 *
 * PBUFs are carved from two NSFMM pools; NSFBUF never calls a storage service
 * itself (goal 2). The PBUF header sits at the very start of the NSFMM object
 * payload, immediately followed by the B-byte data area:
 *
 *   pbuf ----> [ PBUF header (sizeof(PBUF)) ][ data area B bytes ]
 *   start = (UCHAR*)pbuf + sizeof(PBUF)
 *   end   = start + B          (B = NSFBUF_SMALL_DATA or NSFBUF_LARGE_DATA)
 *
 * `size` is maintained as the forward capacity from `data`, i.e.
 * size == end - data, so `len <= size` is exactly the layout invariant
 * `start <= data && data + len <= end`. Every mutator keeps that identity;
 * under NSF_DEBUG the identity and the head's chainlen cache are re-checked
 * after each operation (buf_assert_bounds / buf_assert_chainlen) so a pointer-
 * math slip trips the abend in a host test instead of corrupting a packet.
 *
 * The MM object payload size is sizeof(PBUF) + B, computed at run time -- NOT
 * the target constant 288/2080 -- because sizeof(PBUF) is 32 only on the
 * S/370 target (4-byte pointers) and larger on a host test build (8-byte
 * pointers). Using sizeof(PBUF) for BOTH the pool object size and `start`
 * keeps the two self-consistent on either platform.
 */
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsfabend.h"

#include <stddef.h>            /* ptrdiff_t */
#include <string.h>           /* memcpy */

#ifndef NSF_DEBUG
#define NSF_DEBUG 0
#endif

/* User completion code routed through nsf_abend (spec range 100-199 covers
 * memory/buffers; NSFMM already uses 101-103). Debug-only: a violated PBUF
 * layout or chainlen invariant means NSFBUF itself has a bug. */
#define NSF_AB_BUF_INVARIANT 110

/* The two pools, remembered by buf_init for class->pool selection on free. */
static MMPOOL *g_pool_small;
static MMPOOL *g_pool_large;

/* Monotonic allocation sequence stamped into PBUF.allocseq for leak diagnosis
 * (which buffer, in what order). Reset by buf_init so a run is reproducible. */
static UINT    g_allocseq;

/* --- small mappings ------------------------------------------------------- */

static USHORT buf_data_area(UCHAR class)
{
    return (class == NSFBUF_CLASS_SMALL) ? (USHORT)NSFBUF_SMALL_DATA
                                         : (USHORT)NSFBUF_LARGE_DATA;
}

static MMPOOL *buf_pool_for(UCHAR class)
{
    return (class == NSFBUF_CLASS_SMALL) ? g_pool_small : g_pool_large;
}

/* --- debug invariant checks ----------------------------------------------- */

#if NSF_DEBUG
static void buf_assert_bounds(const PBUF *b)
{
    const UCHAR *start = (const UCHAR *)b + sizeof(PBUF);
    const UCHAR *end   = start + buf_data_area(b->class);

    if (!(start <= b->data
          && b->data + b->len <= end
          && b->size == (USHORT)(end - b->data))) {
        nsf_abend(NSF_AB_BUF_INVARIANT);   /* NSFBUF layout math is wrong */
    }
}

/* Head-level check: the cached chainlen must equal the walk-sum of the chain.
 * Call once per head-mutating op (never inside a per-element loop). */
static void buf_assert_chainlen(const PBUF *head)
{
    if (head != NULL && head->chainlen != buf_chain_len(head)) {
        nsf_abend(NSF_AB_BUF_INVARIANT);   /* chainlen cache drifted */
    }
}

#define BUF_ASSERT_BOUNDS(b)   buf_assert_bounds(b)
#define BUF_ASSERT_CHAIN(h)    buf_assert_chainlen(h)
#else
#define BUF_ASSERT_BOUNDS(b)   ((void)0)
#define BUF_ASSERT_CHAIN(h)    ((void)0)
#endif

/* --- public interface ----------------------------------------------------- */

int buf_init(void)
{
    /* Init window only: mm_pool_create is legal solely between mm_init and
     * mm_init_complete (it ABENDs afterwards -- nsfmm.h). The MM object size is
     * sizeof(PBUF) + B (see the file header); on the target that is 288 / 2080,
     * matching the spec 2.5 budget. */
    g_pool_small = mm_pool_create("BUFSMALL",
                                  (USHORT)(sizeof(PBUF) + NSFBUF_SMALL_DATA),
                                  NSFBUF_SMALL_COUNT);
    g_pool_large = mm_pool_create("BUFLARGE",
                                  (USHORT)(sizeof(PBUF) + NSFBUF_LARGE_DATA),
                                  NSFBUF_LARGE_COUNT);
    g_allocseq = 0;

    /* Report a pool-creation failure so the executive startup (M0-8) can refuse
     * to run rather than limp along with NULL pools. No WTO here -- operator
     * reporting belongs to that startup path. */
    if (g_pool_small == NULL || g_pool_large == NULL) {
        return -1;
    }
    return 0;
}

PBUF *buf_alloc(USHORT hint_len)
{
    MMPOOL *pool;
    PBUF   *b;
    UCHAR  *start;
    UCHAR   class;
    USHORT  bdata;

    /* Class selection: BUFSMALL if the payload plus default headroom fits the
     * small data area (HEADROOM 64 + hint_len <= 256, i.e. hint_len <= 192).
     * All-int arithmetic -- no USHORT overflow on the 64 + hint_len sum. */
    if (NSFBUF_HEADROOM + hint_len <= NSFBUF_SMALL_DATA) {
        pool  = g_pool_small;
        class = NSFBUF_CLASS_SMALL;
        bdata = (USHORT)NSFBUF_SMALL_DATA;
    } else {
        pool  = g_pool_large;
        class = NSFBUF_CLASS_LARGE;
        bdata = (USHORT)NSFBUF_LARGE_DATA;
    }

    if (pool == NULL) {
        return NULL;                    /* not initialized: treat as exhausted */
    }
    b = (PBUF *)mm_alloc(pool);
    if (b == NULL) {
        return NULL;                    /* pool exhausted: normal, caller handles */
    }

    start        = (UCHAR *)b + sizeof(PBUF);
    b->chain     = NULL;
    b->data      = start + NSFBUF_HEADROOM;         /* open one headroom in */
    b->len       = 0;
    b->size      = (USHORT)(bdata - NSFBUF_HEADROOM);
    b->chainlen  = 0;
    b->class     = class;
    b->flags     = 0;
    b->allocseq  = ++g_allocseq;
    b->dbg_owner = NULL;
    b->q.next    = NULL;                /* unlinked until enqueued */
    b->q.prev    = NULL;

    BUF_ASSERT_BOUNDS(b);
    return b;
}

void buf_free(PBUF *b)
{
    while (b != NULL) {
        PBUF *next = b->chain;          /* save the link before the free */
        mm_free(buf_pool_for(b->class), b);
        b = next;
    }
}

int buf_prepend(PBUF *b, USHORT n)
{
    UCHAR *start = (UCHAR *)b + sizeof(PBUF);

    /* Headroom available is data - start (a signed pointer diff); compare it
     * against n without casting the diff to unsigned (keeps -Wsign-compare
     * quiet). */
    if (b->data - start < (ptrdiff_t)n) {
        return -1;                      /* not enough headroom: reject */
    }
    b->data     -= n;
    b->len       = (USHORT)(b->len + n);
    b->size      = (USHORT)(b->size + n);
    b->chainlen  = (USHORT)(b->chainlen + n);

    BUF_ASSERT_BOUNDS(b);
    BUF_ASSERT_CHAIN(b);
    return 0;
}

int buf_trim_head(PBUF *b, USHORT n)
{
    if (n > b->len) {
        return -1;                      /* would consume past the valid data */
    }
    b->data     += n;
    b->len       = (USHORT)(b->len - n);
    b->size      = (USHORT)(b->size - n);
    b->chainlen  = (USHORT)(b->chainlen - n);

    BUF_ASSERT_BOUNDS(b);
    BUF_ASSERT_CHAIN(b);
    return 0;
}

int buf_trim_tail(PBUF *b, USHORT n)
{
    PBUF *last = b;

    /* The logical tail of a packet is the LAST buffer of the chain; trimming
     * the head element would drop bytes from the MIDDLE of the packet. Walk to
     * it and bounds-check n against ITS len (b == last for a single buffer, so
     * that case is unchanged). */
    while (last->chain != NULL) {
        last = last->chain;
    }
    if (n > last->len) {
        return -1;                      /* nothing that far back to drop */
    }
    last->len   = (USHORT)(last->len - n);      /* size unchanged: data fixed */
    b->chainlen = (USHORT)(b->chainlen - n);    /* fold into the head's total  */

    BUF_ASSERT_BOUNDS(last);
    BUF_ASSERT_CHAIN(b);
    return 0;
}

USHORT buf_copyin(PBUF *b, const void *src, USHORT n)
{
    const UCHAR *s     = (const UCHAR *)src;
    PBUF        *head  = b;
    USHORT       total = 0;

    /* Append across the chain: fill the tail room (size - len) of each buffer
     * in turn, clamped so the whole copy stops at n and at capacity. */
    while (b != NULL && total < n) {
        USHORT room = (USHORT)(b->size - b->len);
        USHORT want = (USHORT)(n - total);
        USHORT take = (want < room) ? want : room;

        if (take > 0) {
            memcpy(b->data + b->len, s + total, take);
            b->len = (USHORT)(b->len + take);
            total  = (USHORT)(total + take);
            BUF_ASSERT_BOUNDS(b);
        }
        b = b->chain;
    }
    head->chainlen = (USHORT)(head->chainlen + total);

    BUF_ASSERT_CHAIN(head);
    return total;
}

USHORT buf_copyout(const PBUF *b, void *dst, USHORT n)
{
    UCHAR *d     = (UCHAR *)dst;
    USHORT total = 0;

    /* Read from the logical front across the chain, clamped at n and at the
     * valid length of each buffer. */
    while (b != NULL && total < n) {
        USHORT want = (USHORT)(n - total);
        USHORT take = (want < b->len) ? want : b->len;

        if (take > 0) {
            memcpy(d + total, b->data, take);
            total = (USHORT)(total + take);
        }
        b = b->chain;
    }
    return total;
}

PBUF *buf_chain_append(PBUF *head, PBUF *tail)
{
    PBUF *p;

    if (head == NULL) {
        return tail;
    }
    if (tail != NULL) {
        for (p = head; p->chain != NULL; p = p->chain) {
            /* walk to the last element */
        }
        p->chain = tail;
        head->chainlen = (USHORT)(head->chainlen + buf_chain_len(tail));
        BUF_ASSERT_CHAIN(head);
    }
    return head;
}

USHORT buf_chain_len(const PBUF *head)
{
    UINT total = 0;

    /* Sum in a wide accumulator; a chain's valid bytes cannot exceed the IP
     * datagram limit (64 KB), so the USHORT return never truncates in practice. */
    while (head != NULL) {
        total += head->len;
        head   = head->chain;
    }
    return (USHORT)total;
}

void buf_reset_rx(PBUF *b)
{
    UCHAR *start = (UCHAR *)b + sizeof(PBUF);

    b->data     = start;                /* no headroom on the inbound path */
    b->len      = 0;
    b->size     = buf_data_area(b->class);
    b->chainlen = 0;

    BUF_ASSERT_BOUNDS(b);
}

#if NSF_DEBUG
MMPOOL *buf_debug_pool(UCHAR class)
{
    return buf_pool_for(class);
}
#endif
