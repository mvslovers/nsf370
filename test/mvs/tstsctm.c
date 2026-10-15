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
 * THROWAWAY NAMES, ON PURPOSE.  The two-name arm registers `nsz1` and `nsz2`
 * -- lower case, like every key nsfsct_split derives (ADR-0048 6) --
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
 * ADOPTION IS NOT TESTED HERE.  This registers, finds and removes; round 2
 * adds arm B's private-SSCTSUSE row (find must not read it).  HOLD's anchor is
 * private on purpose since round 2: an NSFS start finding it is the REPLACE
 * row (NSF063W), not the round-1 refusal.
 *
 * PARM:  (none) / 'GATE'  the two-name gate
 *        'HOLD'           chain NSFS's derived SSCT ('nsfs') and leave it --
 *                         the round-1 failure state, for the S NSFS check
 *        'DEAD'           d2 round 2's S1 induction: in the anchor NSFS
 *                         RETAINED (found under 'nsfs'), turn the one CLAIMED
 *                         slot TSTRQXC PARM='LEAK' left into a PENDING request
 *                         owned by an identity the ADR-0040 guard reads DEAD.
 *                         SYNTHETIC DEATH, said here and in the round record:
 *                         a real dying client's slot is reclaimed by the
 *                         running STC within a second (M5-2c1), so it never
 *                         reaches a retained anchor.  What runs for real is
 *                         the adopting start's classify -> reap.
 *        'PROBE'          report whether an SSCT exists under a given name --
 *                         the operator-driven half of the retain/free check,
 *                         which a test program cannot drive by itself because
 *                         it needs a real P NSFS either side.
 * Return codes: 0 passed, 1 ran and failed, 20 could not run.
 *--------------------------------------------------------------------------*/
#include "nsfsct.h"
#include "nsfvsvc.h"
#include "nsfreqx.h"        /* nsfreqx_classify -- DEAD's identity check     */
#include <ibm/mvs/cvt.h>
#include <ibm/mvs/ihaasvt.h>
#include <s370/atomic.h>    /* __cas                                          */
#include <mbtcheck.h>
#include <mvs/subsys.h>
#include <mvs/dd.h>
#include <stdio.h>
#include <string.h>
#include <mvs/apf.h>
#include <mvs/storage.h>    /* getmain/freemain -- arm B's CSA anchor       */
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
    if (nsfsct_plat_find("nsz1") != NULL || nsfsct_plat_find("nsz2") != NULL) {
        printf("  NSZ1/NSZ2 ALREADY CHAINED -- a previous run leaked one.\n");
        wtof("TSTSCTM: THROWAWAY NAMES ALREADY CHAINED -- GATE SKIPPED");
        return -2;
    }

    /* --- register name 1 --- */
    CHECK_EQ((long)nsfsct_plat_register("nsz1", "AAAA", &g_a1), 0L,
             "A: NSZ1 registers on the real chain");
    wtof_name("REGISTERED", "nsz1");
    s1 = nsfsct_plat_find("nsz1");
    CHECK(s1 == (void *)&g_a1,
          "A: NSZ1 is found under NSZ1 -- the control for every refusal below");

    /* THE TWO-NAME PROPERTY.  A second name must not see the first's. */
    CHECK(nsfsct_plat_find("nsz2") == NULL,
          "A: NSZ2 is NOT found while only NSZ1 is chained");
    /* CASE IS PART OF THE NAME ON THE REAL CHAIN.  The whole fix for round 1
     * rests on this: the lower-case key must not answer to the upper-case
     * spelling a procedure name has. */
    CHECK(nsfsct_plat_find("NSZ1") == NULL,
          "A: 'NSZ1' (upper case) is NOT found while 'nsz1' is chained");

    /* --- register name 2 alongside --- */
    CHECK_EQ((long)nsfsct_plat_register("nsz2", "BBBB", &g_a2), 0L,
             "A: NSZ2 registers alongside NSZ1");
    wtof_name("REGISTERED", "nsz2");
    s2 = nsfsct_plat_find("nsz2");
    CHECK(s2 == (void *)&g_a2, "A: NSZ2 finds its OWN anchor");
    CHECK(s2 != s1, "A: ...which is not NSZ1's -- two names, two anchors");
    CHECK(nsfsct_plat_find("nsz1") == (void *)&g_a1,
          "A: and NSZ1 is undisturbed -- a one-name test cannot see this");

    /* --- removal, each with its control in the same run --- */
    CHECK_EQ((long)nsfsct_plat_remove("nsz1"), 0L, "A: NSZ1 removes");
    wtof_name("REMOVED", "nsz1");
    CHECK(nsfsct_plat_find("nsz1") == NULL,
          "A: a removed SSCT is not found -- and it WAS found, four assertions"
          " up, on this same chain");
    CHECK(nsfsct_plat_find("nsz2") == (void *)&g_a2,
          "A: removing NSZ1 left NSZ2 ALONE -- removal is by name, not a sweep");

    /* Removal survives a corrupt anchor, with the intact control just above. */
    memset(g_a2.eye, 0, 8);
    CHECK_EQ((long)nsfsct_plat_remove("nsz2"), 0L,
             "A: removal SURVIVES a corrupt anchor -- ssct_remove_by_name never"
             " dereferences SSCTSUSE (ADR-0048 5.2)");
    wtof_name("REMOVED", "nsz2");
    CHECK(nsfsct_plat_find("nsz2") == NULL, "A: ...and it is gone");

    CHECK_EQ((long)nsfsct_plat_remove("nsz1"), -1L,
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
            NSFV_ANCHOR  *csa = NULL;
            unsigned char savekey;

            /* A WRONG TAIL IN BOTH FIXTURES (round 2b).  Round 1's fixtures
             * wrote the right one themselves, which is what hid that no
             * production code ever did: the CSA control's OK below can only
             * come from register stamping it. */
            mk(&g_a1, NSFSCT_EYE, NSFSCT_VER, "ZZZZ");
            CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
                     "B: nothing under our name yet -> NONE");

            /* ROUND 2: A PRIVATE SSCTSUSE IS NEVER DEREFERENCED.  g_a1 is
             * this job's storage, a perfect anchor by eye, version and tail
             * -- and find must refuse to read it, because at an NSFS start
             * the same value names an address in NSFS's own private area. */
            CHECK_EQ((long)nsfsct_register(&g_a1), 0L,
                     "B: register a PRIVATE anchor");
            CHECK(memcmp(g_a1.jobname_tail, tail, NSFSCT_TAILLEN) == 0,
                  "B: ...register stamped our tail over 'ZZZZ' (round 2b)");
            out = (void *)1;
            CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_BADEYE,
                     "B: a private SSCTSUSE is BADEYE -- never dereferenced,"
                     " however well-formed the bytes behind it");
            CHECK(out == NULL, "B: ...and no address is handed back");
            CHECK_EQ((long)nsfsct_remove(), 0L, "B: remove the private one");

            /* The control: the same anchor contents in CSA, where a real one
             * lives, classify OK.  SP=241 is key 0, so the stores need the
             * key window; freed below on every path. */
            if (__super(PSWKEY0, &savekey) == 0) {
                csa = (NSFV_ANCHOR *)getmain((unsigned)sizeof(NSFV_ANCHOR),
                                             241);
                if (csa != NULL) mk(csa, NSFSCT_EYE, NSFSCT_VER, "ZZZZ");
                __prob(savekey, NULL);
            }
            CHECK(csa != NULL, "B: a CSA anchor for the control was obtained");
            if (csa != NULL) {
                CHECK(memcmp(csa->jobname_tail, "ZZZZ", NSFSCT_TAILLEN) == 0,
                      "B: precondition -- the CSA anchor's tail is 'ZZZZ'");
                CHECK_EQ((long)nsfsct_register(csa), 0L,
                         "B: register the CSA anchor");
                CHECK(memcmp(csa->jobname_tail, tail, NSFSCT_TAILLEN) == 0,
                      "B: ...and register STAMPED our tail into key-0 CSA");
                v = nsfsct_find(&out);
                CHECK_EQ((long)v, (long)NSFSCT_OK,
                         "B: find returns OK -- eye, version and identity all"
                         " agree through the real chain");
                CHECK(out == (void *)csa, "B: ...and hands back our anchor");
                CHECK_EQ((long)nsfsct_register(csa), -1L,
                         "B: a second register under the same name is REFUSED"
                         " (the IEF612I case -- never a second chain entry)");
                CHECK_EQ((long)nsfsct_remove(), 0L, "B: remove");
                CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
                         "B: and it is gone");
                if (__super(PSWKEY0, &savekey) == 0) {
                    freemain(csa);
                    __prob(savekey, NULL);
                }
            }
        }
    }

    /* --- cleanup is asserted, not hoped --- */
    (void)nsfsct_plat_remove("nsz1");
    (void)nsfsct_plat_remove("nsz2");
    CHECK(nsfsct_plat_find("nsz1") == NULL && nsfsct_plat_find("nsz2") == NULL,
          "A: NOTHING LEFT CHAINED -- an SSCT this test leaked would be"
          " registered until IPL and would block a start under that name");
    return 0;
}

/* ---- DEAD: the S1 induction (round 2) ------------------------------------
 *
 * The identity is FABRICATED, and chosen so the guard's own arithmetic reads
 * it DEAD: ASID 1 is the master scheduler, whose ASVT entry is a real ASCB
 * that cannot equal this value -- asserted below through nsfreqx_classify
 * against the live ASVT, not assumed.  Identity first, state LAST and by CS
 * (CLAIMED -> PENDING is the routine's own publish), inflight untouched: the
 * LEAK's count is exactly what the adopting scrub must give back. */
#define SCTM_DEAD_ASCB  0x00FFFFF8u

static int
dead(void)
{
    char         job[NSFSCT_JOBLEN], key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
    NSFV_ANCHOR *a;
    CVT         *cvt;
    ASVT        *asvt;
    UINT         k, claimed = 0, other = 0, idx = 0, in0;
    unsigned char savekey;
    int          cas_rc = -1;

    printf("\n--- DEAD: seed a DEAD-owner PENDING slot in the retained anchor ---\n");
    memcpy(job, "NSFS    ", NSFSCT_JOBLEN);
    (void)nsfsct_split(job, key, tail);
    a = (NSFV_ANCHOR *)nsfsct_plat_find(key);
    printf("  SSCT '%.4s' SSCTSUSE=%08X\n", key, (unsigned)a);
    if (a == NULL) {
        wtof("TSTSCTM: DEAD -- NOTHING CHAINED UNDER %.4s", key);
        return -2;
    }

    /* Only an anchor in the retain posture, of our layout, is touched. */
    CHECK(memcmp(a->eye, NSFSCT_EYE, NSFSCT_EYELEN) == 0,
          "DEAD: the chained anchor carries the eyecatcher");
    CHECK_EQ((long)a->version, (long)NSFV_ANCHOR_VER,
             "DEAD: ...and this build's layout version");
    CHECK_EQ((long)(a->flags & NSFV_ANCHOR_ACTIVE), 0L,
             "DEAD: ...and is RETAINED (ACTIVE clear) -- never a served one");
    CHECK(a->server_ecb_ptr == NULL,
          "DEAD: ...with the wake-ECB pointer withdrawn");
    if (memcmp(a->eye, NSFSCT_EYE, NSFSCT_EYELEN) != 0 ||
        a->version != NSFV_ANCHOR_VER ||
        (a->flags & NSFV_ANCHOR_ACTIVE) || a->server_ecb_ptr != NULL ||
        a->nslots != NSFV_NSLOTS) {
        wtof("TSTSCTM: DEAD -- ANCHOR %08X NOT A RETAINED V%u ANCHOR",
             (unsigned)a, (unsigned)NSFV_ANCHOR_VER);
        return -2;
    }

    for (k = 0; k < NSFV_NSLOTS; k++) {
        UINT st = a->slots[k].req_state;
        if (st == NSFV_REQ_CLAIMED) { claimed++; idx = k; }
        else if (st != NSFV_REQ_FREE) other++;
    }
    in0 = a->inflight;
    printf("  inflight=%u claimed=%u other non-FREE=%u\n",
           (unsigned)in0, (unsigned)claimed, (unsigned)other);
    CHECK(claimed == 1u && other == 0u && in0 == 1u,
          "DEAD: exactly LEAK's state -- one CLAIMED slot, nothing else, one"
          " in flight");
    if (claimed != 1u || other != 0u || in0 != 1u) {
        wtof("TSTSCTM: DEAD -- NOT THE LEAK STATE (CLAIMED=%u OTHER=%u"
             " INFLIGHT=%u)", (unsigned)claimed, (unsigned)other,
             (unsigned)in0);
        return -2;
    }

    cvt  = CVTPTR;
    asvt = (ASVT *)cvt->cvtasvt;
    CHECK_EQ((long)nsfreqx_classify(SCTM_DEAD_ASCB, 1u,
                                    (UINT)asvt->asvtmaxu,
                                    (const UINT *)asvt->asvtenty),
             (long)NSFREQX_CL_DEAD,
             "DEAD: the fabricated identity reads DEAD through the guard's own"
             " arithmetic, against the live ASVT");

    if (__super(PSWKEY0, &savekey) == 0) {
        unsigned want = NSFV_REQ_CLAIMED;
        a->slots[idx].req_ascb = (void *)SCTM_DEAD_ASCB;
        a->slots[idx].req_asid = 1u;
        cas_rc = __cas((unsigned *)&a->slots[idx].req_state, &want,
                       NSFV_REQ_PENDING);
        __prob(savekey, NULL);
    }
    CHECK_EQ((long)cas_rc, 0L, "DEAD: CLAIMED -> PENDING took (CS)");
    CHECK_EQ((long)a->slots[idx].req_state, (long)NSFV_REQ_PENDING,
             "DEAD: the slot reads PENDING");
    CHECK_EQ((long)a->inflight, (long)in0,
             "DEAD: inflight untouched -- the scrub must give it back");
    wtof("TSTSCTM: DEAD SLOT %u PENDING ASCB=%08X ASID=1 INFLIGHT=%u"
         " ANCHOR=%08X -- NOW S NSFS", (unsigned)idx,
         (unsigned)SCTM_DEAD_ASCB, (unsigned)a->inflight, (unsigned)a);
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
     * 'NSFS' blank-padded, giving key 'nsfs' and tail '    '. */
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
    } else if (role[0] == 'D') {
        rc = dead();
        if (rc == -2) return SCTM_CC_SKIP;
    } else if (role[0] == 'H') {
        /* HOLD: chain an SSCT under NSFS's derived key ('nsfs') and LEAVE it.
         *
         * This is the state round 1 failed in -- an SSCT under the derived name
         * when `S NSFS` is issued -- made WITHOUT a retained anchor, so it
         * costs no CSA until IPL.  The round then issues S NSFS: it must START
         * (round 1: IEF612I PROCEDURE NOT FOUND), refuse to chain a second one
         * (NSF059W), and its clean stop must remove this one by name.
         *
         * SSCTSUSE points into this job's own storage, which goes away with
         * the job.  Round 1 dereferences it nowhere (register only asks
         * whether the name is chained); round 2's adoption would, which is why
         * HOLD belongs to round 1 only. */
        char job[NSFSCT_JOBLEN], key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];
        memcpy(job, "NSFS    ", NSFSCT_JOBLEN);
        (void)nsfsct_split(job, key, tail);
        mk(&g_a1, NSFSCT_EYE, NSFSCT_VER, tail);
        if (nsfsct_plat_find(key) != NULL) {
            printf("  '%.4s' is already chained -- HOLD has nothing to do\n", key);
            wtof("TSTSCTM: HOLD %.4s ALREADY CHAINED", key);
            return SCTM_CC_SKIP;
        }
        CHECK_EQ((long)nsfsct_plat_register(key, tail, &g_a1), 0L,
                 "HOLD: the derived key 'nsfs' chains on the real SSI chain");
        CHECK(nsfsct_plat_find(key) != NULL, "HOLD: ...and is found under it");
        CHECK(nsfsct_plat_find("NSFS") == NULL,
              "HOLD: ...but NOT under 'NSFS', the procedure name");
        wtof("TSTSCTM: HOLD %.4s CHAINED SSCTSUSE=%08X -- NOW S NSFS",
             key, (unsigned)&g_a1);
        rc = 0;
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
