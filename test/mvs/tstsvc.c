/*
 * tstsvc.c -- M5 Stage-0a' SVC cross-AS probe: the CLIENT (TSTSVC).
 *
 * ADR-0038.  MVS-only (project.toml host = false).  Cross-AS via a private SVC
 * has no host analog; this IS the transport test.  The client runs in its own
 * address space (a batch job); the probe STC (NSFV) must already be started
 * (S NSFV) -- like the M3/M4 live gates that need a peer running.  If NSFV is
 * not up, the stolen SVC slot holds its original (invalid-SVC) routine and the
 * SVC abends the caller -- so the prerequisite is "NSFV started" (documented).
 *
 * THE DECISIVE PROOF (ADR-0038): the client is UNAUTHORIZED and stays so.
 *   - It verifies TESTAUTH (__isauth() == TESTAUTH FCTN=1) reports NOT
 *     authorized, and asserts it.
 *   - It NEVER calls clib_apf_setup / any self-authorization SVC.
 * An unauthorized problem-state program completing the round trip is the thing
 * the SSI transport (ADR-0036) could NOT do -- that is the whole value of 0a'.
 *
 * Each round trip: build an NSFV_REQ (eye NSFV, func ECHO, a token), issue the
 * private SVC with R1 = &req via the EX-SVC-0 trick (the SVC number in R6 -- the
 * relink-only pattern, mvs38j-ip mvsintr.c), and verify the token round-tripped
 * byte-exact through the STC (echo = token+1) plus a monotonic served counter.
 * The result is read from the caller's block (authoritative; no dependence on
 * the SVC register-return convention -- ADR-0038 6).
 */
#include "nsfpsvc.h"
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                       */
#include <clibwto.h>        /* wtof (console markers survive a hang)           */
#include <mbtcheck.h>
#include <string.h>

#define TSTSVC_N   50u      /* main round trips (>= 40 per the gate)           */

/* Issue the private SVC with R1 = A(req) via the EX-SVC-0 trick.  The SVC
 * number rides R6 (a runtime value, not baked into the opcode -- the relink-
 * only pattern); EX ORs its low byte into a stored "SVC 0", so no storage is
 * modified (RENT-safe).  noinline: the named asm labels must appear once.
 * The result is written by the SVC routine into *req (a key-0 store into this
 * problem-state key-8 block); we read it from memory afterward. */
static void __attribute__((noinline))
nsfv_svc_issue(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"          /* R1 = A(req)                        */
        "         LR    6,%1\n"          /* R6 = SVC number                    */
        "         EX    6,NSFVSVC0\n"    /* execute SVC <R6-low>               */
        "         B     NSFVSVCX\n"
        "NSFVSVC0 SVC   0\n"             /* EX target; storage unmodified      */
        "NSFVSVCX DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

int
main(void)
{
    NSFV_REQ  req;
    unsigned  i;
    UINT      base_seq = 0u;
    int       have_base = 0;

    wtof("TSTSVC: SVC PROBE CLIENT START (N=%u, SVC %u)",
         (unsigned)TSTSVC_N, (unsigned)NSFV_SVCNUM);

    /* THE RED LINE: the client must be UNAUTHORIZED, and must not self-auth.
    ** __isauth() is TESTAUTH FCTN=1: it returns 0 when NOT APF-authorized. */
    CHECK_EQ((long)__isauth(), 0L,
             "client is UNAUTHORIZED (TESTAUTH FCTN=1) and does not self-auth");

    for (i = 0u; i < TSTSVC_N; i++) {
        UINT sent = 0xABCD0000u + i;        /* asymmetric: catches byte-swap   */

        memset(&req, 0, sizeof req);
        memcpy(req.eye, NSFV_REQ_EYE, 4);
        req.func  = NSFV_REQ_ECHO;
        req.token = sent;
        req.rc    = -1;

        nsfv_svc_issue(&req);

        CHECK_EQ((long)req.rc, (long)NSFV_RC_OK, "router rc OK");
        CHECK_EQ((long)req.token, (long)(sent + 1u),
                 "token round-trips byte-exact (STC echo = token+1)");

        if (!have_base) { base_seq = req.seq; have_base = 1; }
        CHECK_EQ((long)req.seq, (long)(base_seq + i),
                 "served counter monotonic across round trips");
    }

    /* Edge tokens: wrap and the sign boundary through the same path. */
    {
        UINT edge[3];
        UINT j;
        edge[0] = 0xFFFFFFFFu;              /* +1 -> 0 (wrap)                  */
        edge[1] = 0x00000000u;              /* +1 -> 1                         */
        edge[2] = 0x7FFFFFFFu;              /* +1 -> 0x80000000 (sign bit)     */

        for (j = 0u; j < 3u; j++) {
            memset(&req, 0, sizeof req);
            memcpy(req.eye, NSFV_REQ_EYE, 4);
            req.func  = NSFV_REQ_ECHO;
            req.token = edge[j];
            req.rc    = -1;

            nsfv_svc_issue(&req);

            CHECK_EQ((long)req.rc, (long)NSFV_RC_OK, "router rc OK (edge)");
            CHECK_EQ((long)req.token, (long)(edge[j] + 1u),
                     "edge token round-trips byte-exact (+1, wrap/sign)");
        }
    }

    wtof("TSTSVC: SVC PROBE CLIENT DONE (base_seq=%u)", (unsigned)base_seq);

    return mbt_test_summary("TSTSVC");
}
