/*----------------------------------------------------------------------------
 * tstsctm.c -- d2 round 1 (ADR-0048): the rendezvous, LIVE.  MVS-only.
 *
 * TWO NAMES, NOT TWO STACKS.  Only one instance can steal SVC 239 today, so a
 * second NSFS is not available -- and the rendezvous does not need one.  Driving
 * libc370's ssct_new / ssct_find / ssct_remove_by_name under two DISTINCT names
 * tests exactly the property the fixed CSA cell and the stolen SVC slot failed:
 * that the registration is keyed by a NAME and not by a place.
 *
 * WHAT THAT DOES NOT ESTABLISH, said here and in the round record: it proves the
 * rendezvous distinguishes two names.  It does NOT prove two stacks can run.
 *
 * THROWAWAY NAMES, ON PURPOSE.  The two-name arm registers `NSZ1` and `NSZ2`,
 * which belong to no product, so a live NSFS's own registration is never
 * touched -- the throwaway-id discipline the SMP rules state, one mechanism
 * over.  The full-API arm uses this JOB's derived name, which is the test
 * runner's and likewise not NSFS's.
 *
 * IT CLEANS UP AFTER ITSELF AND ASSERTS THAT IT DID.  An SSCT this test leaves
 * chained is registered until IPL and would block a start under that name --
 * which is the very failure round 1 exists to prevent, so leaving one behind
 * would be the test reproducing the defect.
 *
 * ADOPTION IS NOT TESTED HERE.  Round 1 registers, finds and removes.
 *
 * PARM:  (none) / 'GATE'  the two-name gate
 *        'PROBE'          report whether an SSCT exists under a given name --
 *                         the operator-driven half of the retain/free check,
 *                         which a test program cannot drive by itself because
 *                         it needs a real P NSFS either side.
 * Return codes: 0 passed, 1 ran and failed, 20 could not run.
 *--------------------------------------------------------------------------*/
#include "nsfsct.h"
#include "nsfvsvc.h"
#include <mbtcheck.h>
#include <mvs/subsys.h>
#include <mvs/dd.h>
#include <stdio.h>
#include <string.h>
#include <mvs/apf.h>
#include <mvs/wto.h>

#define SCTM_CC_SKIP   20

/* A stand-in anchor, shaped like the real one only in the fields find reads.
 * It lives in this job's storage, never in CSA: round 1 tests the RENDEZVOUS,
 * and what it points at only has to be readable. */
static NSFV_ANCHOR g_a1;
static NSFV_ANCHOR g_a2;

static void
mk(NSFV_ANCHOR *a, const char *eye, UINT ver, const char *tail)
{
    memset(a, 0, sizeof(*a));
    memcpy(a->eye, eye, 8);
    a->version = ver;
    memcpy(a->jobname_tail, tail, NSFSCT_TAILLEN);
}

static void
wtof_name(const char *tag, const char *key)
{
    wtof("TSTSCTM: %s %c%c%c%c", tag, key[0], key[1], key[2], key[3]);
}

/* ---- the two-name gate -------------------------------------------------- */
static int
gate(void)
{
    void *s1, *s2;

    printf("\n--- two names on the real SSI chain ---\n");
    mk(&g_a1, NSFSCT_EYE, NSFSCT_VER, "AAAA");
    mk(&g_a2, NSFSCT_EYE, NSFSCT_VER, "BBBB");

    /* Precondition, not an assertion about the stack: if either throwaway name
     * is already chained, a previous run left one behind and this run cannot be
     * aimed.  SKIP rather than register a second one. */
    if (nsfsct_plat_find("NSZ1") != NULL || nsfsct_plat_find("NSZ2") != NULL) {
        printf("  NSZ1/NSZ2 ALREADY CHAINED -- a previous run leaked one.\n");
        wtof("TSTSCTM: THROWAWAY NAMES ALREADY CHAINED -- GATE SKIPPED");
        return -2;
    }

    /* --- register name 1 --- */
    CHECK_EQ((long)nsfsct_plat_register("NSZ1", &g_a1), 0L,
             "A: NSZ1 registers on the real chain");
    wtof_name("REGISTERED", "NSZ1");
    s1 = nsfsct_plat_find("NSZ1");
    CHECK(s1 == (void *)&g_a1,
          "A: NSZ1 is found under NSZ1 -- the control for every refusal below");

    /* THE TWO-NAME PROPERTY.  A second name must not see the first's. */
    CHECK(nsfsct_plat_find("NSZ2") == NULL,
          "A: NSZ2 is NOT found while only NSZ1 is chained");

    /* --- register name 2 alongside --- */
    CHECK_EQ((long)nsfsct_plat_register("NSZ2", &g_a2), 0L,
             "A: NSZ2 registers alongside NSZ1");
    wtof_name("REGISTERED", "NSZ2");
    s2 = nsfsct_plat_find("NSZ2");
    CHECK(s2 == (void *)&g_a2, "A: NSZ2 finds its OWN anchor");
    CHECK(s2 != s1, "A: ...which is not NSZ1's -- two names, two anchors");
    CHECK(nsfsct_plat_find("NSZ1") == (void *)&g_a1,
          "A: and NSZ1 is undisturbed -- a one-name test cannot see this");

    /* --- removal, each with its control in the same run --- */
    CHECK_EQ((long)nsfsct_plat_remove("NSZ1"), 0L, "A: NSZ1 removes");
    wtof_name("REMOVED", "NSZ1");
    CHECK(nsfsct_plat_find("NSZ1") == NULL,
          "A: a removed SSCT is not found -- and it WAS found, four assertions"
          " up, on this same chain");
    CHECK(nsfsct_plat_find("NSZ2") == (void *)&g_a2,
          "A: removing NSZ1 left NSZ2 ALONE -- removal is by name, not a sweep");

    /* Removal survives a corrupt anchor, with the intact control just above. */
    memset(g_a2.eye, 0, 8);
    CHECK_EQ((long)nsfsct_plat_remove("NSZ2"), 0L,
             "A: removal SURVIVES a corrupt anchor -- ssct_remove_by_name never"
             " dereferences SSCTSUSE (ADR-0048 5.2)");
    wtof_name("REMOVED", "NSZ2");
    CHECK(nsfsct_plat_find("NSZ2") == NULL, "A: ...and it is gone");

    CHECK_EQ((long)nsfsct_plat_remove("NSZ1"), -1L,
             "A: removing an absent name reports failure, not success");

    /* --- the full API, under THIS job's own derived name --- */
    {
        char  job[NSFSCT_JOBLEN], key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
        void *out = NULL;
        int   v;

        printf("\n--- the full API under this job's own name ---\n");
        CHECK_EQ((long)nsfsct_plat_jobname(job), 0L, "B: the TIOT jobname reads");
        nsfsct_split(job, key, tail);
        printf("  jobname '%.8s' -> key '%.4s' tail '%.4s'\n", job, key, tail);
        wtof("TSTSCTM: JOBNAME %.8s KEY %.4s", job, key);

        if (nsfsct_plat_find(key) != NULL) {
            printf("  our own derived name is already chained -- skipping the"
                   " API arm rather than disturbing it\n");
            wtof("TSTSCTM: OWN NAME CHAINED -- API ARM SKIPPED");
        } else {
            mk(&g_a1, NSFSCT_EYE, NSFSCT_VER, tail);
            CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
                     "B: nothing under our name yet -> NONE");
            CHECK_EQ((long)nsfsct_register(&g_a1), 0L, "B: register");
            v = nsfsct_find(&out);
            CHECK_EQ((long)v, (long)NSFSCT_OK,
                     "B: find returns OK -- eye, version and identity all agree"
                     " through the real chain");
            CHECK(out == (void *)&g_a1, "B: ...and hands back our anchor");
            CHECK_EQ((long)nsfsct_register(&g_a1), -1L,
                     "B: a second register under the same name is REFUSED (the"
                     " IEF612I case -- never a second chain entry)");
            CHECK_EQ((long)nsfsct_remove(), 0L, "B: remove");
            CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
                     "B: and it is gone");
        }
    }

    /* --- cleanup is asserted, not hoped --- */
    (void)nsfsct_plat_remove("NSZ1");
    (void)nsfsct_plat_remove("NSZ2");
    CHECK(nsfsct_plat_find("NSZ1") == NULL && nsfsct_plat_find("NSZ2") == NULL,
          "A: NOTHING LEFT CHAINED -- an SSCT this test leaked would be"
          " registered until IPL and would block a start under that name");
    return 0;
}

/* ---- PROBE: the operator-driven half ------------------------------------ */
static int
probe(void)
{
    char  job[NSFSCT_JOBLEN], key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
    void *suse;

    printf("\n--- PROBE ---\n");
    /* NSFS's own name, derived the way NSFS derives it.  This job cannot read
     * another job's TIOT, so the name is spelled out: the STC is NSFS, which is
     * 'NSFS' blank-padded, giving key 'NSFS' and tail '    '. */
    memcpy(job, "NSFS    ", NSFSCT_JOBLEN);
    nsfsct_split(job, key, tail);

    suse = nsfsct_plat_find(key);
    printf("  SSCT '%.4s': %s", key, suse ? "CHAINED" : "not chained");
    if (suse) printf("  SSCTSUSE=%08X", (unsigned)suse);
    printf("\n");
    wtof("TSTSCTM: PROBE %.4s %s %08X", key,
         suse ? "CHAINED" : "ABSENT", (unsigned)suse);

    /* A REPORT, NOT AN ASSERTION.  Whether NSFS's SSCT should be chained right
     * now depends on which stop path the STC last took, which this program
     * cannot know.  The round reads two PROBE runs either side of a P NSFS. */
    printf("  (report only -- the round compares two PROBEs across a stop)\n");
    return 0;
}

int
main(int argc, char **argv)
{
    const char *role = (argc > 1 && argv[1] != NULL) ? argv[1] : "GATE";
    int rc;

    printf("=== nsf370 d2 round 1: the rendezvous, live (TSTSCTM) ===\n");
    printf("role: '%s'\n", role);
    wtof("TSTSCTM: START ROLE '%s'", role);

    /* The job STARTS unauthorised, like every other client in this tree --
     * asserted before anything changes it, so the next step is a transition and
     * not an assumption. */
    CHECK_EQ((long)__isauth(), 0L,
             "the job starts UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    /* AND THEN SELF-AUTHORISES, BECAUSE REGISTERING AN SSCT REQUIRES IT.
     * ssct_new and ssct_install need supervisor state (clibssct.h), which
     * __super will not grant an unauthorised task.  That is not a property of
     * the test: in production nsfsct_register runs inside NSFS, which is ac=1
     * and has already called clib_apf_setup, so this reproduces the real
     * caller's state rather than working around a restriction.
     *
     * READING the chain needs none of this -- nsfsct_plat_find is a fetch of
     * common storage -- so PROBE deliberately runs without it, which is also
     * the honest demonstration that a finder needs no authority. */
    if (role[0] != 'P') {
        CHECK_EQ((long)clib_apf_setup("TSTSCTM"), 0L,
                 "self-auth (SVC 244) -- ssct_new/ssct_install need supervisor"
                 " state, exactly as they do inside NSFS");
        if (__isauth() == 0) {
            printf("  SELF-AUTH FAILED -- the register arms cannot run.\n");
            wtof("TSTSCTM: SELF-AUTH FAILED -- GATE SKIPPED");
            return SCTM_CC_SKIP;
        }
    }

    if (role[0] == 'P') {
        rc = probe();
    } else if (role[0] == 'R' && role[1] == 'M') {
        /* RECOVERY AID: RMxxxx removes the SSCT named xxxx.
         *
         * It exists because this round DISCOVERED that a chained SSCT whose
         * name equals a PROCEDURE name makes `S <name>` fail IEF612I PROCEDURE
         * NOT FOUND -- MVS resolves the name as a subsystem first.  A round
         * that can create a condition it cannot clear is not finished, and
         * without this the only way back is an IPL. */
        char key[NSFSCT_KEYLEN];
        int  i;
        for (i = 0; i < NSFSCT_KEYLEN; i++) {
            key[i] = (role[2 + i] != '\0') ? role[2 + i] : ' ';
        }
        printf("  removing SSCT '%.4s'...\n", key);
        rc = nsfsct_plat_remove(key);
        printf("  rc=%d ; now %s\n", rc,
               nsfsct_plat_find(key) ? "STILL CHAINED" : "ABSENT");
        wtof("TSTSCTM: RM %.4s rc=%d %s", key, rc,
             nsfsct_plat_find(key) ? "STILL-CHAINED" : "ABSENT");
        CHECK(nsfsct_plat_find(key) == NULL, "RM: the name is no longer chained");
        rc = 0;
    } else {
        rc = gate();
    }

    if (rc == -2) {
        printf("=== TSTSCTM: GATE SKIPPED -- CC %d, NOT a pass ===\n",
               SCTM_CC_SKIP);
        wtof("TSTSCTM: GATE SKIPPED -- CC %d, NOT a pass", SCTM_CC_SKIP);
        return SCTM_CC_SKIP;
    }
    wtof("TSTSCTM: DONE");
    return mbt_test_summary("TSTSCTM");
}
