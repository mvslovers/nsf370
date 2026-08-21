/*
 * tstdeath.c -- M5 Stage-0c: the client-death guard CLIENT (TSTDEATH).
 *
 * ADR-0040.  MVS-only (project.toml host = false): the guard reads real control
 * blocks (CVT -> ASVT) and the transport is a cross-AS SVC/CSA rendezvous, so
 * there is no host analog; the host coverage of this stage is the
 * NSF_SIZE_ASSERT / NSFV_OFF_ASSERT set in nsfvsvc.h firing at cc370
 * cross-compile.  The probe STC (NSFV) must already be started (S NSFV) -- the
 * same peer-must-run prerequisite as TSTSVC and TSTUBUF.
 *
 * THE RED LINE (ADR-0038): the client is UNAUTHORIZED and stays so.  It asserts
 * TESTAUTH (__isauth() == 0) and never calls clib_apf_setup or any
 * self-authorization SVC.
 *
 * WHAT IT PROVES.  That the STC, before it POSTs a reply, establishes whether
 * the client address space is still there -- and that it gets all three answers
 * right:
 *
 *   LIVE     a request whose identity is this (living) address space is
 *            serviced and posted, exactly as in Stage-0a'/0b.  This is the
 *            direction that MUST NOT false-positive: reaping under a live
 *            client is the catastrophe the whole design is bent around.
 *   DEAD     a request whose identity names an address space that is not there
 *            is REAPED -- the in-flight count comes back, the staging is freed,
 *            the slot is released -- and is NEVER posted into.  Two rows:
 *            an ASID the ASVT reports available, and an ASID that is assigned
 *            but to a DIFFERENT ASCB (the reuse row, ADR-0040 1).
 *   UNKNOWN  an identity the guard cannot look up is neither posted into nor
 *            reaped: the request is HELD.  "I could not check" must never be
 *            answered with "go ahead and free it" (ADR-0040 2).
 *
 * HOW CLIENT DEATH IS MODELLED -- AND WHAT IS SYNTHETIC.  A deterministic batch
 * gate cannot kill its own address space and then verify the result, so this
 * client stays alive and hands the STC a DEAD IDENTITY instead (NSFV_REQ_ORPHAN
 * stages the identity the request carries, POSTs the STC and returns WITHOUT
 * waiting).  Genuine: the in-flight increment, the staged request, the SKIPPED
 * decrement, and an identity that really does name no live address space.
 * Synthetic: the race between a kill and the STC's POST -- that one is the
 * manual, operator-timed check documented in the PR.
 *
 * ONE DETAIL IS LOAD-BEARING (ADR-0040 8): the free-ASID scenario stages this
 * client's OWN REAL ASCB with a free ASID.  If that ASID is assigned between
 * the pick and the STC's check, the ASVT entry then holds the new occupant's
 * ASCB, which cannot be this still-living client's -- so the scenario still
 * reads DEAD, through the mismatch row instead of the available-bit row.
 * Replacing that ASCB with a dummy value would turn the race into a false LIVE.
 */
#include "nsfvsvc.h"
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1), __ascb              */
#include <clibecb.h>        /* ecb_timed_wait (the poll pause)                 */
#include <clibwto.h>        /* wtof (console markers survive a hang)           */
#include <cvt.h>            /* CVT                                             */
#include <ihaasvt.h>        /* ASVT (asvtmaxu, asvtenty)                       */
#include <mbtcheck.h>
#include <string.h>

#define TSTD_ASVT_AVAIL  0x80000000U   /* ASVTAVAI: ASID available/unassigned  */
#define TSTD_ASCBASID    0x24U         /* ASCBASID halfword in the ASCB        */
#define TSTD_POLL_HSEC   10U           /* 0.10 s per poll                      */
#define TSTD_POLL_MAX    50U           /* 50 * 0.10 s = 5 s ceiling            */

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
tstd_pause(unsigned hsec)
{
    ECB local = 0;                       /* nobody posts it; the timer does    */
    ecb_timed_wait(&local, hsec, 0);
}

/* --- the four probe calls ------------------------------------------------ */

static void
tstd_req_init(NSFV_REQ *req, UINT func)
{
    memset(req, 0, sizeof *req);
    memcpy(req->eye, NSFV_REQ_EYE, 4);
    req->func = func;
    req->rc   = -1;
}

/* QUERY: the anchor's state, as seen from an address space that cannot read
 * CSA itself.  Changes nothing and works while the slot is busy. */
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

/* ORPHAN: stage a request carrying `ascb`/`asid` verbatim, POST the STC, come
 * straight back -- the in-flight decrement never runs. */
static int
tstd_orphan(void *ascb, UINT asid)
{
    NSFV_REQ req;

    tstd_req_init(&req, NSFV_REQ_ORPHAN);
    req.token = 0x0C0C0000u;
    req.pascb = ascb;
    req.pasid = asid;
    nsfv_svc_issue(&req);
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

/* Poll until the anchor's request state leaves PENDING (the STC has looked at
 * it), or the ceiling expires.  Returns the last state seen. */
static UINT
tstd_settle(UINT *infl, UINT *reap)
{
    UINT     state = NSFV_REQ_PENDING;
    unsigned n;

    for (n = 0; n < TSTD_POLL_MAX; n++) {
        (void)tstd_query(&state, infl, reap);
        if (state != NSFV_REQ_PENDING) break;
        tstd_pause(TSTD_POLL_HSEC);
    }
    return state;
}

/* --- identity helpers ---------------------------------------------------- */

static UINT
tstd_own_asid(void *ascb)
{
    if (!ascb) return 0u;
    return (UINT)(*(unsigned short *)((char *)ascb + TSTD_ASCBASID));
}

/* Find an ASID the ASVT currently reports AVAILABLE.  Scans DOWNWARD from
 * asvtmaxu: the high end of the range is the least likely to be handed to a
 * new address space while this test runs (and even if it is, the scenario
 * still reads DEAD -- see the file header).  Returns 0 if none is free.
 *
 * Reading the CVT and the ASVT from an unauthorized problem-state program is
 * the same access ufsd_server_state / ufsd_sess_cleanup rely on: both are
 * fetch-accessible, and nothing here writes. */
static UINT
tstd_free_asid(void)
{
    CVT  *cvt;
    ASVT *asvt;
    UINT  i;

    cvt = *(CVT **)16;
    if (!cvt) return 0u;
    asvt = (ASVT *)cvt->cvtasvt;
    if (!asvt || asvt->asvtmaxu == 0u) return 0u;

    for (i = asvt->asvtmaxu; i >= 1u; i--) {
        unsigned entry = *(unsigned *)&asvt->asvtenty[i - 1u];
        if (entry & TSTD_ASVT_AVAIL) return i;   /* ASID i is unassigned */
    }
    return 0u;
}

int
main(void)
{
    void *own_ascb;
    UINT  own_asid;
    UINT  free_asid;
    UINT  state = 0;
    UINT  infl  = 0;
    UINT  reap  = 0;
    UINT  reap0 = 0;

    wtof("TSTDEATH: CLIENT-DEATH GUARD PROBE START (SVC %u)",
         (unsigned)NSFV_SVCNUM);

    /* THE RED LINE: unauthorized, and it stays that way. */
    CHECK_EQ((long)__isauth(), 0L,
             "client is UNAUTHORIZED (TESTAUTH FCTN=1) and does not self-auth");

    own_ascb  = __ascb(0);
    own_asid  = tstd_own_asid(own_ascb);
    free_asid = tstd_free_asid();
    wtof("TSTDEATH: OWN ASCB=%08X ASID=%04X, FREE ASID=%04X",
         (unsigned)own_ascb, (unsigned)own_asid, (unsigned)free_asid);
    CHECK(own_ascb != 0, "own ASCB located (PSAAOLD)");
    CHECK(own_asid != 0u, "own ASID read from ASCBASID (ASCB+X'24')");
    CHECK(free_asid != 0u, "an AVAILABLE ASID found in the ASVT");

    /* Baseline: the STC is idle and the slot is free. */
    CHECK_EQ((long)tstd_query(&state, &infl, &reap0), (long)NSFV_RC_OK,
             "QUERY answers (probe verb reaches the routine)");
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "baseline: request slot FREE");
    CHECK_EQ((long)infl, 0L, "baseline: nothing in flight");

    /* ---------------------------------------------------------------
     * 1. LIVE.  An orphaned request whose identity is THIS address space
     *    must be serviced and posted like any other -- the guard may not
     *    mistake a living client for a dead one.  Deliberately first: it
     *    is the non-blocking way to prove the classification (an
     *    asvtenty[asid-1] off-by-one shows up here), so a broken guard
     *    fails loudly instead of hanging the blocking round trip below.
     * --------------------------------------------------------------- */
    wtof("TSTDEATH: 1 -- ORPHAN with the LIVE identity");
    CHECK_EQ((long)tstd_orphan(own_ascb, own_asid), (long)NSFV_RC_OK,
             "LIVE orphan: SVC accepted and returned without waiting");
    state = tstd_settle(&infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_DONE,
             "LIVE client: request SERVICED (state DONE), not reaped");
    CHECK_EQ((long)infl, 1L, "LIVE client: in-flight count NOT given back");
    CHECK_EQ((long)reap, (long)reap0, "LIVE client: reaped counter unchanged");

    CHECK_EQ((long)tstd_unstage(), (long)NSFV_RC_OK, "UNSTAGE accepted");
    (void)tstd_query(&state, &infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "after UNSTAGE: slot FREE");
    CHECK_EQ((long)infl, 0L, "after UNSTAGE: in-flight back to zero");

    /* ---------------------------------------------------------------
     * 2. DEAD, available-ASID row.  The identity names an ASID the ASVT
     *    reports available: no address space, so the request is reaped
     *    and never posted into.
     * --------------------------------------------------------------- */
    wtof("TSTDEATH: 2 -- ORPHAN with a FREE ASID (dead client)");
    reap0 = reap;
    CHECK_EQ((long)tstd_orphan(own_ascb, free_asid), (long)NSFV_RC_OK,
             "dead orphan (free ASID): SVC accepted");
    state = tstd_settle(&infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE,
             "DEAD client (ASID available): request REAPED, slot released");
    CHECK_EQ((long)infl, 0L, "DEAD client: in-flight count reclaimed");
    CHECK_EQ((long)reap, (long)(reap0 + 1u), "DEAD client: reaped counter +1");

    /* ---------------------------------------------------------------
     * 3. DEAD, ASCB-reuse row.  The ASID IS assigned -- to this very
     *    address space -- but the recorded ASCB is not the one the ASVT
     *    holds for it.  That is exactly the shape of a dead client whose
     *    ASID was handed to someone else, and it is the row an ASID-only
     *    check (ufsd_sess_cleanup's) cannot see.
     * --------------------------------------------------------------- */
    wtof("TSTDEATH: 3 -- ORPHAN with a MISMATCHED ASCB (ASID reuse)");
    reap0 = reap;
    CHECK_EQ((long)tstd_orphan((void *)((char *)own_ascb + 8), own_asid),
             (long)NSFV_RC_OK, "dead orphan (ASCB mismatch): SVC accepted");
    state = tstd_settle(&infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE,
             "DEAD client (ASID reused): request REAPED, slot released");
    CHECK_EQ((long)infl, 0L, "reuse row: in-flight count reclaimed");
    CHECK_EQ((long)reap, (long)(reap0 + 1u), "reuse row: reaped counter +1");

    /* ---------------------------------------------------------------
     * 4. UNKNOWN.  No ASCB to look up: the guard must refuse BOTH ways --
     *    no post (it cannot prove the client is there) and no reap (it
     *    cannot prove it is gone).  The request is HELD and only the
     *    client's own UNSTAGE releases it.
     * --------------------------------------------------------------- */
    wtof("TSTDEATH: 4 -- ORPHAN with NO ASCB (undecidable)");
    reap0 = reap;
    CHECK_EQ((long)tstd_orphan((void *)0, own_asid), (long)NSFV_RC_OK,
             "undecidable orphan: SVC accepted");
    state = tstd_settle(&infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_HELD,
             "UNKNOWN client: request HELD -- neither posted into nor reaped");
    CHECK_EQ((long)infl, 1L, "UNKNOWN client: in-flight count NOT reclaimed");
    CHECK_EQ((long)reap, (long)reap0,
             "UNKNOWN client: reaped counter unchanged (safe side)");

    CHECK_EQ((long)tstd_unstage(), (long)NSFV_RC_OK, "UNSTAGE after HELD");
    (void)tstd_query(&state, &infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "held slot released by UNSTAGE");
    CHECK_EQ((long)infl, 0L, "held in-flight count released by UNSTAGE");

    /* ---------------------------------------------------------------
     * 5. The LIVE control, blocking.  The full Stage-0a' round trip --
     *    stage, POST, WAIT, wake, copy out -- still works with the guard
     *    in front of the reply POST, and reaps nothing.
     * --------------------------------------------------------------- */
    wtof("TSTDEATH: 5 -- blocking ECHO round trip (LIVE control)");
    reap0 = reap;
    {
        NSFV_REQ req;

        tstd_req_init(&req, NSFV_REQ_ECHO);
        req.token = 0x0C0C0005u;
        nsfv_svc_issue(&req);

        CHECK_EQ((long)req.rc, (long)NSFV_RC_OK, "ECHO round trip rc OK");
        CHECK_EQ((long)req.token, (long)0x0C0C0006u,
                 "ECHO round trip: token echoed +1 (the reply POST landed)");
    }
    (void)tstd_query(&state, &infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "after ECHO: slot FREE");
    CHECK_EQ((long)infl, 0L, "after ECHO: nothing in flight");
    CHECK_EQ((long)reap, (long)reap0, "after ECHO: nothing reaped");

    /* Unconditional cleanup: leave no in-flight count behind whatever
    ** happened above, so the STC still stops clean (its drain retains CSA on
    ** a timeout -- ADR-0038 5, unchanged by Stage-0c). */
    (void)tstd_unstage();
    (void)tstd_query(&state, &infl, &reap);
    CHECK_EQ((long)state, (long)NSFV_REQ_FREE, "final: slot FREE");
    CHECK_EQ((long)infl, 0L, "final: in-flight zero (STC can stop clean)");
    wtof("TSTDEATH: DONE -- REAPED=%u INFLIGHT=%u", (unsigned)reap,
         (unsigned)infl);

    return mbt_test_summary("TSTDEATH");
}
