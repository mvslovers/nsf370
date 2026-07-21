/*
 * tstssi.c -- M5 Stage-0a SSI cross-AS probe: the CLIENT (TSTSSI).
 *
 * MVS-only (project.toml host = false).  Cross-AS SSI has no host analog; this
 * IS the transport test.  The client runs in its own address space (a batch
 * job); the probe STC (NSFP) must already be started (S NSFP) -- like the M3/M4
 * live gates that need a peer running (`nc`, a listener).  If NSFP is not up,
 * iefssreq returns SSRTNOSS and the CHECKs fail cleanly, self-documenting the
 * prerequisite.
 *
 * Each round trip: build the SSOB + SSIB(name=NSFP) + our extension, call
 * IEFSSREQ, and verify the token round-tripped byte-exact through the STC
 * (STC echoes token+1) plus a monotonic server-side served counter.  ADR-0036.
 *
 * The five-abend gauntlet (S047/S0C4/S202/S102/X'201') is proven by the job
 * completing CC 0 with no dump -- their ABSENCE is the proof the state/key
 * rules were inherited correctly; grep the spool/console log to confirm.
 *
 * The client authorises itself via clib_apf_setup (SVC 244): IEFSSREQ is an
 * authorized branch-entry and the router does key-0 CSA work in THIS address
 * space, so the calling task must be APF-authorised (ADR-0036).  The library it
 * runs from need not be APF.
 */
#include "nsfpssi.h"
#include <iefssobh.h>       /* SSOB, iefssreq, SSRTOK                          */
#include <iefjssib.h>       /* SSIB                                            */
#include <clibos.h>         /* clib_apf_setup                                 */
#include <clibwto.h>        /* wtof (console markers survive a hang)          */
#include <mbtcheck.h>
#include <string.h>

#define TSTSSI_N   50u      /* main round trips (>= 40 per the gate)          */

int
main(void)
{
    SSIB      ssib;
    SSOB      ssob;
    NSFPSSOB  ext;
    unsigned  i;
    int       rc;
    UINT      base_seq = 0u;
    int       have_base = 0;

    wtof("TSTSSI: SSI PROBE CLIENT START (N=%u)", (unsigned)TSTSSI_N);

    /* Self-authorise (SVC 244): needed for IEFSSREQ + the router's key-0 work. */
    CHECK_EQ((long)clib_apf_setup("TSTSSI"), 0L,
             "clib_apf_setup (SVC 244 self-authorization)");

    /* Fixed request framing (rebuilt extension per call). */
    memset(&ssib, 0, sizeof ssib);
    memcpy(ssib.SSIBID, "SSIB", 4);
    ssib.SSIBLEN = (unsigned short)sizeof(SSIB);
    memcpy(ssib.SSIBSSNM, NSFP_SUBSYS, 4);

    memset(&ssob, 0, sizeof ssob);
    memcpy(ssob.SSOBID, "SSOB", 4);
    ssob.SSOBLEN  = (unsigned short)sizeof(SSOB);
    ssob.SSOBFUNC = (unsigned short)NSFP_SSOBFUNC;
    ssob.SSOBSSIB = &ssib;
    ssob.SSOBINDV = &ext;

    for (i = 0u; i < TSTSSI_N; i++) {
        UINT sent = 0xABCD0000u + i;        /* asymmetric: catches byte-swap    */

        memset(&ext, 0, sizeof ext);
        memcpy(ext.eye, NSFPSSOB_EYE, 4);
        ext.func      = NSFP_REQ_ECHO;
        ext.token     = sent;
        ssob.SSOBRETN = -1;

        rc = iefssreq(&ssob);

        CHECK_EQ((long)rc, (long)SSRTOK, "iefssreq SSRTOK (NSFP up + routed)");
        CHECK_EQ((long)ssob.SSOBRETN, (long)NSFP_RC_OK, "router SSOBRETN OK");
        CHECK_EQ((long)ext.rc, (long)NSFP_RC_OK, "extension rc OK");
        CHECK_EQ((long)ext.token, (long)(sent + 1u),
                 "token round-trips byte-exact (STC echo = token+1)");

        if (!have_base) { base_seq = ext.seq; have_base = 1; }
        CHECK_EQ((long)ext.seq, (long)(base_seq + i),
                 "served counter monotonic across round trips");
    }

    /* Edge tokens: wrap and the sign boundary through the same path. */
    {
        UINT edge[3];
        UINT j;
        edge[0] = 0xFFFFFFFFu;              /* +1 -> 0 (wrap)                   */
        edge[1] = 0x00000000u;              /* +1 -> 1                          */
        edge[2] = 0x7FFFFFFFu;              /* +1 -> 0x80000000 (sign bit)      */

        for (j = 0u; j < 3u; j++) {
            memset(&ext, 0, sizeof ext);
            memcpy(ext.eye, NSFPSSOB_EYE, 4);
            ext.func      = NSFP_REQ_ECHO;
            ext.token     = edge[j];
            ssob.SSOBRETN = -1;

            rc = iefssreq(&ssob);

            CHECK_EQ((long)rc, (long)SSRTOK, "iefssreq SSRTOK (edge token)");
            CHECK_EQ((long)ssob.SSOBRETN, (long)NSFP_RC_OK, "router OK (edge)");
            CHECK_EQ((long)ext.token, (long)(edge[j] + 1u),
                     "edge token round-trips byte-exact (+1, wrap/sign)");
        }
    }

    wtof("TSTSSI: SSI PROBE CLIENT DONE (base_seq=%u)", (unsigned)base_seq);

    return mbt_test_summary("TSTSSI");
}
