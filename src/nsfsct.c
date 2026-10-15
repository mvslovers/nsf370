/*----------------------------------------------------------------------------
 * nsfsct.c -- the d2 rendezvous, PURE half + the three round-1 entry points.
 *
 * ADR-0048 round 1: register, find, remove.  Round 2 adds the start-action
 * table; the adoption itself is src/nsfsx.c's.
 *
 * Everything in this file that touches MVS goes through the nsfsct_plat_*
 * seam, so the truth table below is pinned by a host test (test/tstsct.c)
 * instead of by a live run that would have to arrange every row.
 *--------------------------------------------------------------------------*/
#include "nsfsct.h"

#include <ctype.h>      /* isupper, tolower (EBCDIC on MVS, ASCII on host)    */
#include <string.h>     /* memcpy, memcmp                                     */

/* nsfsct_find LIVES IN THE PLATFORM HALF, DELIBERATELY.  It is the one entry
 * point that must DEREFERENCE the anchor -- eyecatcher, version, jobname_tail
 * -- and this file cannot include nsfvsvc.h (see the header).  Writing it here
 * would mean spelling those offsets as literals, and a hand-copied offset that
 * nothing compares against the struct is precisely the drift this project has
 * paid for before.  Over there the real NSFV_ANCHOR is in scope and the
 * compiler computes them. */

/*--------------------------------------------------------------------------
 * PURE -- split the jobname.
 *------------------------------------------------------------------------*/
int
nsfsct_split(const char *jobname, char *key, char *tail)
{
    int i, folded = 0;

    if (jobname == NULL) {
        /* A caller with no jobname has no identity, and inventing one here
         * would register SOMETHING under a name nobody can reproduce.  Blanks
         * are what the TIOT would have given us for an empty name, so they are
         * what a caller gets -- and nsfsct_plat_jobname fails separately, which
         * is where the condition is actually reported. */
        if (key  != NULL) memset(key,  ' ', NSFSCT_KEYLEN);
        if (tail != NULL) memset(tail, ' ', NSFSCT_TAILLEN);
        return 0;
    }
    /* The key is folded through the C library's own classification, never by
     * byte arithmetic: on MVS that is EBCDIC, where the letters are not one
     * contiguous range, and on the host it is ASCII.  Only upper-case letters
     * change; digits, blanks and national characters (@ # $) stay as they are,
     * which is why the count of folded characters is the answer to "can this
     * key be a procedure name". */
    for (i = 0; i < NSFSCT_KEYLEN; i++) {
        unsigned char c = (unsigned char)jobname[i];
        if (isupper(c)) {
            folded++;
            c = (unsigned char)tolower(c);
        }
        if (key != NULL) key[i] = (char)c;
    }
    if (tail != NULL) memcpy(tail, jobname + NSFSCT_KEYLEN, NSFSCT_TAILLEN);
    return folded > 0;
}

/*--------------------------------------------------------------------------
 * PURE -- classify.  See the header for why the order is checked-then-trusted.
 *------------------------------------------------------------------------*/
int
nsfsct_classify(const char *eye, UINT version,
                const char *stored_tail, const char *our_tail)
{
    /* 1. Is there an anchor behind the SSCT at all, and is it plausibly ours?
     *    A null eye is SSCTSUSE == NULL, which is a registration whose anchor
     *    was never stored or has been cleared.  Same verdict either way: there
     *    is nothing here to believe. */
    if (eye == NULL) {
        return NSFSCT_BADEYE;
    }
    if (memcmp(eye, NSFSCT_EYE, NSFSCT_EYELEN) != 0) {
        return NSFSCT_BADEYE;
    }

    /* 2. Do we understand the layout?  If not, STOP HERE -- and in particular
     *    do NOT fall through to the identity compare, because jobname_tail's
     *    offset is a property of THIS version.  Reading it out of an anchor
     *    written by a different build compares an arbitrary byte range. */
    if (version != NSFSCT_VER) {
        return NSFSCT_BADVER;
    }

    /* 3. Is it ours, or a CL4 truncation collision?  SSCTSNAM carries chars
     *    0..3 and matched by construction (we found it under our own name), so
     *    the tail is the whole of the remaining question. */
    if (stored_tail == NULL || our_tail == NULL) {
        return NSFSCT_FOREIGN;
    }
    if (memcmp(stored_tail, our_tail, NSFSCT_TAILLEN) != 0) {
        return NSFSCT_FOREIGN;
    }

    return NSFSCT_OK;
}

const char *
nsfsct_verdict_name(int verdict)
{
    switch (verdict) {
    case NSFSCT_NONE:    return "NONE";
    case NSFSCT_OK:      return "OK";
    case NSFSCT_BADEYE:  return "BADEYE";
    case NSFSCT_BADVER:  return "BADVER";
    case NSFSCT_FOREIGN: return "FOREIGN";
    default:             return "?";
    }
}

int
nsfsct_posture(int nslots_ok, int active, int ecb_published, int server_gone)
{
    if (!nslots_ok)               return NSFSCT_POSTURE_DAMAGED;
    if (active || ecb_published)  return NSFSCT_POSTURE_SERVED;
    if (!server_gone)             return NSFSCT_POSTURE_ALIVE;
    return NSFSCT_POSTURE_RETAINED;
}

int
nsfsct_start_action(int verdict, int posture)
{
    switch (verdict) {
    case NSFSCT_NONE:
        return NSFSCT_START_FRESH;
    case NSFSCT_OK:
        if (posture == NSFSCT_POSTURE_RETAINED) return NSFSCT_START_ADOPT;
        if (posture == NSFSCT_POSTURE_DAMAGED)  return NSFSCT_START_REPLACE;
        return NSFSCT_START_ASIDE;
    case NSFSCT_BADEYE:
    case NSFSCT_BADVER:
        return NSFSCT_START_REPLACE;
    case NSFSCT_FOREIGN:
    default:
        /* An unknown verdict is a newer classifier than this table: touch
         * nothing, which is the one action that cannot be wrong. */
        return NSFSCT_START_ASIDE;
    }
}

/*--------------------------------------------------------------------------
 * THE ROUND-1 API.
 *------------------------------------------------------------------------*/

/* Our own name, derived once per call rather than cached.  Caching it would
 * create a second place the name can be wrong, and the derivation is three
 * instructions over a field MVS already holds. */
static int
sct_our_name(char *key, char *tail)
{
    char job[NSFSCT_JOBLEN];

    if (nsfsct_plat_jobname(job) != 0) {
        return -1;
    }
    return nsfsct_split(job, key, tail) ? 0 : NSFSCT_RC_NOTSAFE;
}

int
nsfsct_register(void *anchor)
{
    char key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
    int  rc;

    if (anchor == NULL) return -1;
    /* A key folding could not change could equal a procedure name, and an SSCT
     * under a procedure name makes the next `S <name>` fail IEF612I (round 1).
     * Not registering costs the rendezvous; registering costs the restart. */
    if ((rc = sct_our_name(key, tail)) != 0) return rc;

    /* REFUSE RATHER THAN CHAIN A SECOND ONE.  An SSCT already under our name
     * is the IEF612I case, and adding another is the failure and not the fix
     * (ADR-0048 2, red line).  The caller reports it; this function's job is
     * to not make it worse.
     *
     * Note what this does NOT do: it does not look at what the existing SSCT
     * points at, does not classify it and does not remove it.  Deciding
     * between "my own predecessor, adopt it" and "someone else's, refuse" is
     * nsfsct_start_action's, and nsfsx_start asks it BEFORE calling this. */
    if (nsfsct_plat_find(key) != NULL) {
        return -1;
    }

    return nsfsct_plat_register(key, anchor);
}

int
nsfsct_remove(void)
{
    char key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];

    if (sct_our_name(key, tail) != 0) return -1;
    return nsfsct_plat_remove(key);
}
