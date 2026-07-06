/*
 * tstmm.c -- NSFMM host unit tests (spec ch. 02).
 *
 * Covers the whole pool contract: alloc to exhaustion (NULL + fails++), free
 * returning objects, the inuse/hiwater/allocs/frees accounting, the
 * enforcement ABENDs (post-seal mm_pool_create; double free; eyecatcher
 * corruption), and the region-level leak gate after mm_shutdown.
 *
 * The enforcement checks drive nsf_abend through a longjmp hook so the test
 * survives an intentional "this must never happen" and asserts it fired. The
 * eyecatcher/double-free checks live inside NSFMM's NSF_DEBUG path, so they are
 * compiled here only when NSF_DEBUG is on (make test-host sets -DNSF_DEBUG=1).
 */
#include "nsfmm.h"
#include "nsfabend.h"
#include <mbtcheck.h>
#include <setjmp.h>
#include <string.h>

#ifndef NSF_DEBUG
#define NSF_DEBUG 0
#endif

static jmp_buf g_jmp;
static UINT    g_abend_code;

static void catch_abend(UINT code)
{
    g_abend_code = code;
    longjmp(g_jmp, 1);              /* never returns to nsf_abend's caller */
}

int main(void)
{
    MMPOOL *pool;
    MMSTATS st;
    void   *objs[4];
    void   *p;
    int     i;

    printf("=== nsf370 NSFMM tests ===\n");

    /* ---- init + create a small pool ---- */
    CHECK_EQ(mm_init(NULL), 0, "mm_init returns 0");
    pool = mm_pool_create("TESTOBJ", 32, 4);   /* 4 objects, 32B payload */
    CHECK(pool != NULL, "mm_pool_create returns a pool");
    mm_init_complete();

    /* ---- alloc within capacity ---- */
    for (i = 0; i < 4; i++) {
        objs[i] = mm_alloc(pool);
        CHECK(objs[i] != NULL, "alloc within capacity is non-NULL");
    }
    mm_stats(pool, &st);
    CHECK_EQ(st.inuse, 4, "inuse == 4 at capacity");
    CHECK_EQ(st.hiwater, 4, "hiwater == 4");
    CHECK_EQ(st.allocs, 4, "allocs == 4");

    /* ---- exhaustion returns NULL and counts ---- */
    p = mm_alloc(pool);
    CHECK(p == NULL, "alloc past capacity returns NULL");
    mm_stats(pool, &st);
    CHECK_EQ(st.fails, 1, "fails incremented on exhaustion");
    CHECK_EQ(st.inuse, 4, "inuse unchanged by a failed alloc");

    /* ---- distinct objects do not overlap ---- */
    memset(objs[1], 0xAA, 32);
    memset(objs[2], 0x55, 32);
    CHECK(((UCHAR *)objs[1])[0] == 0xAA && ((UCHAR *)objs[2])[0] == 0x55,
          "distinct objects occupy distinct storage");

    /* ---- free returns an object; it is handed back out (LIFO) ---- */
    mm_free(pool, objs[0]);
    mm_stats(pool, &st);
    CHECK_EQ(st.inuse, 3, "inuse == 3 after one free");
    CHECK_EQ(st.frees, 1, "frees == 1");
    CHECK_EQ(st.hiwater, 4, "hiwater stays at the peak");
    p = mm_alloc(pool);
    CHECK(p == objs[0], "the freed object is the next one handed out (LIFO)");
    mm_stats(pool, &st);
    CHECK_EQ(st.inuse, 4, "inuse back to 4");
    CHECK_EQ(st.allocs, 5, "allocs == 5 cumulative");

    /* ---- free everything: pool returns to baseline ---- */
    mm_free(pool, objs[0]);            /* == p */
    mm_free(pool, objs[1]);
    mm_free(pool, objs[2]);
    mm_free(pool, objs[3]);
    mm_stats(pool, &st);
    CHECK_EQ(st.inuse, 0, "pool back to baseline (inuse == 0)");
    CHECK_EQ(st.frees, 5, "frees == 5 cumulative");

    /* ---- enforcement: mm_pool_create after the seal ABENDs ---- */
    {
        nsf_abend_fn prev = nsf_abend_sethook(catch_abend);
        g_abend_code = 0;
        if (setjmp(g_jmp) == 0) {
            (void)mm_pool_create("LATE", 16, 1);   /* sealed above: must abend */
            CHECK(0, "post-init mm_pool_create should not return");
        } else {
            CHECK_EQ((long)g_abend_code, 101, "post-init mm_pool_create fired the abend hook");
        }
        nsf_abend_sethook(prev);
    }

#if NSF_DEBUG
    /* ---- enforcement: a double free is caught (freed eyecatcher) ---- */
    {
        nsf_abend_fn prev;
        void *d = mm_alloc(pool);
        CHECK(d != NULL, "alloc an object to double-free");
        mm_free(pool, d);                          /* first free: fine */
        prev = nsf_abend_sethook(catch_abend);
        g_abend_code = 0;
        if (setjmp(g_jmp) == 0) {
            mm_free(pool, d);                      /* second free: must abend */
            CHECK(0, "double free should not return");
        } else {
            CHECK_EQ((long)g_abend_code, 102, "double free caught in debug");
        }
        nsf_abend_sethook(prev);
    }

    /* ---- enforcement: eyecatcher corruption is caught ---- */
    {
        nsf_abend_fn prev;
        void  *d = mm_alloc(pool);
        UCHAR *hdr;
        CHECK(d != NULL, "alloc an object to corrupt");
        hdr = (UCHAR *)d - sizeof(MMOBJ);           /* reach back to eye[0] */
        hdr[0] = 0x00;                              /* smash the eyecatcher */
        prev = nsf_abend_sethook(catch_abend);
        g_abend_code = 0;
        if (setjmp(g_jmp) == 0) {
            mm_free(pool, d);                       /* corrupted: must abend */
            CHECK(0, "free of a corrupted object should not return");
        } else {
            CHECK_EQ((long)g_abend_code, 102, "eyecatcher corruption caught in debug");
        }
        nsf_abend_sethook(prev);
        /* d is deliberately left allocated; mm_shutdown releases the region. */
    }
#endif /* NSF_DEBUG */

    /* ---- shutdown leaks no region ---- */
    mm_shutdown();
#if NSF_DEBUG
    CHECK_EQ((long)mm_debug_live_regions(), 0, "mm_shutdown released every region");
#endif

    return mbt_test_summary("TSTMM");
}
