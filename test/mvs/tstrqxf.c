/*
 * tstrqxf.c -- M5-2b1 OBSERVATION test: what does a faulting write-out leave
 *              behind in the anchor?
 *
 * MVS-only (host = false): SVC, CSA, storage keys and a second address space
 * have no host analog.
 *
 * THIS TEST ANSWERS A QUESTION; IT DOES NOT GUARD A CONTRACT.  M5-2b1 closed
 * the write-out key window, which converts what used to be a silent key-0
 * clobber into a PROTECTION EXCEPTION INSIDE THE SVC ROUTINE.  Recovery from
 * that is NOT b1's job -- ADR-0039 already names hostile-pointer fault recovery
 * as an open M5-2 item, and address validation is (d).  What b1 owes is an
 * assessment of the exposure, and the honest way to get one is to induce the
 * fault on the machine and read the anchor afterwards.
 *
 * SO: A DIRTY ANCHOR HERE IS AN EXPECTED FINDING, NOT A FAILURE.  The CHECKs
 * below assert only what must hold regardless -- the client survives, the
 * transport still answers, and the probe verbs can clean up after it.  The
 * dangling state itself is REPORTED (printf + WTO), never asserted.
 *
 * RUN IT LAST, after every other gate, and recycle the STC (P NSFS / S NSFS)
 * before anything else runs.
 *
 * ------------------------------------------------------------------------
 * WHICH DIRECTION THIS ACTUALLY FAULTS -- read this before believing the name.
 *
 * The routine reads the caller's `ubuf` on the way IN (RQEIN, an MVCK with
 * source key 8) before it ever writes it on the way OUT (RQEOUT).  Both use
 * the SAME pointer.  So any pointer bad enough to fault the write-out faults
 * the read-in FIRST -- and this test therefore measures the IN-direction's
 * dangling state, which is the PRE-EXISTING exposure (proven to fault since
 * tstmvck.c scenario 3), not one M5-2b1 created.
 *
 * The one pointer class that gets past the read-in and would have been
 * clobbered by the old key-0 write-out is KEY-0, NON-FETCH-PROTECTED storage:
 * MVCK's source-key check permits a key-8 read of it (fetch protection is what
 * blocks a fetch, and it is off -- b0 measured ISK X'06' on CSA), while a key-8
 * STORE into it is denied.  That is exactly the hazard b1 closes.  It is also
 * exactly what this test must NOT hand over: the only key-0 non-fetch-protected
 * storage an unauthorised client can name is system storage, and if the window
 * ever failed to take, the resulting key-0 store would corrupt it.  The
 * out-direction's dangling state is therefore REASONED from the code path
 * (a straight line from the fault to REPLYC) and labelled as such, not measured.
 *
 * SAFE BY CONSTRUCTION.  The address handed over is one this client has proven
 * it cannot even READ from its own key.  The read-in runs first and uses key 8,
 * so it faults before any store is attempted -- no store happens under ANY
 * window behaviour, including a window that failed to take.
 * ------------------------------------------------------------------------
 *
 * PREREQUISITE: the NSFS STC must be started (S NSFS).
 * THE RED LINE IS UNCHANGED: the client is UNAUTHORISED and never self-auths.
 */
#include "nsfvsvc.h"
#include "nsfreq.h"         /* NSFRQE -- the 64-byte image the request carries */
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                      */
#include <clibtry.h>        /* ___try -- capture the abend, no dump            */
#include <clibwto.h>        /* wtof -- survives a hang, unlike SYSPRINT        */
#include <mbtcheck.h>
#include <string.h>

/* Search for an unreadable address: step upward from our own storage, stop at
 * the first candidate we cannot read.  READ-probe only -- never a store -- so
 * a mapped page belonging to someone else is skipped, not corrupted.  The
 * ceiling stays below the top of the private area (LSQA/SWA) and below CSA,
 * which b0 measured at X'009E0000' on this system. */
#define XF_STEP     0x10000u            /* 64 KB steps                        */
#define XF_CEIL     0x00900000u         /* stop well below LSQA / CSA         */

static NSFRQE   g_image;                /* a valid 64-byte image to carry     */
static unsigned g_probe;                /* the candidate under test           */
static char     g_own[64];              /* a known-good address to start from */

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
xf_query(UINT *state, UINT *infl, UINT *reap)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_QUERY);
    nsfv_svc_issue(&req);
    if (state) *state = req.qstate;
    if (infl)  *infl  = req.qinfl;
    if (reap)  *reap  = req.qreap;
    return req.rc;
}

static int
xf_unstage(void)
{
    NSFV_REQ req;

    xf_req_init(&req, NSFV_REQ_UNSTAGE);
    nsfv_svc_issue(&req);
    return req.rc;
}

/* --- bodies run under ___try -------------------------------------------- */

/* Read one byte of the candidate.  Faults for an unmapped page and for
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

/* The whole point: hand the routine a pointer this client cannot touch. */
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

int
main(void)
{
    UINT     st0, in0, rp0, st1, in1, rp1, st2, in2, rp2;
    int      rc, found;
    unsigned addr;

    wtof("TSTRQXF: WRITE-OUT FAULT OBSERVATION START");
    printf("=== TSTRQXF -- M5-2b1: what a faulting move leaves behind ===\n");

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
               " test leaked it (see the finding below)\n", (unsigned)in0);
    }

    /* ---- find an address this client cannot read ----------------------- */
    memset(g_own, 0, sizeof g_own);
    memset(&g_image, 0, sizeof g_image);
    found = 0;
    addr  = ((unsigned)(void *)g_own + XF_STEP) & ~(XF_STEP - 1u);
    for (; addr < XF_CEIL; addr += XF_STEP) {
        g_probe = addr;
        if (___try(t_read_probe) != 0) { found = 1; break; }
    }
    CHECK(found != 0, "found an address this client cannot read");
    if (!found) {
        wtof("TSTRQXF: no unreadable address below %08X -- scenario skipped",
             XF_CEIL);
        printf("  SKIPPED: no unreadable address found below %08X\n", XF_CEIL);
        wtof("TSTRQXF: WRITE-OUT FAULT OBSERVATION DONE (skipped)");
        return mbt_test_summary("TSTRQXF");
    }
    printf("  probe address = %08X (read faults from the client's own key)\n",
           g_probe);
    wtof("TSTRQXF: probe addr=%08X", g_probe);

    /* ---- hand it to the routine ---------------------------------------- */
    rc = ___try(t_bad_ubuf);
    printf("  bad-ubuf request: try rc=%08X\n", (unsigned)rc);
    wtof("TSTRQXF: bad-ubuf rc=%08X", (unsigned)rc);
    CHECK(rc != 0,
          "the request faulted rather than completing on a bad pointer");
    if (rc > 0) {
        printf("  the fault is S%03X (a protection or translation exception)\n",
               ((unsigned)rc >> 12) & 0xFFFu);
    }
    CHECK(rc >= 0, "the fault was CAUGHT (ESTAE created) -- no dump, client alive");

    /* ---- THE OBSERVATION: what did it leave behind? -------------------- */
    rc = xf_query(&st1, &in1, &rp1);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK,
             "the transport still answers after the fault (not wedged)");
    printf("  after:  state=%u inflight=%u reaped=%u\n",
           (unsigned)st1, (unsigned)in1, (unsigned)rp1);
    wtof("TSTRQXF: after fault state=%u inflight=%u reaped=%u",
         (unsigned)st1, (unsigned)in1, (unsigned)rp1);

    /* PREDICTION, recorded so the run can falsify it: the read-in faults
     * BEFORE the slot is published, so state should still be FREE while
     * inflight is stuck at 1 (incremented ahead of the dispatch, given back
     * only by REPLYC / the bail paths, none of which run).  Reported, not
     * asserted -- the finding is whatever the machine says. */
    printf("  FINDING: slot %s, inflight %s (predicted: FREE / stuck at %u)\n",
           (st1 == NSFV_REQ_FREE) ? "FREE (not published)" : "NOT free",
           (in1 > in0) ? "LEAKED" : "clean", (unsigned)(in0 + 1u));

    /* ---- can the probe verbs clean up after it? ------------------------
     * UNSTAGE exists to give a slot and one in-flight count back.  Whether it
     * can reach THIS leak is part of the observation, so it is reported, not
     * asserted -- only "UNSTAGE is accepted and the slot ends FREE" is a real
     * invariant here. */
    rc = xf_unstage();
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "UNSTAGE accepted after the fault");
    rc = xf_query(&st2, &in2, &rp2);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers after UNSTAGE");
    printf("  after UNSTAGE: state=%u inflight=%u reaped=%u\n",
           (unsigned)st2, (unsigned)in2, (unsigned)rp2);
    wtof("TSTRQXF: after UNSTAGE state=%u inflight=%u", (unsigned)st2,
         (unsigned)in2);
    CHECK_EQ((long)st2, (long)NSFV_REQ_FREE, "the slot is FREE");

    if (in2 > in0) {
        printf("  FINDING: UNSTAGE did NOT recover the in-flight count"
               " (%u -> %u).\n", (unsigned)in0, (unsigned)in2);
        printf("           It gives a count back only when the slot was"
               " PUBLISHED, and this\n");
        printf("           fault happens BEFORE publication -- so the probe's"
               " own cleanup verb\n");
        printf("           cannot reach a pre-publication leak.  Reported for"
               " b2/(c), not fixed here.\n");
        wtof("TSTRQXF: FINDING inflight %u->%u, UNSTAGE cannot recover it",
             (unsigned)in0, (unsigned)in2);
    } else {
        printf("  the in-flight count came back clean\n");
    }

    /* ---- the transport still works end to end -------------------------- */
    rc = xf_query(&st2, &in2, &rp2);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "the transport survived the whole run");

    wtof("TSTRQXF: WRITE-OUT FAULT OBSERVATION DONE");
    return mbt_test_summary("TSTRQXF");
}
