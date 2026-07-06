/*
 * tstbuf.c -- NSFBUF host unit tests (spec ch. 03).
 *
 * White-box tests of the buffer manager: they know the pooled-object layout
 * (PBUF header immediately followed by the B-byte data area) so they can check
 * data/size against `start = (UCHAR*)b + sizeof(PBUF)`. Portable C: builds and
 * runs natively via `make test-host` (the CI gate) with NSF_DEBUG=1, so the
 * NSFBUF/NSFMM internal invariant abends are armed while the test runs.
 *
 * Covers: class selection at the 192/193 boundary and field initialization;
 * headroom prepend + over-prepend reject; head/tail trim + over-trim reject;
 * single-buffer copyin/copyout with clamping at n and at capacity/valid length;
 * chaining (append links, chain_len sums, copyout spans a chain); the inbound
 * reset seam; and the two-pool leak gate -- buf_free of a MIXED (small+large)
 * chain returns BOTH pools to inuse==0, and every region is released at
 * shutdown.
 */
#include "nsfbuf.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <string.h>

#ifndef NSF_DEBUG
#define NSF_DEBUG 0
#endif

/* Recover the data-area base the same way NSFBUF does internally. */
static UCHAR *buf_start(PBUF *b)
{
    return (UCHAR *)b + sizeof(PBUF);
}

int main(void)
{
    PBUF   *b, *small, *large;
    UCHAR   src[400];
    UCHAR   dst[400];
    UCHAR  *start, *d0;
    USHORT  got;
    int     i;

    printf("=== nsf370 NSFBUF tests ===\n");

    for (i = 0; i < (int)sizeof(src); i++) {
        src[i] = (UCHAR)(i & 0xFF);
    }

    /* ---- init: pools live inside the mm init window ---- */
    CHECK_EQ(mm_init(NULL), 0, "mm_init returns 0");
    buf_init();                                 /* creates BUFSMALL + BUFLARGE */
    mm_init_complete();

    /* ---- field initialization (first alloc: allocseq deterministic) ---- */
    b = buf_alloc(100);                         /* 64 + 100 <= 256 -> small */
    CHECK(b != NULL, "buf_alloc returns a buffer");
    start = buf_start(b);
    CHECK_EQ(b->class, NSFBUF_CLASS_SMALL, "hint 100 selects BUFSMALL");
    CHECK(b->data == start + NSFBUF_HEADROOM, "data opens one headroom in");
    CHECK_EQ(b->len, 0, "len starts at 0");
    CHECK_EQ(b->size, NSFBUF_SMALL_DATA - NSFBUF_HEADROOM, "size == B - headroom (192)");
    CHECK_EQ(b->chainlen, 0, "chainlen starts at 0");
    CHECK(b->chain == NULL, "chain starts NULL");
    CHECK_EQ(b->flags, 0, "flags start 0");
    CHECK_EQ((long)b->allocseq, 1, "first alloc gets allocseq 1");
    CHECK(b->q.next == NULL && b->q.prev == NULL, "q stays unlinked at alloc");
    buf_free(b);

    /* ---- class selection at the 192 / 193 boundary ---- */
    small = buf_alloc(192);                     /* 64 + 192 == 256 -> small */
    large = buf_alloc(193);                     /* 64 + 193 == 257 -> large */
    CHECK_EQ(small->class, NSFBUF_CLASS_SMALL, "hint 192 is the last small");
    CHECK_EQ(large->class, NSFBUF_CLASS_LARGE, "hint 193 crosses to large");
    CHECK_EQ(large->size, NSFBUF_LARGE_DATA - NSFBUF_HEADROOM, "large size == 2048 - 64");
    CHECK((long)large->allocseq > (long)small->allocseq, "allocseq is monotonic");
    buf_free(small);
    buf_free(large);

    /* ---- prepend within headroom, then reject over-prepend ---- */
    b  = buf_alloc(0);                           /* small, full 64B headroom */
    d0 = b->data;
    CHECK_EQ(buf_prepend(b, 20), 0, "prepend 20 within headroom accepted");
    CHECK(b->data == d0 - 20, "prepend moves data back by n");
    CHECK_EQ(b->len, 20, "prepend grows len by n");
    CHECK_EQ(b->size, (NSFBUF_SMALL_DATA - NSFBUF_HEADROOM) + 20, "prepend grows size by n");
    CHECK_EQ(b->chainlen, 20, "prepend folds into chainlen");
    /* 44 headroom bytes remain; asking for 100 must be rejected untouched */
    CHECK(buf_prepend(b, 100) != 0, "over-prepend rejected");
    CHECK(b->data == d0 - 20, "rejected prepend leaves data unchanged");
    CHECK_EQ(b->len, 20, "rejected prepend leaves len unchanged");
    /* claim exactly the rest of the headroom, then one byte over */
    CHECK_EQ(buf_prepend(b, 44), 0, "prepend the last 44 headroom bytes");
    CHECK(b->data == buf_start(b), "data now sits at start (headroom empty)");
    CHECK(buf_prepend(b, 1) != 0, "prepend past an empty headroom rejected");
    buf_free(b);

    /* ---- trim head/tail within bounds, then reject over-trim ---- */
    b   = buf_alloc(200);                        /* large (64+200 > 256) */
    got = buf_copyin(b, src, 100);
    CHECK_EQ(got, 100, "copyin 100 bytes");
    CHECK_EQ(b->len, 100, "len == 100 after copyin");
    d0 = b->data;
    CHECK_EQ(buf_trim_head(b, 30), 0, "trim_head 30 accepted");
    CHECK(b->data == d0 + 30, "trim_head advances data by n");
    CHECK_EQ(b->len, 70, "trim_head shrinks len by n");
    CHECK_EQ(buf_trim_tail(b, 20), 0, "trim_tail 20 accepted");
    CHECK_EQ(b->len, 50, "trim_tail shrinks len by n");
    CHECK(buf_trim_head(b, 1000) != 0, "over-trim_head rejected");
    CHECK(buf_trim_tail(b, 1000) != 0, "over-trim_tail rejected");
    CHECK_EQ(b->len, 50, "rejected trims leave len unchanged");
    /* trim_head consumed 30 from the front: the remaining data starts at src[30] */
    got = buf_copyout(b, dst, 400);
    CHECK_EQ(got, 50, "copyout returns the surviving 50 bytes");
    CHECK(memcmp(dst, src + 30, 50) == 0, "surviving bytes are src[30..79]");
    buf_free(b);

    /* ---- copyin/copyout clamping on a single buffer ---- */
    b   = buf_alloc(0);                          /* small: capacity 192 */
    got = buf_copyin(b, src, 50);
    CHECK_EQ(got, 50, "copyin clamps at n (50)");
    got = buf_copyout(b, dst, 50);
    CHECK_EQ(got, 50, "copyout returns 50");
    CHECK(memcmp(dst, src, 50) == 0, "copyout bytes match copyin");
    got = buf_copyout(b, dst, 100);
    CHECK_EQ(got, 50, "copyout clamps at valid length (50)");
    buf_free(b);

    b   = buf_alloc(0);                          /* small: capacity 192 */
    got = buf_copyin(b, src, 400);
    CHECK_EQ(got, NSFBUF_SMALL_DATA - NSFBUF_HEADROOM, "copyin clamps at capacity (192)");
    CHECK_EQ(b->len, NSFBUF_SMALL_DATA - NSFBUF_HEADROOM, "len saturates at capacity");
    buf_free(b);

    /* ---- chain-aware copyin: a payload that overflows the head spills into
     * the tail; copyout reads it back contiguously (the multi-buffer copyin
     * path, exercised here for the first time) ---- */
    small = buf_alloc(0);                        /* small head: capacity 192 */
    large = buf_alloc(500);                      /* large tail: capacity 1984 */
    buf_chain_append(small, large);              /* two empty buffers chained */
    got = buf_copyin(small, src, 250);           /* 192 -> head, 58 -> tail */
    CHECK_EQ(got, 250, "chained copyin spans both buffers (250)");
    CHECK_EQ(small->len, NSFBUF_SMALL_DATA - NSFBUF_HEADROOM, "head filled to capacity (192)");
    CHECK_EQ(large->len, 250 - (NSFBUF_SMALL_DATA - NSFBUF_HEADROOM), "tail took the remainder (58)");
    CHECK_EQ(small->chainlen, 250, "head chainlen == spanning copyin total");
    CHECK_EQ(buf_chain_len(small), 250, "walk-sum agrees with the cached chainlen");
    got = buf_copyout(small, dst, 250);
    CHECK_EQ(got, 250, "chained copyout returns 250");
    CHECK(memcmp(dst, src, 250) == 0, "spanning copyin/copyout round-trips the bytes");
    buf_free(small);                             /* frees both via the chain */

    /* ---- reset_rx: the inbound seam yields data==start, size==B ---- */
    b = buf_alloc(0);                            /* small */
    buf_reset_rx(b);
    CHECK(b->data == buf_start(b), "reset_rx puts data at start (no headroom)");
    CHECK_EQ(b->len, 0, "reset_rx clears len");
    CHECK_EQ(b->size, NSFBUF_SMALL_DATA, "reset_rx opens the full data area (size == B)");
    buf_free(b);

    /* ---- chaining: append links, chain_len sums, copyout spans the chain ---- */
    small = buf_alloc(0);                        /* small */
    large = buf_alloc(500);                      /* large */
    CHECK_EQ(small->class, NSFBUF_CLASS_SMALL, "chain head is small");
    CHECK_EQ(large->class, NSFBUF_CLASS_LARGE, "chain tail is large");
    buf_copyin(small, src, 10);                  /* head: src[0..9]   */
    buf_copyin(large, src + 10, 20);             /* tail: src[10..29] */
    CHECK(buf_chain_append(small, large) == small, "append returns the head");
    CHECK(small->chain == large, "append links tail onto head");
    CHECK_EQ(buf_chain_len(small), 30, "chain_len sums both buffers (30)");
    CHECK_EQ(small->chainlen, 30, "cached chainlen equals the walk-sum");
    got = buf_copyout(small, dst, 100);
    CHECK_EQ(got, 30, "copyout spans the chain (30 bytes, clamped at valid)");
    CHECK(memcmp(dst, src, 30) == 0, "chained copyout yields src[0..29] contiguously");

    /* ---- leak gate: one buf_free of the MIXED chain drains BOTH pools ---- *
     * At this point the mixed chain is the only outstanding allocation (every
     * earlier buffer was freed), so each pool holds exactly its one chain
     * element. A single buf_free(head) must return both to inuse==0, which
     * exercises the per-element class->pool routing in buf_free. The pool
     * inspection uses NSF_DEBUG-only accessors (as in tstmm.c), so it is
     * compiled only in the host build; the free itself always runs. */
#if NSF_DEBUG
    {
        MMPOOL *pool_s = buf_debug_pool(NSFBUF_CLASS_SMALL);
        MMPOOL *pool_l = buf_debug_pool(NSFBUF_CLASS_LARGE);
        MMSTATS st;

        mm_stats(pool_s, &st);
        CHECK_EQ(st.inuse, 1, "BUFSMALL holds the one chain head before free");
        mm_stats(pool_l, &st);
        CHECK_EQ(st.inuse, 1, "BUFLARGE holds the one chain tail before free");

        buf_free(small);                         /* frees small AND large via the chain */

        mm_stats(pool_s, &st);
        CHECK_EQ(st.inuse, 0, "BUFSMALL back to baseline after chain free");
        mm_stats(pool_l, &st);
        CHECK_EQ(st.inuse, 0, "BUFLARGE back to baseline after chain free");
    }
#else
    buf_free(small);                             /* frees small AND large via the chain */
#endif

    mm_shutdown();
#if NSF_DEBUG
    CHECK_EQ((long)mm_debug_live_regions(), 0, "shutdown released every region");
#endif

    return mbt_test_summary("TSTBUF");
}
