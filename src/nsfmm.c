/*
 * nsfmm.c -- the Memory Manager (see nsfmm.h, spec ch. 02).
 *
 * Fixed-size object pools carved from one region each. The pool table itself
 * is a fixed static array, not allocated -- there is genuinely no runtime
 * allocation after init. Free lists are LIFO (hot objects stay resident).
 *
 * Region seam (spec 2.3, ADR-0015): storage comes from libc370 malloc, which
 * on 24-bit MVS 3.8j resolves to GETMAIN below the 16 MB line and is released
 * by free() at mm_shutdown. Keeping NSFMM pure portable C -- no assembler
 * GETMAIN helper -- is deliberate: the acquisition primitive is the ONLY place
 * a storage service is touched, and it is init-time only, never on a hot path.
 */
#include "nsfmm.h"
#include "nsfabend.h"

#include <stdlib.h>            /* malloc, free */
#include <string.h>           /* memset, memcpy */

#ifndef NSF_DEBUG
#define NSF_DEBUG 0
#endif

#define NSF_MM_MAXPOOLS 16    /* spec 2.5 lists 8 default pools; headroom to 16 */

/* Object eyecatcher bytes (spec 2.3). Deliberately EBCDIC-readable in an MVS
 * dump: 0xC5D6 = 'EO' (allocated), 0xC6D9 = 'FR' (free). Kept as raw byte
 * values, not char literals, so the read-back compare against the unsigned
 * eye[] field is signedness-safe and reproduces the exact on-target bytes. */
#define MM_EYE_ALLOC0 0xC5
#define MM_EYE_ALLOC1 0xD6
#define MM_EYE_FREE0  0xC6
#define MM_EYE_FREE1  0xD9

/* User completion codes routed through nsf_abend (spec 2.2; memory range
 * 100-199). On MVS these surface as ABEND U0101/U0102/U0103. */
#define NSF_AB_MM_SEALED    101   /* mm_pool_create after mm_init_complete   */
#define NSF_AB_MM_BADOBJ    102   /* corrupted / doubly-freed object on free */
#define NSF_AB_MM_WRONGPOOL 103   /* object freed to the wrong pool          */

#define MM_ALIGN 8u               /* S/370 doubleword; keeps objects aligned */

static MMPOOL g_pools[NSF_MM_MAXPOOLS];
static UCHAR  g_npools;            /* pools created so far                    */
static UCHAR  g_sealed;            /* mm_init_complete called: no more pools  */
static UINT   g_live_regions;      /* regions currently held (leak gate)      */

/* --- the storage seam (init-time only) ------------------------------------ */

static void *mm_region_get(unsigned nbytes)
{
    void *p = malloc(nbytes);
    if (p != NULL) {
        g_live_regions++;
    }
    return p;
}

static void mm_region_free(void *region)
{
    if (region != NULL) {
        free(region);
        g_live_regions--;
    }
}

static unsigned mm_roundup8(unsigned n)
{
    return (n + (MM_ALIGN - 1u)) & ~(MM_ALIGN - 1u);
}

/* --- public interface ----------------------------------------------------- */

int mm_init(const MMCFG *cfg)
{
    (void)cfg;                          /* reserved (spec 2.2); defaults now */
    memset(g_pools, 0, sizeof(g_pools));
    g_npools = 0;
    g_sealed = 0;
    /* g_live_regions is intentionally NOT reset: it tracks storage the OS
     * still holds. mm_shutdown drives it to 0; a fresh mm_init must not mask
     * an earlier leak. */
    return 0;
}

MMPOOL *mm_pool_create(const char *name8, USHORT objsize, USHORT count)
{
    MMPOOL  *p;
    UCHAR   *base;
    unsigned payload, stride, i, n;

    if (g_sealed) {
        nsf_abend(NSF_AB_MM_SEALED);    /* enforced: no pools after seal */
        return NULL;                    /* unreachable once abend is real */
    }
    if (g_npools >= NSF_MM_MAXPOOLS || count == 0) {
        return NULL;                    /* config error; init code reports it */
    }

    payload = mm_roundup8(objsize);
    stride  = (unsigned)sizeof(MMOBJ) + payload;   /* 8 on MVS, 16 on host */
    n       = count;

    base = (UCHAR *)mm_region_get(stride * n);
    if (base == NULL) {
        return NULL;                    /* region acquisition failed at init */
    }

    p = &g_pools[g_npools];
    memset(p, 0, sizeof(*p));
    memset(p->name, ' ', sizeof(p->name));          /* blank-padded eyecatcher */
    for (i = 0; i < sizeof(p->name) && name8 != NULL && name8[i] != '\0'; i++) {
        p->name[i] = name8[i];
    }
    p->region  = base;
    p->objsize = (USHORT)payload;
    p->count   = count;
    p->poolid  = g_npools;
    p->freelist = NULL;

    /* Carve the region into `count` objects, each threaded onto the free
     * list. Objects start life stamped FREE so a dump reads cleanly even in a
     * non-debug build. */
    for (i = 0; i < n; i++) {
        MMOBJ *o = (MMOBJ *)(void *)(base + i * stride);
        o->eye[0] = MM_EYE_FREE0;
        o->eye[1] = MM_EYE_FREE1;
        o->poolid = g_npools;
        o->flags  = 0;
        o->next   = p->freelist;
        p->freelist = o;
    }

    g_npools++;
    return p;
}

void mm_init_complete(void)
{
    g_sealed = 1;
}

void *mm_alloc(MMPOOL *pool)
{
    MMOBJ *o = pool->freelist;

    if (o == NULL) {
        pool->fails++;                  /* exhaustion: normal, caller handles */
        return NULL;
    }
    pool->freelist = o->next;
    o->next = NULL;
#if NSF_DEBUG
    o->eye[0] = MM_EYE_ALLOC0;
    o->eye[1] = MM_EYE_ALLOC1;
#endif
    pool->inuse++;
    if (pool->inuse > pool->hiwater) {
        pool->hiwater = pool->inuse;
    }
    pool->allocs++;
    return (void *)((UCHAR *)o + sizeof(MMOBJ));
}

void mm_free(MMPOOL *pool, void *obj)
{
    MMOBJ *o = (MMOBJ *)(void *)((UCHAR *)obj - sizeof(MMOBJ));

#if NSF_DEBUG
    if (o->eye[0] != MM_EYE_ALLOC0 || o->eye[1] != MM_EYE_ALLOC1) {
        /* Not a live object: corruption, or a double free (a freed object
         * carries the FREE eyecatcher). */
        nsf_abend(NSF_AB_MM_BADOBJ);
        return;                         /* defence in depth if a hook returns */
    }
    if (o->poolid != pool->poolid) {
        nsf_abend(NSF_AB_MM_WRONGPOOL);
        return;
    }
    o->eye[0] = MM_EYE_FREE0;
    o->eye[1] = MM_EYE_FREE1;
#endif
    o->next = pool->freelist;
    pool->freelist = o;
    pool->inuse--;
    pool->frees++;
}

void mm_stats(const MMPOOL *pool, MMSTATS *out)
{
    memcpy(out->name, pool->name, sizeof(out->name));
    out->objsize = pool->objsize;
    out->count   = pool->count;
    out->inuse   = pool->inuse;
    out->hiwater = pool->hiwater;
    out->allocs  = pool->allocs;
    out->frees   = pool->frees;
    out->fails   = pool->fails;
}

void mm_shutdown(void)
{
    unsigned i, n = g_npools;

    for (i = 0; i < n; i++) {
        mm_region_free(g_pools[i].region);
        g_pools[i].region = NULL;
    }
    g_npools = 0;
    g_sealed = 0;
}

#if NSF_DEBUG
UINT mm_debug_live_regions(void)
{
    return g_live_regions;
}
#endif
