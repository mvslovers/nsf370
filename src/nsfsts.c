/*
 * nsfsts.c -- the Statistics Manager (see nsfsts.h, spec ch. 08).
 *
 * The registry is one file-scope object -- a header (eyecatcher, count, max)
 * followed by a fixed array of records. It is BSS, never NSFMM storage (goal 2
 * does not apply: counters exist before pools and must survive into a dump).
 *
 * Each record wraps the 16-byte STSCTR with its owning component so STSCTR
 * itself stays 16 bytes and pointer-free while the render can still group by
 * component. The array never moves and registration only appends, so a STSCTR*
 * handed back by sts_register is stable for the whole run -- a component caches
 * it once and STS_INC()s it on the hot path with no lookup.
 */
#include "nsfsts.h"

#include <stdio.h>            /* snprintf */
#include <string.h>         /* memset, memcpy */

/* One registry record: the component label plus its counter. */
typedef struct stsrec {
    char   component[8];        /* grouping label, NUL-padded */
    STSCTR ctr;                 /* the 16-byte counter (name + value) */
} STSREC;

static struct {
    char   eye[8];             /* "NSFSTATS" (not NUL-terminated) */
    UINT   count;             /* counters registered so far       */
    UINT   max;               /* NSFSTS_MAX (copy for the dump)   */
    STSREC rec[NSFSTS_MAX];
} g_sts;

/* Copy up to dstsize bytes of a NUL-terminated field, leaving the remainder as
 * the caller left it (zeroed). Fields need no NUL of their own -- sts_render
 * bounds every read with a printf precision -- so a name that exactly fills its
 * field is stored whole. Mirrors mm_pool_create's name-copy discipline. */
static void sts_copyfield(char *dst, unsigned dstsize, const char *src)
{
    unsigned i;

    for (i = 0; i < dstsize && src != NULL && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
}

void sts_init(void)
{
    memset(&g_sts, 0, sizeof(g_sts));
    memcpy(g_sts.eye, "NSFSTATS", sizeof(g_sts.eye));
    g_sts.count = 0;
    g_sts.max   = NSFSTS_MAX;
}

STSCTR *sts_register(const char *component, const char *name)
{
    STSREC *r;

    if (g_sts.count >= NSFSTS_MAX) {
        return NULL;                    /* registry full: caller reports it */
    }
    r = &g_sts.rec[g_sts.count];
    memset(r, 0, sizeof(*r));
    sts_copyfield(r->component, sizeof(r->component), component);
    sts_copyfield(r->ctr.name, sizeof(r->ctr.name), name);
    r->ctr.value = 0;
    g_sts.count++;
    return &r->ctr;
}

void sts_reset(void)
{
    UINT i;

    for (i = 0; i < g_sts.count; i++) {
        g_sts.rec[i].ctr.value = 0;     /* keep registrations, zero the values */
    }
}

UINT sts_count(void)
{
    return g_sts.count;
}

UINT sts_render(char *buf, UINT bufsize)
{
    UINT written = 0;
    UINT i;

    if (bufsize == 0) {
        return 0;
    }
    buf[0] = '\0';

    for (i = 0; i < g_sts.count; i++) {
        const STSREC *r = &g_sts.rec[i];
        char line[64];
        int  len;

        /* Precisions bound every read to the field width, so a field that
         * exactly fills its array (no NUL) cannot over-read. */
        len = snprintf(line, sizeof(line), "%.8s %.12s %u\n",
                       r->component, r->ctr.name, (unsigned)r->ctr.value);
        if (len < 0) {
            continue;                   /* encoding error: skip the line */
        }
        if (written + (UINT)len >= bufsize) {
            break;                      /* next whole line would not fit */
        }
        memcpy(buf + written, line, (UINT)len);
        written += (UINT)len;
    }

    buf[written] = '\0';
    return written;
}
