#ifndef NSFMM_H
#define NSFMM_H
/*
 * nsfmm.h -- the Memory Manager (spec ch. 02).
 *
 * NSFMM owns ALL dynamic storage of the stack. It acquires one contiguous
 * region per pool at initialization and never again; protocol code never
 * calls a storage service directly. Each pool hands out fixed-size objects
 * with O(1) alloc/free and accounts for every one (in-use, high-water,
 * cumulative allocs/frees, exhaustion failures) so leaks show up as a simple
 * differential.
 *
 * Lifecycle (enforced -- see nsfabend.h):
 *   mm_init()            open the init window (reset the pool table)
 *   mm_pool_create() *   create pools -- legal only inside the init window
 *   mm_init_complete()   seal: any later mm_pool_create ABENDs
 *   mm_alloc()/mm_free() runtime object churn; mm_alloc NULL = pool exhausted
 *   mm_shutdown()        release every region
 *
 * mm_alloc() returning NULL is normal and expected -- every caller handles it
 * (drop + count, reject, fail with ENOBUFS). Exhaustion is never an ABEND.
 */

#include "nsf.h"

/* Per-object header, prefixed on every pooled object (spec 2.3). 8 bytes on
 * the S/370 target: 4 bytes of metadata + a 4-byte free-list link. */
typedef struct mmobj {
    UCHAR         eye[2];       /* 0xC5D6 'EO' allocated / 0xC6D9 'FR' free   */
    UCHAR         poolid;       /* owning pool index (double-free/wrong-pool) */
    UCHAR         flags;        /* reserved                                   */
    struct mmobj *next;         /* free-list link -- valid only while free    */
} MMOBJ;
NSF_SIZE_ASSERT(MMOBJ, 8);

/* Pool descriptor (spec 2.3). 40 bytes on the S/370 target. */
typedef struct mmpool {
    char    name[8];            /* eyecatcher, blank-padded, e.g. "TCPTCB "  */
    MMOBJ  *freelist;           /* LIFO free-list head                       */
    UCHAR  *region;             /* base of the one acquired region           */
    USHORT  objsize;            /* payload bytes per object (excl. MMOBJ)    */
    USHORT  count;              /* total objects in the pool                 */
    USHORT  inuse;              /* currently allocated                       */
    USHORT  hiwater;            /* peak inuse                                */
    UINT    allocs;             /* cumulative successful allocations         */
    UINT    frees;              /* cumulative frees                          */
    UINT    fails;              /* cumulative exhaustion events (NULL rets)  */
    UCHAR   poolid;             /* this pool's index                         */
    UCHAR   rsvd[3];
} MMPOOL;
NSF_SIZE_ASSERT(MMPOOL, 40);

/* Snapshot returned by mm_stats (DISPLAY MEMORY / NSFSTS). */
typedef struct mmstats {
    char    name[8];
    USHORT  objsize;
    USHORT  count;
    USHORT  inuse;
    USHORT  hiwater;
    UINT    allocs;
    UINT    frees;
    UINT    fails;
} MMSTATS;

/* Init configuration (spec 2.2). Reserved for now; pass NULL for defaults. */
typedef struct mmcfg {
    USHORT  flags;
    USHORT  rsvd;
} MMCFG;

int      mm_init(const MMCFG *cfg);                          /* init-time only */
MMPOOL  *mm_pool_create(const char *name8,                   /* init-time only */
                        USHORT objsize, USHORT count);
void     mm_init_complete(void);                             /* seal the pools */
void    *mm_alloc(MMPOOL *pool);                             /* NULL = exhausted */
void     mm_free(MMPOOL *pool, void *obj);
void     mm_stats(const MMPOOL *pool, MMSTATS *out);
void     mm_shutdown(void);                                  /* release regions */

#if NSF_DEBUG
/* Diagnostic for the leak gate under NSF_DEBUG (host tests): the count of
 * regions currently held, which returns to 0 after mm_shutdown. Intentionally
 * outside the production interface. */
UINT     mm_debug_live_regions(void);
#endif

#endif /* NSFMM_H */
