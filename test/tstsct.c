/*----------------------------------------------------------------------------
 * tstsct.c -- d2 round 1 (ADR-0048): the rendezvous, host half.
 *
 * WHAT THIS ESTABLISHES: the pure truth table (split + classify) and the
 * register / find / remove plumbing, driven against the in-memory shim.
 *
 * WHAT IT DOES NOT: anything about the SSI chain, and anything about the real
 * NSFV_ANCHOR offsets -- the shim's anchor view shares the FIELD NAMES and not
 * the layout.  The offsets are the cross build's (src/nsfsct_plat.c uses the
 * real members); the mechanism is TSTSCTM's.  Said here as well as in the round
 * record, because a green host run is exactly the thing that gets over-read.
 *
 * NOT TESTED HERE AND DELIBERATELY SO: adoption.  Round 1 finds and removes;
 * what a start DOES with a retained anchor is round 2, and no assertion below
 * may quietly assume it.
 *--------------------------------------------------------------------------*/
#include "nsfsct.h"
#include "nsfsct_host.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

static void
mk_anchor(NSFSCT_HOST_ANCHOR *a, const char *eye, UINT ver, const char *tail)
{
    memset(a, 0, sizeof(*a));
    memcpy(a->eye, eye, 8);
    a->version = ver;
    memcpy(a->jobname_tail, tail, NSFSCT_TAILLEN);
}

/* ---- 1. the split ------------------------------------------------------- */
static void
t_split(void)
{
    char key[NSFSCT_KEYLEN], tail[NSFSCT_TAILLEN];

    printf("\n--- 1. jobname split ---\n");

    nsfsct_split("NSFSTACK", key, tail);
    CHECK(memcmp(key,  "NSFS", 4) == 0, "1: key is jobname[0:4]");
    CHECK(memcmp(tail, "TACK", 4) == 0, "1: tail is jobname[4:8]");

    /* THE COLLISION THE WHOLE RULE EXISTS FOR: two jobnames, one CL4 key. */
    {
        char k2[NSFSCT_KEYLEN], t2[NSFSCT_TAILLEN];
        nsfsct_split("NSFSTEST", k2, t2);
        CHECK(memcmp(key, k2, 4) == 0,
              "1: NSFSTACK and NSFSTEST COLLIDE on the CL4 key -- which is the"
              " reason the tail exists");
        CHECK(memcmp(tail, t2, 4) != 0,
              "1: ...and the TAILS distinguish them exactly");
    }

    /* A short name arrives blank-padded and is NOT trimmed: trimming would make
     * "NSF " and "NSF" two spellings of one name. */
    nsfsct_split("NSF     ", key, tail);
    CHECK(memcmp(key,  "NSF ", 4) == 0, "1: a short name keeps its padding");
    CHECK(memcmp(tail, "    ", 4) == 0, "1: ...tail included");

    nsfsct_split(NULL, key, tail);
    CHECK(memcmp(key, "    ", 4) == 0,
          "1: a NULL jobname yields blanks, never invented identity");
}

/* ---- 2. classify: every row of the truth table -------------------------- */
static void
t_classify(void)
{
    printf("\n--- 2. classify ---\n");

    CHECK_EQ((long)nsfsct_classify(NSFSCT_EYE, NSFSCT_VER, "TACK", "TACK"),
             (long)NSFSCT_OK, "2: eye + version + identity -> OK");

    CHECK_EQ((long)nsfsct_classify(NULL, NSFSCT_VER, "TACK", "TACK"),
             (long)NSFSCT_BADEYE, "2: SSCTSUSE null -> BADEYE");
    CHECK_EQ((long)nsfsct_classify("XXXXXXXX", NSFSCT_VER, "TACK", "TACK"),
             (long)NSFSCT_BADEYE, "2: wrong eyecatcher -> BADEYE");

    CHECK_EQ((long)nsfsct_classify(NSFSCT_EYE, NSFSCT_VER + 1u, "TACK", "TACK"),
             (long)NSFSCT_BADVER, "2: version mismatch -> BADVER");
    CHECK_EQ((long)nsfsct_classify(NSFSCT_EYE, NSFSCT_VER - 1u, "TACK", "TACK"),
             (long)NSFSCT_BADVER, "2: ...in either direction");

    CHECK_EQ((long)nsfsct_classify(NSFSCT_EYE, NSFSCT_VER, "TEST", "TACK"),
             (long)NSFSCT_FOREIGN,
             "2: eye+version good but the tail differs -> FOREIGN (the CL4"
             " truncation collision, caught rather than silently adopted)");

    /* THE ORDER IS AN ASSERTION, NOT AN IMPLEMENTATION DETAIL.  A
     * version-mismatched anchor must NOT be classified on its identity: the
     * offset of jobname_tail is a property of THIS version, so reading it out
     * of a foreign layout compares an arbitrary byte range.  If the checks were
     * reordered, this row would come back FOREIGN. */
    CHECK_EQ((long)nsfsct_classify(NSFSCT_EYE, NSFSCT_VER + 1u, "TEST", "TACK"),
             (long)NSFSCT_BADVER,
             "2: version is checked BEFORE identity -- a foreign layout's tail"
             " is not a tail");

    /* And a bad eyecatcher outranks a bad version, for the same reason one
     * level up: without the eyecatcher there is no reason to believe the
     * version field is a version field. */
    CHECK_EQ((long)nsfsct_classify("XXXXXXXX", NSFSCT_VER + 1u, "TEST", "TACK"),
             (long)NSFSCT_BADEYE, "2: eye is checked BEFORE version");

    CHECK(strcmp(nsfsct_verdict_name(NSFSCT_OK), "OK") == 0,
          "2: verdicts have names, for messages and for the record");
    CHECK(strcmp(nsfsct_verdict_name(99), "?") == 0,
          "2: ...including an unknown one");
}

/* ---- 3. register / find / remove, and the two-name property ------------- */
static void
t_plumbing(void)
{
    NSFSCT_HOST_ANCHOR a_anchor, b_anchor;
    void *out = NULL;

    printf("\n--- 3. register / find / remove ---\n");
    nsfsct_host_reset();
    mk_anchor(&a_anchor, NSFSCT_EYE, NSFSCT_VER, "TACK");
    mk_anchor(&b_anchor, NSFSCT_EYE, NSFSCT_VER, "TEST");

    /* --- name A --- */
    nsfsct_host_set_jobname("NSFSTACK");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
             "3: nothing registered yet -> NONE");
    CHECK(out == NULL, "3: ...and no anchor handed back");

    CHECK_EQ((long)nsfsct_register(&a_anchor), 0L, "3: A registers");
    CHECK_EQ((long)nsfsct_host_count(), 1L, "3: exactly one registration");

    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_OK,
             "3: A finds its OWN -- the control for every refusal below");
    CHECK(out == (void *)&a_anchor, "3: ...and gets its own anchor back");

    /* THE IEF612I RED LINE: a second register under the same name must REFUSE,
     * never chain a second entry. */
    CHECK_EQ((long)nsfsct_register(&a_anchor), -1L,
             "3: registering twice under one name is REFUSED");
    CHECK_EQ((long)nsfsct_host_count(), 1L,
             "3: ...and nothing was chained -- still exactly one");

    /* --- the two-name property, which is what the fixed cell and the stolen
     * slot failed.  B's name must not find A's registration. --- */
    nsfsct_host_set_jobname("NSFBTEST");
    out = (void *)1;
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
             "3: name B does NOT find name A's SSCT");
    CHECK(out == NULL, "3: ...and hands back nothing");

    CHECK_EQ((long)nsfsct_register(&b_anchor), 0L, "3: B registers alongside A");
    CHECK_EQ((long)nsfsct_host_count(), 2L, "3: TWO names coexist");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_OK, "3: B finds its own");
    CHECK(out == (void *)&b_anchor, "3: ...and gets B's anchor, not A's");

    /* A is undisturbed -- the control that B's registration did not overwrite
     * it, which a one-name test cannot see at all. */
    nsfsct_host_set_jobname("NSFSTACK");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_OK, "3: A still finds its own");
    CHECK(out == (void *)&a_anchor, "3: ...still A's anchor");

    /* --- removal, with its control in the same run --- */
    CHECK_EQ((long)nsfsct_remove(), 0L, "3: A removes");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE,
             "3: a removed SSCT is not found -- and it WAS found immediately"
             " before, two assertions up");
    CHECK_EQ((long)nsfsct_remove(), -1L, "3: removing again finds nothing");

    nsfsct_host_set_jobname("NSFBTEST");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_OK,
             "3: removing A left B ALONE -- removal is by name, not a sweep");
    CHECK_EQ((long)nsfsct_remove(), 0L, "3: B removes too");
    CHECK_EQ((long)nsfsct_host_count(), 0L, "3: registry empty");
}

/* ---- 4. find's verdicts through the plumbing ---------------------------- */
static void
t_find_verdicts(void)
{
    NSFSCT_HOST_ANCHOR bad_ver, foreign, bad_eye;
    void *out = NULL;

    printf("\n--- 4. find verdicts ---\n");
    nsfsct_host_reset();
    nsfsct_host_set_jobname("NSFSTACK");

    /* A version-mismatched anchor: REMOVABLE BUT NOT ADOPTABLE (ADR-0048 3.2).
     * Round 1 asserts only the first half -- that removal works regardless --
     * because the second half is a statement about adoption, which is round 2. */
    mk_anchor(&bad_ver, NSFSCT_EYE, NSFSCT_VER + 1u, "TACK");
    CHECK_EQ((long)nsfsct_register(&bad_ver), 0L, "4: register a stale-version anchor");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_BADVER, "4: find says BADVER");
    CHECK(out == (void *)&bad_ver,
          "4: ...and still hands the address back, without calling it adoptable");
    CHECK_EQ((long)nsfsct_remove(), 0L,
             "4: REMOVAL WORKS ANYWAY -- it goes by name and never dereferences"
             " the anchor");

    /* A foreign anchor -- the CL4 collision reaching find. */
    mk_anchor(&foreign, NSFSCT_EYE, NSFSCT_VER, "TEST");
    CHECK_EQ((long)nsfsct_register(&foreign), 0L, "4: register a foreign tail");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_FOREIGN,
             "4: find says FOREIGN rather than adopting a stranger's anchor");
    CHECK_EQ((long)nsfsct_remove(), 0L, "4: and it is removable");

    /* A corrupt anchor: removal survives it, with the control above (an intact
     * one removed) in the same run. */
    mk_anchor(&bad_eye, "GARBAGE!", NSFSCT_VER, "TACK");
    CHECK_EQ((long)nsfsct_register(&bad_eye), 0L, "4: register a corrupt anchor");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_BADEYE, "4: find says BADEYE");
    CHECK(out == NULL,
          "4: ...and hands back NOTHING -- an anchor that failed its eyecatcher"
          " is not an address a caller may use");
    CHECK_EQ((long)nsfsct_remove(), 0L,
             "4: REMOVAL SURVIVES A CORRUPT ANCHOR -- the property ADR-0048 5.2"
             " answers the lifecycle objection with");
    CHECK_EQ((long)nsfsct_host_count(), 0L, "4: registry empty again");
}

/* ---- 5. no identity, no registration ------------------------------------ */
static void
t_no_identity(void)
{
    NSFSCT_HOST_ANCHOR a;
    void *out = NULL;

    printf("\n--- 5. no jobname ---\n");
    nsfsct_host_reset();
    mk_anchor(&a, NSFSCT_EYE, NSFSCT_VER, "TACK");
    nsfsct_host_set_jobname(NULL);          /* make the TIOT read fail        */

    CHECK_EQ((long)nsfsct_register(&a), -1L,
             "5: no jobname -> no registration (never under an invented name)");
    CHECK_EQ((long)nsfsct_host_count(), 0L, "5: ...and nothing was chained");
    CHECK_EQ((long)nsfsct_find(&out), (long)NSFSCT_NONE, "5: find answers NONE");
    CHECK_EQ((long)nsfsct_remove(), -1L, "5: remove answers failure");

    CHECK_EQ((long)nsfsct_register(NULL), -1L, "5: a NULL anchor is refused");
}

int
main(void)
{
    printf("=== nsf370 d2 round 1: the rendezvous (TSTSCT) ===\n");
    t_split();
    t_classify();
    t_plumbing();
    t_find_verdicts();
    t_no_identity();
    return mbt_test_summary("TSTSCT");
}
