/*----------------------------------------------------------------------------
 * nsfsct_plat.c -- the d2 rendezvous, MVS half.  Swapped for
 * src/nsfsct_plat_host.c on the host by [host].replace.
 *
 * Everything here that the pure half cannot do: the TIOT jobname, the SSI
 * chain, and the one place the anchor is dereferenced.
 *
 * ADR-0048 round 1.  Nothing in this file adopts anything.
 *--------------------------------------------------------------------------*/
#include "nsfsct.h"
#include "nsfvsvc.h"    /* the real NSFV_ANCHOR -- available HERE, not in the
                        ** pure half, which is why nsfsct_find lives here     */

#include <mvs/subsys.h> /* ssct_new / ssct_find / ssct_install / _remove_by_name */
#include <mvs/dd.h>     /* __jobname                                          */
#include <string.h>
#include <mvs/apf.h>    /* PSWKEY0/__prob/__super */

/*--------------------------------------------------------------------------
 * THE MIRROR IS CHECKED HERE, where both headers are in scope -- a mirror
 * nobody compares is a mirror that drifts, and the entire point of the pure
 * half is that it CAN be wrong in isolation.  These are negative-array
 * typedefs rather than NSF_SIZE_ASSERT because that macro is a deliberate
 * no-op off-target (nsf.h), and a check that only fires on one of the two
 * builds is the shape this file exists to avoid.
 *------------------------------------------------------------------------*/
typedef char nsfsct_chk_ver[(NSFSCT_VER == NSFV_ANCHOR_VER) ? 1 : -1];
typedef char nsfsct_chk_eye[(sizeof(((NSFV_ANCHOR *)0)->eye)
                             == NSFSCT_EYELEN) ? 1 : -1];
typedef char nsfsct_chk_tail[(sizeof(((NSFV_ANCHOR *)0)->jobname_tail)
                              == NSFSCT_TAILLEN) ? 1 : -1];

int
nsfsct_plat_jobname(char *out8)
{
    const char *job;

    if (out8 == NULL) return -1;
    job = __jobname();
    if (job == NULL) return -1;
    memcpy(out8, job, NSFSCT_JOBLEN);
    return 0;
}

/* A CL4 key is not NUL-terminated; libc370's ssct_* take a C string, so the
 * key is copied into a 5-byte buffer here rather than at every call site. */
static void
sct_keyz(const char *key, char *z5)
{
    memcpy(z5, key, NSFSCT_KEYLEN);
    z5[NSFSCT_KEYLEN] = '\0';
}

void *
nsfsct_plat_find(const char *key)
{
    char  z[NSFSCT_KEYLEN + 1];
    SSCT *ssct;

    if (key == NULL) return NULL;
    sct_keyz(key, z);

    /* Reading the SSI chain needs no authorization and no key change -- JESCT
     * and the SSCTs are common storage, and this is a fetch.  Only the
     * install/remove paths below enter supervisor state. */
    ssct = ssct_find(z);
    if (ssct == NULL) return NULL;
    return ssct->ssctsuse;
}

int
nsfsct_plat_register(const char *key, void *anchor)
{
    char           z[NSFSCT_KEYLEN + 1];
    unsigned char  savekey;
    SSCT          *ssct;
    int            rc;

    if (key == NULL || anchor == NULL) return -1;
    sct_keyz(key, z);

    /* ssct_new and ssct_install both require supervisor state (clibssct.h). */
    if (__super(PSWKEY0, &savekey)) return -1;

    /* NO SSVT.  The SSCT is a RENDEZVOUS here, not a transport: nothing is
     * dispatched through it, no IEFSSREQ is issued, and no client touches it
     * (ADR-0048 5.1).  A vector table would be the transport ADR-0036 was
     * superseded for. */
    ssct = ssct_new(z, NULL, anchor);
    if (ssct == NULL) {
        __prob(savekey, NULL);
        return -1;
    }
    rc = ssct_install(ssct, NULL);
    if (rc != 0) {
        ssct_free(ssct);
        __prob(savekey, NULL);
        return -1;
    }
    __prob(savekey, NULL);
    return 0;
}

int
nsfsct_plat_remove(const char *key)
{
    char           z[NSFSCT_KEYLEN + 1];
    unsigned char  savekey;
    int            rc;

    if (key == NULL) return -1;
    sct_keyz(key, z);

    if (__super(PSWKEY0, &savekey)) return -1;
    /* ssct_remove_by_name is ssct_find followed by ssct_remove, so it NEVER
     * dereferences SSCTSUSE.  That is what makes a corrupt anchor -- or one
     * pointing at freed storage -- removable anyway, and it is the mechanism
     * ADR-0048 5.2 answers the lifecycle objection with. */
    rc = ssct_remove_by_name(z);
    __prob(savekey, NULL);
    return (rc == 0) ? 0 : -1;
}

/*--------------------------------------------------------------------------
 * find -- the one entry point that dereferences the anchor, which is why it
 * is here and not in the pure half.  The offsets are the compiler's, taken
 * from the real NSFV_ANCHOR; the CLASSIFICATION is the pure half's.
 *------------------------------------------------------------------------*/
int
nsfsct_find(void **out)
{
    char         job[NSFSCT_JOBLEN];
    char         key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
    NSFV_ANCHOR *anchor;

    if (out != NULL) *out = NULL;
    if (nsfsct_plat_jobname(job) != 0) return NSFSCT_NONE;
    nsfsct_split(job, key, tail);

    anchor = (NSFV_ANCHOR *)nsfsct_plat_find(key);
    if (anchor == NULL) {
        /* No SSCT under our name, or one whose SSCTSUSE is null.  NONE is the
         * safe reading of both: it leads to an ordinary start, where BADEYE
         * would invite a round-2 caller to remove a registration that may not
         * be there. */
        return NSFSCT_NONE;
    }

    {
        int verdict = nsfsct_classify(anchor->eye, anchor->version,
                                      anchor->jobname_tail, tail);
        if (out != NULL && verdict != NSFSCT_BADEYE) {
            *out = anchor;
        }
        return verdict;
    }
}
