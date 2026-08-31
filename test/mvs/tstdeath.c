/*
 * tstdeath.c -- M5 Stage-0c: the client-death guard, ROW 1 (TSTDEATH).
 *
 * ADR-0040.  MVS-only (project.toml host = false): the guard reads real control
 * blocks (CVT -> ASVT) and the transport is a cross-AS SVC/CSA rendezvous, so
 * there is no host analog.  The probe STC (NSFV) must already be started
 * (S NSFV) -- the same peer-must-run prerequisite as TSTSVC and TSTUBUF.
 *
 * THE RED LINE (ADR-0038): the client is UNAUTHORIZED and stays so.  It asserts
 * TESTAUTH (__isauth() == 0) and never calls clib_apf_setup or any
 * self-authorization SVC.
 *
 * ============================================================================
 * WHAT THIS COVERS, AND WHAT IT NO LONGER COVERS  (M5-2c2 stage c, option E)
 * ============================================================================
 *
 * It proves ROW 1 -- LIVE -- and nothing else.  It is deliberately still an
 * ISOLATED Stage-0 probe: no CTCI, no sockets, no protocol layer, so when it
 * goes red it names a MECHANISM rather than "something in the stack broke".
 * That property is the whole reason this file was kept rather than retired.
 *
 * The full ADR-0040 coverage map, and the KIND of coverage each row has:
 *
 *   row 1  LIVE                 LIVE, this named probe, + host-pinned
 *   row 2  DEAD (avail bit)     an operator-driven PROCEDURE at milestone
 *                               gates -- NOT a test, NOT in the matrix
 *                               (docs/procedure-row2-client-death.md)
 *                               + host-pinned
 *   row 3  DEAD (ASCB reuse)    host-pinned ONLY -- not live-producible:
 *                               0 of 9 reuses produced it, because the ASCB
 *                               comes back unchanged and the guard reads LIVE
 *   row 4  UNKNOWN              host-pinned ONLY -- four branches, none of
 *                               them live-producible by a real client
 *
 * "Host-pinned" means test/tstreqx.c drives nsfreqx_classify directly and pins
 * every row and every UNKNOWN branch.  That is the floor under all four rows
 * and it is untouched by this file.
 *
 * WHY ROWS 2-4 LEFT THIS FILE.  They were driven by NSFV_REQ_ORPHAN, which
 * staged a REQUEST-SUPPLIED identity verbatim -- a forged identity accepted
 * from an unauthorised caller, which is exactly what the guard must never
 * trust for a real client.  The verb was retired in M5-2c2 stage b.  Row 2 has
 * a real replacement (a real STC client dying with a request outstanding: 6 of
 * 6 in the M5-2c2 mapping round), but it needs an operator and has no
 * deterministic batch form -- hence a procedure.  Rows 3 and 4 were measured
 * NOT live-producible at all, so nothing was lost there but a rehearsal.
 *
 * ============================================================================
 * HOW THIS FILE FAILS -- read this before diagnosing a red or missing run
 * ============================================================================
 *
 * CLEAN FAIL (CC 1, assertions reported): everything except a misclassified
 * live client -- a broken round trip, a wrong token, a leaked slot or
 * in-flight count, a reaped counter that moved.
 *
 * HANG (no RC, SYSPRINT lost to the S222): THIS IS THE ROW-1 SIGNAL.  The
 * blocking ECHO parks on the CSA reply ECB, and the STC only POSTs it after
 * the guard classifies the client LIVE.  So if the guard wrongly answers DEAD
 * or UNKNOWN, nothing POSTs and this job blocks forever -- it cannot report a
 * failed assertion, because it never regains control.
 *
 * THAT IS WHY THE CONSOLE MARKERS BELOW EXIST.  A hung job's SYSPRINT is lost,
 * but the console log survives (the M4-5 lesson).  If TSTDEATH's last console
 * line is the "ISSUING THE BLOCKING ECHO" marker, the guard did not classify
 * this live client as LIVE -- and the STC will have said which way it went, in
 * the same log:
 *     NSFV050I CLIENT DEAD ...      -> the guard reaped a LIVE client
 *     NSFV051W CLIENT LIVENESS UNKNOWN ... -> it could not decide
 * Either line beside a missing completion marker names the mechanism exactly.
 */
#include "nsfvsvc.h"
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1), __ascb              */
#include <clibwto.h>        /* wtof -- the console survives a hang             */
#include <mbtcheck.h>
#include <string.h>

#define TSTD_ASCBASID    0x24U         /* ASCBASID halfword in the ASCB        */

/* Issue the private SVC with R1 = A(req) via the EX-SVC-0 trick (SVC number in
 * R6 -- EX ORs its low byte into a stored "SVC 0", so no storage is modified,
 * RENT-safe).  Identical to TSTSVC's / TSTUBUF's issuer (ADR-0038 6); the asm
 * labels are per-file, so they are unique in the load module. */
static void __attribute__((noinline))
nsfv_svc_issue(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"          /* R1 = A(req)                        */
        "         LR    6,%1\n"          /* R6 = SVC number                    */
        "         EX    6,NSFDTH0\n"     /* execute SVC <R6-low>               */
        "         B     NSFDTHX\n"
        "NSFDTH0  SVC   0\n"             /* EX target; storage unmodified       */
        "NSFDTHX  DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

static void
tstd_req_init(NSFV_REQ *req, UINT func)
{
    memset(req, 0, sizeof *req);
    memcpy(req->eye, NSFV_REQ_EYE, 4);
    req->func = func;
    req->rc   = -1;
}

/* QUERY: the anchor's state, as seen from an address space that cannot read
 * CSA itself.  Changes nothing and works while the slot is busy -- it branches
 * out ahead of the claim, so it never blocks. */
static int
tstd_query(UINT *state, UINT *infl, UINT *reap)
{
    NSFV_REQ req;

    tstd_req_init(&req, NSFV_REQ_QUERY);
    nsfv_svc_issue(&req);
    if (state) *state = req.qstate;
    if (infl)  *infl  = req.qinfl;
    if (reap)  *reap  = req.qreap;
    return req.rc;
}

/* UNSTAGE: give back a slot the STC deliberately did not release. */
static int
tstd_unstage(void)
{
    NSFV_REQ req;

    tstd_req_init(&req, NSFV_REQ_UNSTAGE);
    nsfv_svc_issue(&req);
    return req.rc;
}

static UINT
tstd_own_asid(void *ascb)
{
    if (!ascb) return 0u;
    return (UINT)(*(unsigned short *)((char *)ascb + TSTD_ASCBASID));
}

int
main(void)
{
    void *own_ascb;
    UINT  own_asid;
    UINT  state = 0;
    UINT  infl  = 0;
    UINT  reap  = 0;
    UINT  reap0 = 0;

    wtof("TSTDEATH: CLIENT-DEATH GUARD, ROW 1 (LIVE) -- START (SVC %u)",
         (unsigned)NSFV_SVCNUM);

    /* THE RED LINE: unauthorized, and it stays that way. */
    CHECK_EQ((long)__isauth(), 0L,
             "client is UNAUTHORIZED (TESTAUTH FCTN=1) and does not self-auth");

    own_ascb = __ascb(0);
    own_asid = tstd_own_asid(own_ascb);
    wtof("TSTDEATH: OWN ASCB=%08X ASID=%04X",
         (unsigned)own_ascb, (unsigned)own_asid);
    CHECK(own_ascb != 0, "own ASCB located (PSAAOLD)");
    CHECK(own_asid != 0u, "own ASID read from ASCBASID (ASCB+X'24')");

    /* Baseline: the STC is idle and the slot is free.  QUERY does not block,
     * so a failure here is a CLEAN fail and rules the transport out before the
     * blocking round trip below can hang on it. */
    CHECK_EQ((long)tstd_query(&state, &infl, &reap0), (long)NSFV_RC_OK,
             "QUERY answers (probe verb reaches the routine)");
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "baseline: request slot FREE");
    CHECK_EQ((long)infl, 0L, "baseline: nothing in flight");

    /* ---------------------------------------------------------------
     * ROW 1 -- LIVE.  The full Stage-0a' round trip: stage, POST, WAIT,
     * wake, copy out.  The STC POSTs the reply ONLY after the guard has
     * classified this (living) client LIVE, so a completed round trip
     * IS the row-1 witness -- reaping under a live client is the
     * catastrophe the whole design is bent around.
     *
     * The markers bracket the blocking call on purpose: see the file
     * header.  If the second marker never appears, the guard did not
     * answer LIVE, and NSFV050I / NSFV051W in the same console log says
     * which way it went.
     * --------------------------------------------------------------- */
    wtof("TSTDEATH: ROW 1 -- ISSUING THE BLOCKING ECHO."
         " IF THIS IS THE LAST TSTDEATH LINE, THE GUARD DID NOT ANSWER LIVE;"
         " LOOK FOR NSFV050I / NSFV051W");
    {
        NSFV_REQ req;

        tstd_req_init(&req, NSFV_REQ_ECHO);
        req.token = 0x0C0C0005u;
        nsfv_svc_issue(&req);

        wtof("TSTDEATH: ROW 1 -- BLOCKING ECHO RETURNED rc=%d token=%08X",
             (int)req.rc, (unsigned)req.token);

        CHECK_EQ((long)req.rc, (long)NSFV_RC_OK, "ECHO round trip rc OK");
        CHECK_EQ((long)req.token, (long)0x0C0C0006u,
                 "ECHO round trip: token echoed +1 (the reply POST landed)");
    }
    (void)tstd_query(&state, &infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "after ECHO: slot FREE");
    CHECK_EQ((long)infl, 0L, "after ECHO: nothing in flight");
    /* The guard did not reap a LIVE client.  This is the assertion that makes
     * a WRONG-but-not-hanging verdict a clean FAIL rather than a silent pass:
     * a reap would have moved this counter. */
    CHECK_EQ((long)reap, (long)reap0, "LIVE client: nothing reaped");

    /* Unconditional cleanup: leave no in-flight count behind whatever
    ** happened above, so the STC still stops clean (its drain retains CSA on
    ** a timeout -- ADR-0038 5, unchanged by Stage-0c). */
    (void)tstd_unstage();
    (void)tstd_query(&state, &infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "final: slot FREE");
    CHECK_EQ((long)infl, 0L, "final: in-flight zero (STC can stop clean)");
    wtof("TSTDEATH: DONE -- ROW 1 (LIVE) PROVEN, REAPED=%u INFLIGHT=%u",
         (unsigned)reap, (unsigned)infl);

    return mbt_test_summary("TSTDEATH");
}
