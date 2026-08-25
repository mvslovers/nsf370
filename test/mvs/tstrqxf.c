/*
 * tstrqxf.c -- M5-2b1: the write-out fault tests.
 *
 * MVS-only (host = false): SVC, CSA, storage keys and a second address space
 * have no host analog.
 *
 * TWO SCENARIOS, AND THEY ASK DIFFERENT QUESTIONS.
 *
 *   (A) THE UNREADABLE POINTER -- an OBSERVATION, not a gate.  It hands the
 *       routine a `ubuf` this client cannot even read, and reads the anchor
 *       back afterwards.  It measures the IN direction (see below), which is
 *       the PRE-EXISTING exposure, and it is green on unmodified code too --
 *       it proves the fault is caught cleanly, nothing about M5-2b1.
 *
 *   (B) THE DISCRIMINATING CASE -- the gate that b1 actually earns.  It hands
 *       the routine a `ubuf` in KEY-0, NON-FETCH-PROTECTED storage: the one
 *       pointer class that gets PAST the key-8 read-in and that the old key-0
 *       write-out then clobbered SILENTLY.  Under the M5-2b1 window that store
 *       must take a PROTECTION EXCEPTION.  Remove the window and this case
 *       goes green-store; that asymmetry IS the fix, and it is the only thing
 *       in this file that discriminates.
 *
 * A DIRTY ANCHOR IS AN EXPECTED FINDING IN BOTH, NOT A FAILURE.  M5-2b1 closed
 * the write-out key window, which converts a silent key-0 clobber into a
 * protection exception INSIDE the SVC routine.  Recovery from that is NOT b1's
 * job -- ADR-0039 already names hostile-pointer fault recovery as an open M5-2
 * item, and address validation is (d).  So the CHECKs assert only what must
 * hold regardless -- the client survives, the transport still answers, the
 * probe verbs can clean up -- plus, in (B), the fault itself.  The dangling
 * state is REPORTED (printf + WTO), never asserted.
 *
 * RUN IT LAST, after every other gate, and recycle the STC (P NSFS / S NSFS)
 * before anything else runs.
 *
 * ------------------------------------------------------------------------
 * THE RETURN CODE HAS THREE STATES, NOT TWO.
 *
 *    0                     everything ran and everything passed
 *    1                     the gate RAN and something FAILED (mbt_test_summary)
 *    XF_CC_GATE_SKIPPED    the gate COULD NOT RUN -- (B) was skipped
 *
 * The third one exists because this file becomes the standing regression for
 * b2/b3/b4, and (B) carries b1's entire proof.  (B) skips when the anchor
 * cannot be found or validated, and skipping is the RIGHT behaviour -- asserting
 * against an address that was never validated would be worse than not asserting,
 * and a hard failure would make the test lie when NSFS simply is not started.
 * But a skip that reports CC 0 makes a run with NO PROOF IN IT indistinguishable
 * from a run that proved the window works, and six months from now "TSTRQXF
 * CC 0" would be read as "the window was exercised".
 *
 * So a skip returns its own code.  "The gate could not run" and "the gate ran
 * and failed" are different facts and do not share a code.  The skip code takes
 * PRECEDENCE over the failure code: a skip is always accompanied by the failing
 * CHECK that caused it, so if failure won, the skip code would be unreachable --
 * and of the two facts, "the central claim went unproven" is the one that
 * invalidates the whole run.
 * ------------------------------------------------------------------------
 * WHY (A) CANNOT MEASURE THE WRITE-OUT -- read this before believing its name.
 *
 * The routine reads the caller's `ubuf` on the way IN (RQEIN, an MVCK with
 * source key 8) before it ever writes it on the way OUT (RQEOUT).  Both use
 * the SAME pointer.  So any pointer bad enough to fault the write-out faults
 * the read-in FIRST -- and (A) therefore measures the IN-direction's dangling
 * state, which is the PRE-EXISTING exposure (proven to fault since tstmvck.c
 * scenario 3), not one M5-2b1 created.
 *
 * The one pointer class that gets past the read-in is KEY-0, NON-FETCH-
 * PROTECTED storage: MVCK's source-key check permits a key-8 READ of it (fetch
 * protection is what blocks a fetch, and it is off -- b0 measured ISK X'06' on
 * CSA), while a key-8 STORE into it is denied.  That is exactly the hazard b1
 * closes, and exactly what (B) constructs.
 * ------------------------------------------------------------------------
 * A FAULT ALONE IS NOT EVIDENCE -- (B) MUST ALSO SHOW THE REQUEST DID NOT
 * COMPLETE.
 *
 * "The request took an S0C4" says a fault happened, not WHERE.  That gap is
 * not hypothetical: M5-2b2's first attempt broke the routine so that it
 * faulted immediately after the POST, in code that had nothing to do with the
 * write-out -- and (B) passed, because an S0C4 is an S0C4.  Every gate in the
 * set went green over a broken routine.  Since b2/b3/b4 all lean on this file,
 * (B) now asserts a fault AND that the request is demonstrably unfinished.
 * A fault plus an unfinished request is RQEOUT; a fault alone is nothing.
 *
 * NOT `req_state == DONE`.  It looks like the obvious check and it is wrong:
 * the STC sets DONE asynchronously, so DONE is reached even when the client
 * dies the instant after the POST.  It is evidence the STC ran, never evidence
 * the client got back.
 *
 * TWO SENTINELS, each independently sufficient, both asserted:
 *
 *   rc      the caller's NSFV_REQ.rc is pre-set to XF_RC_SENT.  REPLYC -- and
 *           every bail path -- writes rc.  Still the sentinel => control never
 *           reached REPLYC or any bail, so it died between DOPOST and there,
 *           which is exactly where RQEOUT lives.
 *   rqeimg  the caller's 64-byte NSFRQE image is pre-filled with a positional
 *           pattern.  RQEOUT's SECOND move copies the slot back over it, and
 *           the slot differs from what we sent (the STC wrote retcode/errno_
 *           into it), so a completed write-back MUST change these bytes.
 *           Byte-identical => the write-back never ran.
 *
 * THE THIRD CANDIDATE -- reading the target bytes back -- IS VACUOUS HERE, and
 * the reason is worth stating so nobody adds it thinking it strengthens
 * anything.  The safety property below is that the in-move and the out-move
 * are the reverse of each other inside stage[], so the out-move writes back
 * EXACTLY the bytes the in-move read.  The target is therefore byte-identical
 * whether the store landed or not.  It cannot distinguish the two cases: the
 * very construction that makes (B) safe to run is what makes that check empty.
 * ------------------------------------------------------------------------
 * HOW (B) IS SAFE, WHICH IS THE WHOLE REASON IT CAN EXIST.
 *
 * The obvious way to build this case -- point `ubuf` at some piece of system
 * storage -- is the one thing that must never be done: if the window ever
 * failed to take, the resulting key-0 store would corrupt it.  So (B) points
 * `ubuf` INTO THE ANCHOR'S OWN STAGING BUFFER at a non-zero offset.  Two
 * consequences, both deliberate:
 *
 *   - the only storage at risk is NSF's own scratch, which the next request
 *     overwrites anyway;
 *   - source and destination are then BOTH inside stage[], so the move the
 *     routine performs is stage[OFF..] -> stage[0..] on the way in and exactly
 *     the reverse on the way out.  A window that failed to take produces a
 *     BYTE-IDENTICAL round trip -- a harmless staging-to-staging copy, not a
 *     clobber.  (Which is also why the discriminator is fault-vs-no-fault and
 *     never a content comparison: the content is the same either way.)
 *
 * SELF-VALIDATING, AND IT HAS TO BE.  Three independent facts are established
 * before the request is issued, and no two of them together would do:
 *
 *   1. the anchor eyecatcher reads "NSFVANCR" -- the chase found NSF's anchor
 *      and not an arbitrary address;
 *   2. a key-8 READ of the target SUCCEEDS -- so the page is mapped and not
 *      fetch-protected (any readable address satisfies this alone);
 *   3. a key-8 STORE into the target FAULTS -- so it is store-protected
 *      against this client's key (any unwritable address satisfies this alone).
 *
 * Together they pin the target as key-0-without-fetch-protection, which is
 * what makes the S0C4 that (B) then observes a KEY fault and not the fault of
 * a bad address.  The read-in succeeding inside the routine is the fourth
 * confirmation: it could not have succeeded against fetch-protected storage.
 * ------------------------------------------------------------------------
 * FINDING THE ANCHOR.  An unauthorised client is not given the anchor address,
 * so (B) discovers it the way the routine itself does: the SVC table entry for
 * our stolen SVC holds the routine's CSA entry point, and NSFV_ANCH_OFF bytes
 * into that routine is the word the STC patched with the anchor address.  The
 * chase is READ-ONLY and runs HOP BY HOP under ___try, because "the chase
 * faulted" without knowing WHICH hop faulted is a useless datum -- and whether
 * a problem-state key-8 client can read the SVC table at all is a fact worth
 * recording either way.  NOTE: the equivalent chase in src/nsfv.c and
 * src/nsfsx.c runs __super'd into key 0 (it needs key 0 for the STORE); this
 * one is the first to do it from an UNAUTHORISED client, and it never stores.
 *
 * PREREQUISITE: the NSFS STC must be started (S NSFS).
 * THE RED LINE IS UNCHANGED: the client is UNAUTHORISED and never self-auths.
 */
#include "nsfvsvc.h"
#include "nsfreqx.h"   /* NSFREQX_GUARD -- the per-slot guard word */
#include "nsfreq.h"         /* NSFRQE -- the 64-byte image the request carries */
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                      */
#include <clibtry.h>        /* ___try -- capture the abend, no dump            */
#include <clibwto.h>        /* wtof -- survives a hang, unlike SYSPRINT        */
#include <cvt.h>            /* CVTPTR / cvtabend -- the chase starts at 16     */
#include <ihascvt.h>        /* SCVT (scvtsvct), SVCTABLE, SVCENTRY             */
#include <mbtcheck.h>
#include <string.h>

/* Search for an unreadable address: step upward from our own storage, stop at
 * the first candidate we cannot read.  READ-probe only -- never a store -- so
 * a mapped page belonging to someone else is skipped, not corrupted.  The
 * ceiling stays below the top of the private area (LSQA/SWA) and below CSA,
 * which b0 measured at X'009E0000' on this system. */
#define XF_STEP     0x10000u            /* 64 KB steps                        */
#define XF_CEIL     0x00900000u         /* stop well below LSQA / CSA         */

/* (B) the offset into stage[] the discriminating case aims at, and the number
 * of bytes it moves.  Non-zero so the in-move (stage+OFF -> stage+0) and the
 * out-move (stage+0 -> stage+OFF) touch disjoint halves of the buffer; well
 * clear of both ends of the 2048-byte staging area. */
#define XF_STAGE_OFF  1024u
#define XF_STAGE_LEN  64u

/* An NSFRQE function code no dispatcher case claims, so nsfreq_dispatch takes
 * its `default:` arm and completes NSF_EINVAL.  DELIBERATE: the reply never
 * reaches the client (the routine faults on the way out), so a verb with side
 * effects would strand them -- an RQ_INITAPI would leave an app-registry slot
 * held in NSFS with no token to release it.  Do not "improve" this into a
 * realistic verb. */
#define XF_FN_UNKNOWN 250u

/* The COND CODE for "the load-bearing case could not run" -- see the header.
 * Deliberately NOT 1: mbt_test_summary returns 0/1, so 1 already means "ran and
 * failed".  Returned from the test itself; the mbt harness is not modified. */
#define XF_CC_GATE_SKIPPED 20

/* A byte value to attempt to store during the store-protection pre-check.  It
 * is never expected to land; if the store ever succeeded the target would be
 * NSF's own staging scratch, overwritten by the next request. */
#define XF_POISON     0x5Au

/* (B)'s completion sentinels.  XF_RC_SENT is not 0 and not any NSFV_RC_*
 * (0/4/8/12), so it cannot be confused with a real router return code. */
#define XF_RC_SENT   0x5AB2CC01

static NSFRQE      g_image;             /* a valid 64-byte image to carry     */
static NSFRQE      g_image_ref;         /* (B): the pattern we pre-filled it   */
static NSFV_REQ    g_breq;              /* (B): file scope so main can read it */
                                        /*      AFTER the routine faults       */
static unsigned    g_probe;             /* (A) the candidate under test       */
static char        g_own[64];           /* a known-good address to start from */

static NSFV_ANCHOR *g_anchor;           /* (B) discovered, never given        */
static volatile unsigned char *g_stagep;/* (B) &anchor->stage[XF_STAGE_OFF]   */
static unsigned char g_stage_byte;      /* the byte the read pre-check saw    */
static int           g_stage_rc;        /* set only if the request DID return */

/* Issue the private SVC with R1 = A(req) via the EX-SVC-0 trick (ADR-0038 6);
 * the asm labels are per-file, so they stay unique in the load module. */
static void __attribute__((noinline))
nsfv_svc_issue(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"
        "         LR    6,%1\n"
        "         EX    6,NSFXF0\n"
        "         B     NSFXFX\n"
        "NSFXF0   SVC   0\n"
        "NSFXFX   DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

static void
xf_req_init(NSFV_REQ *req, UINT func)
{
    memset(req, 0, sizeof *req);
    memcpy(req->eye, NSFV_REQ_EYE, 4);
    req->func = func;
    req->rc   = -1;
}

static int
xf_query_slot(UINT idx, UINT *state, UINT *infl, UINT *reap)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_QUERY);
    req.slot = idx;                 /* QUERY names a slot since M5-2b3      */
    nsfv_svc_issue(&req);
    if (state) *state = req.qstate;
    if (infl)  *infl  = req.qinfl;
    if (reap)  *reap  = req.qreap;
    return req.rc;
}

static int
xf_query(UINT *state, UINT *infl, UINT *reap)
{
    return xf_query_slot(0u, state, infl, reap);
}

static int
xf_unstage(void)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_UNSTAGE);
    req.slot = 0u;
    nsfv_svc_issue(&req);
    return req.rc;
}

/* The SLOT probe verb (M5-2b3): CS one named slot from `expect` to `set`.
 * PROBE SCAFFOLDING, due out in M5-2c.  Returns the router rc -- NSFV_RC_OK
 * when the compare took, NSFV_RC_NOREQ when it did not, so a test can assert
 * the pre-claim HAPPENED instead of inferring it. */
static int
xf_slot_cas(UINT idx, UINT expect, UINT set, UINT *replaced)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_SLOT);
    req.slot    = idx;
    req.sexpect = expect;
    req.snew    = set;
    nsfv_svc_issue(&req);
    if (replaced) *replaced = req.qstate;
    return req.rc;
}

/* --- bodies run under ___try -------------------------------------------- */

/* Read one byte of the (A) candidate.  Faults for an unmapped page and for
 * fetch-protected storage alike; either answer is fine, because both make the
 * SVC routine's key-8 read-in fault before it can store anything. */
static int
t_read_probe(void)
{
    volatile unsigned char *p = (volatile unsigned char *)g_probe;
    unsigned char           v;

    v = *p;
    return (int)v & 0;                  /* keep the read, discard the value   */
}

/* Issue one ordinary RQE request and report WHICH SLOT the routine claimed.
 * The image is a dispatchable NSFRQE with an unknown fn, so the STC completes
 * it with an error rather than doing any real work -- what is under test here
 * is the CLAIM, which happens before the routine ever looks at the verb. */
static int
xf_rqe_slot(UINT *slot_used)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_RQE);
    req.ubuf   = NULL;
    req.ulen   = 0u;
    req.rqeimg = (void *)&g_image;
    req.slot   = 0xFFFFu;               /* so an unwritten field is obvious   */
    nsfv_svc_issue(&req);
    if (slot_used) *slot_used = req.slot;
    return req.rc;
}

/* (A) The whole point: hand the routine a pointer this client cannot touch. */
static int
t_bad_ubuf(void)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_RQE);
    req.ubuf   = (void *)g_probe;
    req.ulen   = 64u;
    req.rqeimg = (void *)&g_image;
    nsfv_svc_issue(&req);
    return 0;                           /* not reached: the routine faults    */
}

/* --- (B) the chase, one hop per ___try ----------------------------------- */

static void *g_hop_in;
static void *g_hop_out;
static char  g_eye[8];

static int t_hop_cvt(void)
{
    g_hop_out = (void *)CVTPTR;                     /* the read of location 16 */
    return 0;
}
static int t_hop_scvt(void)
{
    g_hop_out = ((CVT *)g_hop_in)->cvtabend;
    return 0;
}
static int t_hop_svct(void)
{
    g_hop_out = ((SCVT *)g_hop_in)->scvtsvct;
    return 0;
}
static int t_hop_epa(void)
{
    g_hop_out = ((SVCTABLE *)g_hop_in)->svcentry[NSFV_SVCNUM].svcepa;
    return 0;
}
static int t_hop_anch(void)
{
    g_hop_out = *(void **)((unsigned char *)g_hop_in + NSFV_ANCH_OFF);
    return 0;
}
static int t_hop_eye(void)
{
    memcpy(g_eye, ((NSFV_ANCHOR *)g_hop_in)->eye, sizeof g_eye);
    return 0;
}

/* Run one hop, report it either way, and say WHICH hop faulted. */
static void *
xf_hop(const char *name, int (*body)(void), void *in, int *failed)
{
    int rc;

    if (*failed) return NULL;
    g_hop_in  = in;
    g_hop_out = NULL;
    rc = ___try(body);
    if (rc != 0) {
        printf("  chase %-6s FAULTED (try rc=%08X)\n", name, (unsigned)rc);
        wtof("TSTRQXF: chase %s FAULTED rc=%08X", name, (unsigned)rc);
        *failed = 1;
        return NULL;
    }
    printf("  chase %-6s -> %08X\n", name, (unsigned)g_hop_out);
    return g_hop_out;
}

/* (C) read the five self-check words the routine leaves in the dead ANCSAVE.
 *
 * THREE STATES, and the third is why this is not a bool.  1 = the check ran and
 * passed, 2 = it ran and FAILED, 0 = it was NEVER WRITTEN.  With a 1/0 pair a
 * word that was never reached reads exactly like one that ran and failed, and
 * those are different faults with different next steps: "the sentinel was
 * dead" sends you at the save area, "control never got there" sends you at the
 * path.  Same contract as this file's CC 20 skip code, applied to the
 * routine's own evidence.  The assertions want 1; the PRINTED line is what
 * makes 1/2/1/1 and 1/0/1/1 distinguishable to a human reading the spool. */
static UINT g_pool_ver;
static UINT g_pool_n;
static UINT g_pool_guards;      /* slots whose guard word is stamped        */

/* (C) read the pool's published shape out of CSA, key 8, read-only -- exactly
 * as (B) already proved works on this block. */
static int
t_read_pool(void)
{
    const volatile NSFV_ANCHOR *a = (const volatile NSFV_ANCHOR *)g_anchor;
    UINT i;

    g_pool_ver    = a->version;
    g_pool_n      = a->nslots;
    g_pool_guards = 0;
    for (i = 0; i < g_pool_n && i < NSFV_NSLOTS; i++) {
        if (memcmp((const void *)g_anchor->slots[i].rqe_guard,
                   NSFREQX_GUARD, NSFREQX_GUARDLEN) == 0) {
            g_pool_guards++;
        }
    }
    return 0;
}

/* (B) pre-check 1: the target must be READABLE from the client's own key --
 * otherwise the routine's key-8 read-in would fault and we would be measuring
 * (A) all over again. */
static int
t_stage_read(void)
{
    g_stage_byte = g_stagep[0];
    return 0;
}

/* (B) pre-check 2: the target must NOT be STORABLE from the client's own key.
 * This is the load-bearing half: it is what makes the S0C4 the request later
 * takes a KEY fault rather than the fault of a bad address. */
static int
t_stage_write(void)
{
    g_stagep[0] = (unsigned char)XF_POISON;
    return 0;
}

/* (B) The discriminating request: a ubuf in key-0, non-fetch-protected storage.
 * The read-in succeeds (key-8 fetch is permitted), the STC services the
 * request, and the write-out must fault under the M5-2b1 window. */
static int
t_stage_ubuf(void)
{
    nsfv_svc_issue(&g_breq);            /* set up by main, so it OUTLIVES this */
    g_stage_rc = g_breq.rc;             /* reached ONLY without the window     */
    return 0;
}

/* The ONE exit.  Prints the summary as usual, then decides the code. */
static int
xf_finish(int skipped)
{
    int rc = mbt_test_summary("TSTRQXF");

    if (skipped) {
        printf("*** (B) DID NOT RUN.  This run contains NO evidence about the"
               " write-out key\n");
        printf("*** window.  Returning CC %d rather than %d so it cannot be"
               " mistaken for a\n", XF_CC_GATE_SKIPPED, rc);
        printf("*** clean pass -- see the header.\n");
        wtof("TSTRQXF: GATE SKIPPED -- CC %d, NOT a pass",
             XF_CC_GATE_SKIPPED);
        return XF_CC_GATE_SKIPPED;
    }
    return rc;
}

int
main(void)
{
    UINT     st0, in0, rp0, st1, in1, rp1, st2, in2, rp2;
    UINT     st3, in3, rp3, st4, in4, rp4;
    int      rc, found, failed;
    unsigned addr;
    void    *cvt, *scvt, *svct, *epa, *anch;

    wtof("TSTRQXF: WRITE-OUT FAULT TESTS START");
    printf("=== TSTRQXF -- M5-2b1: the write-out fault tests ===\n");

    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    /* ---- baseline ------------------------------------------------------ */
    rc = xf_query(&st0, &in0, &rp0);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers (the STC is up)");
    printf("  before: state=%u inflight=%u reaped=%u\n",
           (unsigned)st0, (unsigned)in0, (unsigned)rp0);
    CHECK_EQ((long)st0, (long)NSFV_REQ_FREE,
             "the slot is FREE at entry (no other client mid-request)");
    if (in0 != 0u) {
        printf("  NOTE: entry in-flight is already %u -- a previous run of this"
               " test leaked it (see the findings below)\n", (unsigned)in0);
    }

    memset(g_own, 0, sizeof g_own);
    memset(&g_image, 0, sizeof g_image);
    memcpy(g_image.eye, "RQE ", 4);
    g_image.fn = (USHORT)XF_FN_UNKNOWN;

    /* ==================================================================== *
     * (A) THE UNREADABLE POINTER -- observation only.  Green on unmodified
     *     code too: it measures the pre-existing IN-direction exposure.
     * ==================================================================== */
    printf("\n--- (A) unreadable ubuf: the IN direction (pre-existing) ---\n");

    found = 0;
    addr  = ((unsigned)(void *)g_own + XF_STEP) & ~(XF_STEP - 1u);
    for (; addr < XF_CEIL; addr += XF_STEP) {
        g_probe = addr;
        if (___try(t_read_probe) != 0) { found = 1; break; }
    }
    CHECK(found != 0, "found an address this client cannot read");
    if (!found) {
        wtof("TSTRQXF: no unreadable address below %08X -- (A) skipped",
             XF_CEIL);
        printf("  SKIPPED: no unreadable address found below %08X\n", XF_CEIL);
    } else {
        printf("  probe address = %08X (read faults from the client's own"
               " key)\n", g_probe);
        wtof("TSTRQXF: probe addr=%08X", g_probe);

        rc = ___try(t_bad_ubuf);
        printf("  bad-ubuf request: try rc=%08X\n", (unsigned)rc);
        wtof("TSTRQXF: bad-ubuf rc=%08X", (unsigned)rc);
        CHECK(rc != 0,
              "the request faulted rather than completing on a bad pointer");
        if (rc > 0) {
            printf("  the fault is S%03X (a protection or translation"
                   " exception)\n", ((unsigned)rc >> 12) & 0xFFFu);
        }
        CHECK(rc >= 0,
              "the fault was CAUGHT (ESTAE created) -- no dump, client alive");

        /* THE OBSERVATION: what did it leave behind?
         * PREDICTION, recorded so the run can falsify it: the read-in faults
         * BEFORE the slot is published, so state should still be FREE while
         * inflight is stuck at 1.  Reported, not asserted. */
        rc = xf_query(&st1, &in1, &rp1);
        CHECK_EQ((long)rc, (long)NSFV_RC_OK,
                 "the transport still answers after the (A) fault");
        printf("  after:  state=%u inflight=%u reaped=%u\n",
               (unsigned)st1, (unsigned)in1, (unsigned)rp1);
        wtof("TSTRQXF: (A) after fault state=%u inflight=%u",
             (unsigned)st1, (unsigned)in1);
        /* M5-2b3 CHANGED THIS OUTCOME, so the prediction is restated rather
         * than left stale.  Before the pool a client that faulted in the
         * write-IN direction left its slot FREE (it had not published yet) and
         * inflight leaked.  With the pool the claim is a CS to CLAIMED, so the
         * slot is now stuck CLAIMED -- and a CLAIMED slot carries no identity,
         * so the death guard can never reclaim it.  That is exactly the
         * consequence ADR-0042 states and nsfvsvc.h documents next to
         * NSFV_REQ_CLAIMED; this line is where it is MEASURED rather than
         * predicted.  Reported, not asserted: fault recovery is still open. */
        printf("  FINDING: slot %s, inflight %s (b3 predicts CLAIMED=%u /"
               " stuck at %u)\n",
               (st1 == NSFV_REQ_CLAIMED) ? "CLAIMED (claimed, never published)"
                                         : ((st1 == NSFV_REQ_FREE) ? "FREE"
                                                                   : "other"),
               (in1 > in0) ? "LEAKED" : "clean",
               (unsigned)NSFV_REQ_CLAIMED, (unsigned)(in0 + 1u));

        /* Can the probe verbs clean up after it?  Reported, not asserted --
         * only "UNSTAGE is accepted and the slot ends FREE" is an invariant. */
        rc = xf_unstage();
        CHECK_EQ((long)rc, (long)NSFV_RC_OK, "UNSTAGE accepted after (A)");
        rc = xf_query(&st2, &in2, &rp2);
        CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers after UNSTAGE");
        printf("  after UNSTAGE: state=%u inflight=%u reaped=%u\n",
               (unsigned)st2, (unsigned)in2, (unsigned)rp2);
        CHECK_EQ((long)st2, (long)NSFV_REQ_FREE, "the slot is FREE");

        if (in2 > in0) {
            printf("  FINDING: UNSTAGE did NOT recover the in-flight count"
                   " (%u -> %u).\n", (unsigned)in0, (unsigned)in2);
            printf("           It gives a count back only when the slot was"
                   " PUBLISHED, and this\n");
            printf("           fault happens BEFORE publication -- so the"
                   " probe's own cleanup verb\n");
            printf("           cannot reach a pre-publication leak.  Reported"
                   " for b2/(c), not fixed here.\n");
            wtof("TSTRQXF: FINDING inflight %u->%u, UNSTAGE cannot recover it",
                 (unsigned)in0, (unsigned)in2);
        } else {
            printf("  the in-flight count came back clean\n");
        }
    }

    /* ==================================================================== *
     * (B) THE DISCRIMINATING CASE -- the gate M5-2b1 earns.  RUN LAST.
     * ==================================================================== */
    printf("\n--- (B) key-0 non-fetch-protected ubuf: the OUT direction ---\n");

    /* --- discover the anchor, hop by hop --- */
    failed = 0;
    cvt  = xf_hop("CVT",   t_hop_cvt,  NULL, &failed);
    scvt = xf_hop("SCVT",  t_hop_scvt, cvt,  &failed);
    svct = xf_hop("SVCT",  t_hop_svct, scvt, &failed);
    epa  = xf_hop("EPA",   t_hop_epa,  svct, &failed);
    if (!failed) {
        /* 24-bit machine: mask the SVC-table entry-point word before using it
         * as an address, so a flag byte can never become part of it. */
        epa = (void *)((unsigned)epa & 0x00FFFFFFu);
        printf("  chase EP     -> %08X (SVC %u routine, in CSA)\n",
               (unsigned)epa, (unsigned)NSFV_SVCNUM);
    }
    anch = xf_hop("ANCHOR", t_hop_anch, epa, &failed);

    /* One assertion over both ways this can go wrong: a faulting hop, and a
     * chase that completed every hop but read back a null anchor word (no STC
     * has published one).  The second used to pass this CHECK and then skip
     * with CC 0 -- the silent case the return-code contract closes. */
    CHECK(failed == 0 && anch != NULL,
          "an UNAUTHORISED client can chase CVT -> SCVT -> SVCTABLE -> the"
          " routine EP -> the anchor word");

    if (failed || anch == NULL) {
        printf("  (B) SKIPPED: the anchor chase did not complete\n");
        wtof("TSTRQXF: (B) SKIPPED -- chase incomplete");
        wtof("TSTRQXF: WRITE-OUT FAULT TESTS DONE (B skipped)");
        return xf_finish(1);
    }

    /* Confirmation 1: this really is NSF's anchor, not an arbitrary address. */
    memset(g_eye, 0, sizeof g_eye);
    failed = 0;
    (void)xf_hop("EYE", t_hop_eye, anch, &failed);
    CHECK(failed == 0, "the anchor's eyecatcher is readable from key 8");
    CHECK(memcmp(g_eye, "NSFVANCR", 8) == 0,
          "the chase found NSF's anchor (eyecatcher \"NSFVANCR\")");
    if (failed || memcmp(g_eye, "NSFVANCR", 8) != 0) {
        printf("  (B) SKIPPED: %08X does not carry the anchor eyecatcher\n",
               (unsigned)anch);
        wtof("TSTRQXF: (B) SKIPPED -- no anchor eyecatcher at %08X",
             (unsigned)anch);
        wtof("TSTRQXF: WRITE-OUT FAULT TESTS DONE (B skipped)");
        return xf_finish(1);
    }

    g_anchor = (NSFV_ANCHOR *)anch;
    /* Slot 0's staging (M5-2b3): still NSF's OWN scratch, which is what makes
     * pointing ubuf at it safe -- a window that failed to take performs a
     * byte-identical round trip inside stage[] rather than clobbering anything
     * that matters. */
    g_stagep = (volatile unsigned char *)&g_anchor->slots[0].stage[XF_STAGE_OFF];
    printf("  anchor = %08X, target = &stage[%u] = %08X, len = %u\n",
           (unsigned)g_anchor, (unsigned)XF_STAGE_OFF, (unsigned)g_stagep,
           (unsigned)XF_STAGE_LEN);
    wtof("TSTRQXF: (B) anchor=%08X target=%08X", (unsigned)g_anchor,
         (unsigned)g_stagep);

    /* Confirmation 2: readable from our own key (mapped, not fetch-protected).
     * Any readable address satisfies this ALONE -- it is confirmation 3 that
     * makes the pair mean "key 0". */
    rc = ___try(t_stage_read);
    printf("  key-8 READ of the target: try rc=%08X (byte %02X)\n",
           (unsigned)rc, (unsigned)g_stage_byte);
    CHECK_EQ((long)rc, 0L,
             "a key-8 READ of the target SUCCEEDS (mapped, not"
             " fetch-protected)");

    /* Confirmation 3: NOT storable from our own key.  The load-bearing half --
     * without it, an S0C4 from the request below could be a bad address.  Any
     * unwritable address satisfies this ALONE; together with 2 it pins the
     * target as key-0 storage with fetch protection off, which is precisely
     * the class the old key-0 write-out clobbered silently. */
    rc = ___try(t_stage_write);
    printf("  key-8 STORE into the target: try rc=%08X\n", (unsigned)rc);
    CHECK(rc != 0,
          "a key-8 STORE into the target FAULTS (it is key-0 storage)");
    if (rc > 0) {
        printf("  the store fault is S%03X\n", ((unsigned)rc >> 12) & 0xFFFu);
    }

    rc = xf_query(&st3, &in3, &rp3);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers before (B)");
    CHECK_EQ((long)st3, (long)NSFV_REQ_FREE, "the slot is FREE before (B)");
    printf("  before (B): state=%u inflight=%u reaped=%u\n",
           (unsigned)st3, (unsigned)in3, (unsigned)rp3);

    /* --- the case itself ---
     * The read-in MUST succeed (key-8 fetch of fetch-unprotected storage), the
     * STC services the request, and the write-out must fault under the window.
     * WITHOUT the window the store succeeds and the request returns rc=0 --
     * having performed a byte-identical staging-to-staging round trip. */
    /* Pre-fill both sentinels.  The image pattern is POSITIONAL (byte i
     * depends on i), so a partial write-back is detectable and not just a
     * whole-buffer compare that a memset-like clobber could also pass. */
    {
        unsigned char *ip = (unsigned char *)&g_image;
        unsigned       i;

        for (i = 0; i < sizeof g_image; i++) {
            ip[i] = (unsigned char)(0xA0u + (i & 0x0Fu));
        }
        memcpy(g_image.eye, "RQE ", 4);          /* still a dispatchable RQE */
        g_image.fn = (USHORT)XF_FN_UNKNOWN;      /* -> EINVAL, no side effect */
        memcpy(&g_image_ref, &g_image, sizeof g_image);
    }
    xf_req_init(&g_breq, NSFV_REQ_RQE);
    g_breq.ubuf   = (void *)g_stagep;
    g_breq.ulen   = XF_STAGE_LEN;
    g_breq.rqeimg = (void *)&g_image;
    g_breq.rc     = XF_RC_SENT;         /* REPLYC and every bail overwrite it */

    g_stage_rc = 0x7FFFFFFF;
    rc = ___try(t_stage_ubuf);
    printf("  key-0-ubuf request: try rc=%08X\n", (unsigned)rc);
    wtof("TSTRQXF: (B) request rc=%08X", (unsigned)rc);
    if (rc == 0) {
        printf("  *** NO FAULT: the request RETURNED, req.rc=%d -- the"
               " write-out\n", g_stage_rc);
        printf("  *** stored into key-0 storage under the routine's own key."
               " This is\n");
        printf("  *** exactly the state M5-2b1 removes.\n");
    } else {
        printf("  the fault is S%03X\n", ((unsigned)rc >> 12) & 0xFFFu);
    }
    CHECK(rc != 0,
          "THE GATE (1/3): the key-0 write-out FAULTS -- the M5-2b1 SPKA window"
          " checks the caller-supplied destination against the caller's key");
    CHECK(rc >= 0,
          "the fault was CAUGHT (ESTAE created) -- no dump, client alive");

    /* --- the fault was in the WRITE-OUT, not merely somewhere ---
     * Without these two, an S0C4 from anywhere at all satisfies the gate --
     * which is how M5-2b2's broken routine passed it. */
    printf("  rc sentinel: %08X (pre-set %08X) | image %s\n",
           (unsigned)g_breq.rc, (unsigned)XF_RC_SENT,
           (memcmp(&g_image, &g_image_ref, sizeof g_image) == 0)
               ? "byte-identical" : "MODIFIED");
    wtof("TSTRQXF: (B) rc=%08X imagechg=%d", (unsigned)g_breq.rc,
         (memcmp(&g_image, &g_image_ref, sizeof g_image) != 0));
    CHECK_EQ((long)g_breq.rc, (long)XF_RC_SENT,
             "THE GATE (2/3): rc is untouched -- the routine never reached"
             " REPLYC or any bail path, so it died in between");
    CHECK(memcmp(&g_image, &g_image_ref, sizeof g_image) == 0,
          "THE GATE (3/3): the caller's NSFRQE image is byte-identical -- the"
          " write-back never ran");

    /* --- what did the OUT direction leave behind? ---
     * PREDICTION (b1 reasoned it from a branchless path; here it is measured):
     * the fault is INSIDE RQEOUT, after the STC serviced and replied, so
     * req_state should be stuck at DONE -- the slot busy forever, every later
     * request RCNOREQ -- with inflight leaked.  Worse than (A)'s FREE.
     * Reported, never asserted: recovery is the open M5-2 item. */
    rc = xf_query(&st4, &in4, &rp4);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK,
             "the transport still answers after the (B) fault (not wedged)");
    printf("  after (B): state=%u inflight=%u reaped=%u\n",
           (unsigned)st4, (unsigned)in4, (unsigned)rp4);
    wtof("TSTRQXF: (B) after fault state=%u inflight=%u",
         (unsigned)st4, (unsigned)in4);
    printf("  FINDING: slot %s (predicted DONE=%u), inflight %s\n",
           (st4 == NSFV_REQ_DONE) ? "DONE (published, never released)"
                                  : "not DONE",
           (unsigned)NSFV_REQ_DONE,
           (in4 > in3) ? "LEAKED" : "clean");

    /* Unlike (A), the slot here is NOT free -- so UNSTAGE can reach it. */
    rc = xf_unstage();
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "UNSTAGE accepted after (B)");
    rc = xf_query(&st4, &in4, &rp4);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers after UNSTAGE");
    printf("  after UNSTAGE: state=%u inflight=%u reaped=%u\n",
           (unsigned)st4, (unsigned)in4, (unsigned)rp4);
    CHECK_EQ((long)st4, (long)NSFV_REQ_FREE,
             "the slot is FREE again (UNSTAGE reaches a PUBLISHED slot)");

    /* ==================================================================== *
     * (C) M5-2b3: THE SLOT POOL IS REAL.  A POSITIVE check.
     *
     * This part used to read back b2's five save-area self-check words.  That
     * evidence has been collected -- the SVRB's RBEXSAVE is a genuine
     * per-invocation area, a canary survives both the POST and the WAIT -- it
     * is recorded in ADR-0038, and carrying five stores and three CLCs per
     * request forever to re-prove it was not worth the hot path (issue #61).
     *
     * So the part is CONVERTED, not deleted: the same SHAPE of evidence
     * (stamp something, read it back, prove it took) aimed at what is now
     * unproven.  Every gate in the set runs through the claim, so a pool that
     * is subtly wrong -- a stride the assembler and C disagree about -- would
     * pass all of them while the routine wrote into the wrong slot.
     *
     * THE STRIDE CROSS-CHECK IS THE POINT.  The C side computes &slots[i] from
     * sizeof(NSFV_SLOT); the assembler walks LA Rn,SLOTLEN(,Rn).  Those are two
     * hand-maintained numbers in two languages, and NOTHING ELSE IN THE SUITE
     * COMPARES THEM -- a size assert cannot, because both sides would still be
     * internally consistent.  Here the routine is asked to change slot i and
     * the test reads slot i back through the C struct: they agree, or the two
     * strides differ and this is the only place that says so.
     * ==================================================================== */
    printf("\n--- (C) M5-2b3: the slot pool is real ---\n");

    failed = 0;
    (void)xf_hop("POOL", t_read_pool, NULL, &failed);
    CHECK(failed == 0, "the pool header is readable from key 8");
    printf("  version = %u (expected %u)\n",
           (unsigned)g_pool_ver, (unsigned)NSFV_ANCHOR_VER);
    printf("  nslots  = %u (expected %u)\n",
           (unsigned)g_pool_n, (unsigned)NSFV_NSLOTS);
    printf("  slots with a stamped guard word: %u of %u\n",
           (unsigned)g_pool_guards, (unsigned)g_pool_n);
    wtof("TSTRQXF: (C) ver=%u nslots=%u guards=%u",
         (unsigned)g_pool_ver, (unsigned)g_pool_n, (unsigned)g_pool_guards);

    CHECK_EQ((long)g_pool_ver, (long)NSFV_ANCHOR_VER,
             "the anchor carries the layout version the routine checks");
    CHECK_EQ((long)g_pool_n, (long)NSFV_NSLOTS,
             "the allocator published the slot count the scan is bounded by");
    /* Not "the first one" and not "at least one": ALL of them.  A wrong slot
     * stride on the C side would leave guards landing at addresses the loop
     * does not look at, so this is a statement about the whole array. */
    CHECK_EQ((long)g_pool_guards, (long)NSFV_NSLOTS,
             "EVERY slot's guard word is stamped (the array is really 64 slots"
             " at the stride C computes)");

    /* ---- the stride cross-check: does the ASSEMBLER agree with C? -------
     * Ask the routine to move a named slot FREE -> HELD, then read that slot
     * back through the C struct.  If the assembler's SLOTLEN differed from
     * sizeof(NSFV_SLOT), the routine would have changed a DIFFERENT address
     * and the read-back would still say FREE.
     *
     * Slot 3, not slot 0: at slot 0 the two strides cannot disagree, because
     * index 0 needs no walk at all.  A test that only ever touched slot 0
     * would pass with ANY stride -- which is the whole failure this check
     * exists to catch. */
    {
        UINT before = 99u, after = 99u, replaced = 99u;
        int  crc;

        (void)xf_query_slot(3u, &before, NULL, NULL);
        CHECK_EQ((long)before, (long)NSFV_REQ_FREE,
                 "stride check: slot 3 starts FREE");

        crc = xf_slot_cas(3u, NSFV_REQ_FREE, NSFV_REQ_HELD, &replaced);
        /* Assert the pre-set TOOK.  The verb is a CS precisely so this is an
         * observation and not an inference (CLAUDE.md 8.5). */
        CHECK_EQ((long)crc, (long)NSFV_RC_OK,
                 "stride check: the SLOT verb's compare-and-swap took");
        CHECK_EQ((long)replaced, (long)NSFV_REQ_FREE,
                 "stride check: it replaced the state we predicted");

        after = g_anchor->slots[3].req_state;      /* read through the C struct */
        printf("  slot 3 via C after the routine set it: %u (want %u)\n",
               (unsigned)after, (unsigned)NSFV_REQ_HELD);
        CHECK_EQ((long)after, (long)NSFV_REQ_HELD,
                 "STRIDE CROSS-CHECK: the assembler's SLOTLEN walk and C's"
                 " &slots[3] are the same address");

        /* And put it back, so the pool is left exactly as it was found. */
        crc = xf_slot_cas(3u, NSFV_REQ_HELD, NSFV_REQ_FREE, NULL);
        CHECK_EQ((long)crc, (long)NSFV_RC_OK, "stride check: slot 3 restored");
        CHECK_EQ((long)g_anchor->slots[3].req_state, (long)NSFV_REQ_FREE,
                 "stride check: slot 3 reads FREE again");
    }

    /* ---- the range check rejects instead of computing an address -------- */
    {
        UINT dummy = 0;
        int  brc   = xf_query_slot(NSFV_NSLOTS, &dummy, NULL, NULL);

        CHECK_EQ((long)brc, (long)NSFV_RC_INVALID,
                 "an index AT nslots is rejected (not an off-by-one into the"
                 " word past the array)");
        brc = xf_query_slot(0xFFFFu, &dummy, NULL, NULL);
        CHECK_EQ((long)brc, (long)NSFV_RC_INVALID,
                 "a wild index is rejected without computing an address");
    }

    /* ==================================================================== *
     * (D) M5-2b3: the three pool checks (ADR-0042 7).
     *
     * REUSE, SKIP, EXHAUSTION.  All three are statements about the claim scan,
     * which runs before the routine looks at the verb, so an ordinary RQE
     * request exercises it regardless of what the STC then does with it.
     *
     * What these do NOT prove, and the report says so: that two clients in two
     * address spaces racing on the SAME slot word resolve correctly.  The CS
     * makes that true by construction, and construction is not a live gate.
     * That is b4.
     * ==================================================================== */
    printf("\n--- (D) M5-2b3: reuse, skip, exhaustion ---\n");

    /* ---- REUSE: a released slot really goes back to the pool ------------- */
    {
        UINT used[4];
        UINT k;
        int  rrc;
        int  same = 1;

        for (k = 0; k < 4; k++) {
            used[k] = 0xFFFFu;
            rrc = xf_rqe_slot(&used[k]);
            CHECK(rrc == NSFV_RC_OK, "reuse: the request was served");
        }
        printf("  slots used by 4 sequential requests: %u %u %u %u\n",
               (unsigned)used[0], (unsigned)used[1],
               (unsigned)used[2], (unsigned)used[3]);
        wtof("TSTRQXF: (D) reuse slots %u/%u/%u/%u",
             (unsigned)used[0], (unsigned)used[1],
             (unsigned)used[2], (unsigned)used[3]);

        for (k = 0; k < 4; k++) {
            if (used[k] != used[0]) same = 0;
        }
        /* The scan takes the LOWEST free slot, so a slot that is genuinely
         * released comes straight back. If release were broken, each request
         * would walk one further and these would read 0,1,2,3 -- which is
         * exactly the failure this distinguishes. */
        CHECK(same,
              "REUSE: all four sequential requests got the SAME slot back"
              " (0,1,2,3 would mean release is broken)");
        CHECK_EQ((long)used[0], 0L, "reuse: and it is the lowest slot");
    }

    /* ---- SKIP: pre-claimed slots are stepped over, not overwritten ------- */
    {
        UINT used = 0xFFFFu;
        UINT k;
        int  claimed = 0;

        /* Pre-claim 0..4. Each CS is ASSERTED, so a miscount shows up here
         * rather than as a mysterious pool bug three checks later. */
        for (k = 0; k < 5; k++) {
            if (xf_slot_cas(k, NSFV_REQ_FREE, NSFV_REQ_CLAIMED, NULL)
                == NSFV_RC_OK) claimed++;
        }
        CHECK_EQ((long)claimed, 5L, "skip: five slots were really pre-claimed");

        (void)xf_rqe_slot(&used);
        printf("  with slots 0-4 pre-claimed, the request landed on slot %u\n",
               (unsigned)used);
        wtof("TSTRQXF: (D) skip -> slot %u (want 5)", (unsigned)used);
        /* THIS is what proves the scan is a scan and not a constant. */
        CHECK_EQ((long)used, 5L,
                 "SKIP: the scan stepped over exactly the pre-claimed slots"
                 " and landed on the next free one");

        for (k = 0; k < 5; k++) {
            (void)xf_slot_cas(k, NSFV_REQ_CLAIMED, NSFV_REQ_FREE, NULL);
        }
        CHECK_EQ((long)g_anchor->slots[0].req_state, (long)NSFV_REQ_FREE,
                 "skip: the pre-claimed slots were released again");
    }

    /* ---- EXHAUSTION: a full pool answers ENOBUFS and changes nothing ----- */
    {
        UINT before_infl = 0, after_infl = 0;
        UINT exh_before = 0, exh_after = 0;
        UINT used = 0xFFFFu;
        UINT k;
        UINT claimed = 0;
        int  erc;

        (void)xf_query_slot(0u, NULL, &before_infl, NULL);

        for (k = 0; k < NSFV_NSLOTS; k++) {
            if (xf_slot_cas(k, NSFV_REQ_FREE, NSFV_REQ_CLAIMED, NULL)
                == NSFV_RC_OK) claimed++;
        }
        CHECK_EQ((long)claimed, (long)NSFV_NSLOTS,
                 "exhaustion: every one of the 64 slots was pre-claimed");

        exh_before = g_anchor->exhausted;
        erc = xf_rqe_slot(&used);
        exh_after = g_anchor->exhausted;
        printf("  with the pool full: rc=%d (want %d = NOBUF)\n",
               erc, (int)NSFV_RC_NOBUF);
        printf("  exhausted counter: %u -> %u\n",
               (unsigned)exh_before, (unsigned)exh_after);
        wtof("TSTRQXF: (D) exhaustion rc=%d exh %u->%u", erc,
             (unsigned)exh_before, (unsigned)exh_after);
        /* The counter exists so a full pool is a SIZING FACT rather than
         * something inferred from client-side errnos -- so it has to be read
         * by something, or it is a diagnostic whose absence is
         * indistinguishable from its success (CLAUDE.md 8.5). */
        CHECK_EQ((long)(exh_after - exh_before), 1L,
                 "exhaustion: the anchor's `exhausted` counter ticked exactly"
                 " once");
        CHECK_EQ((long)erc, (long)NSFV_RC_NOBUF,
                 "EXHAUSTION: a full pool answers NOBUF (-> ENOBUFS), not a"
                 " hang and not a wrong slot");

        (void)xf_query_slot(0u, NULL, &after_infl, NULL);
        printf("  inflight before=%u after=%u\n",
               (unsigned)before_infl, (unsigned)after_infl);
        /* The rejection happens BEFORE the in-flight increment, so a caller
         * that got no slot must leave the count exactly as it found it. */
        CHECK_EQ((long)after_infl, (long)before_infl,
                 "exhaustion: the rejected request leaked no in-flight count");

        /* And it must have left every slot as it found it -- still CLAIMED by
         * us, not quietly stolen. */
        {
            UINT still = 0;
            for (k = 0; k < NSFV_NSLOTS; k++) {
                if (g_anchor->slots[k].req_state == NSFV_REQ_CLAIMED) still++;
            }
            CHECK_EQ((long)still, (long)NSFV_NSLOTS,
                     "exhaustion: every slot is still exactly as it was found");
        }

        for (k = 0; k < NSFV_NSLOTS; k++) {
            (void)xf_slot_cas(k, NSFV_REQ_CLAIMED, NSFV_REQ_FREE, NULL);
        }
        /* Leave the pool as we found it, or every later gate in the round
         * inherits a full pool and fails for the wrong reason. */
        {
            UINT free_now = 0;
            for (k = 0; k < NSFV_NSLOTS; k++) {
                if (g_anchor->slots[k].req_state == NSFV_REQ_FREE) free_now++;
            }
            CHECK_EQ((long)free_now, (long)NSFV_NSLOTS,
                     "exhaustion: the whole pool was released again");
        }
    }

    /* ---- the transport still works end to end -------------------------- */
    rc = xf_query(&st2, &in2, &rp2);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "the transport survived the whole run");

    wtof("TSTRQXF: WRITE-OUT FAULT TESTS DONE");
    return xf_finish(0);
}
