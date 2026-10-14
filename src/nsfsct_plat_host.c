/*----------------------------------------------------------------------------
 * nsfsct_plat_host.c -- host stand-in for src/nsfsct_plat.c ([host].replace).
 *
 * An in-memory SSCT registry and a settable jobname, so the round-1 plumbing --
 * register refuses when one exists, find classifies, remove removes -- is
 * pinned by a host test instead of only by a live run.
 *
 * WHAT THIS DOES NOT PROVE, and the round record says so in the same words:
 * nothing here touches the SSI chain, so it establishes the LOGIC and not the
 * MVS mechanism.  The anchor view below carries the same field NAMES as
 * NSFV_ANCHOR but is a separate struct -- the real OFFSETS are proven by the
 * cross build (where nsfsct_plat.c uses the real members) and by the live gate.
 *--------------------------------------------------------------------------*/
#include "nsfsct.h"
#include "nsfsct_host.h"

#include <string.h>

#define SCT_HOST_MAX  8

typedef struct {
    int   used;
    char  name[NSFSCT_KEYLEN];
    void *suse;
} SCT_HOST_ENT;

static SCT_HOST_ENT g_reg[SCT_HOST_MAX];
static char         g_job[NSFSCT_JOBLEN] = { 'N','S','F','S','T','A','C','K' };
static int          g_job_fail;

void
nsfsct_host_set_jobname(const char *job8)
{
    if (job8 == NULL) { g_job_fail = 1; return; }
    g_job_fail = 0;
    memcpy(g_job, job8, NSFSCT_JOBLEN);
}

void
nsfsct_host_reset(void)
{
    memset(g_reg, 0, sizeof(g_reg));
    g_job_fail = 0;
    memcpy(g_job, "NSFSTACK", NSFSCT_JOBLEN);
}

int
nsfsct_host_count(void)
{
    int i, n = 0;
    for (i = 0; i < SCT_HOST_MAX; i++) if (g_reg[i].used) n++;
    return n;
}

int
nsfsct_plat_jobname(char *out8)
{
    if (out8 == NULL || g_job_fail) return -1;
    memcpy(out8, g_job, NSFSCT_JOBLEN);
    return 0;
}

static SCT_HOST_ENT *
sct_host_lookup(const char *key)
{
    int i;
    for (i = 0; i < SCT_HOST_MAX; i++) {
        if (g_reg[i].used && memcmp(g_reg[i].name, key, NSFSCT_KEYLEN) == 0) {
            return &g_reg[i];
        }
    }
    return NULL;
}

void *
nsfsct_plat_find(const char *key)
{
    SCT_HOST_ENT *e;
    if (key == NULL) return NULL;
    e = sct_host_lookup(key);
    return e ? e->suse : NULL;
}

int
nsfsct_plat_register(const char *key, void *anchor)
{
    int i;
    if (key == NULL || anchor == NULL) return -1;
    for (i = 0; i < SCT_HOST_MAX; i++) {
        if (!g_reg[i].used) {
            g_reg[i].used = 1;
            memcpy(g_reg[i].name, key, NSFSCT_KEYLEN);
            g_reg[i].suse = anchor;
            return 0;
        }
    }
    return -1;
}

int
nsfsct_plat_remove(const char *key)
{
    SCT_HOST_ENT *e;
    if (key == NULL) return -1;
    e = sct_host_lookup(key);
    if (e == NULL) return -1;
    memset(e, 0, sizeof(*e));
    return 0;
}

int
nsfsct_find(void **out)
{
    char                job[NSFSCT_JOBLEN];
    char                key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
    NSFSCT_HOST_ANCHOR *a;
    int                 verdict;

    if (out != NULL) *out = NULL;
    if (nsfsct_plat_jobname(job) != 0) return NSFSCT_NONE;
    /* A key that could be a procedure name is never registered (nsfsct_split,
     * nsfsct_register), so nothing of ours can be under it -- and whatever IS
     * under it belongs to someone else, whose SSCTSUSE this must not read. */
    if (!nsfsct_split(job, key, tail)) return NSFSCT_NONE;

    a = (NSFSCT_HOST_ANCHOR *)nsfsct_plat_find(key);
    if (a == NULL) return NSFSCT_NONE;

    verdict = nsfsct_classify(a->eye, a->version, a->jobname_tail, tail);
    if (out != NULL && verdict != NSFSCT_BADEYE) *out = a;
    return verdict;
}
