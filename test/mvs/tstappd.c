/*
 * tstappd.c -- M5-2c1 stage a: what does the client-death guard say about an
 *              application that ended WITHOUT calling TERMAPI?
 *
 * MVS-only (host = false): there is no ASVT, no second address space and no
 * "the job ended" on the host.  The portable half of this step -- the registry,
 * the zero-identity red line and the report's text -- is pinned in TSTREQ.
 *
 * THIS TEST MEASURES; IT ASSERTS ALMOST NOTHING.  The answer it exists to get
 * is not knowable in advance and is not this program's to judge: the operator
 * issues `F NSFS,APPS` and reads the verdict off the console.  What the
 * program guarantees is that the SITUATION is real -- an app instance with
 * sockets, registered from THIS address space, whose identity is on the
 * console so the reading can be tied to a job that demonstrably ended.
 *
 * WHY THE ANSWER DECIDES THE FEATURE.  Reclaiming a leaked app slot means
 * classifying its owner DEAD.  ADR-0040's rule is absolute: UNKNOWN is never
 * reaped, because a live client called dead has its storage freed underneath
 * it.  So if MVS leaves the ASVT entry standing after a normal end, the
 * verdict is UNKNOWN, and a sweep would reclaim nothing in exactly the case
 * relink-only applications will actually produce.  Better to know that before
 * the sweep exists than after.
 *
 * PREREQUISITE: the NSFS STC must be started (S NSFS).
 *
 * ROLES, by PARM -- one program, because the three arms must differ ONLY in
 * how the job ends.  Built from different code they would prove less:
 *
 *   (no PARM) / 'LEAVE'  THE MAIN CASE.  INITAPI, SOCKET, and RETURN without
 *                        TERMAPI -- the mistake a relink-only application
 *                        actually makes.  Ends normally, CC 0.
 *   'HANG'               the operator-driven case.  Same, then waits to be
 *                        CANCELled (bounded, so a forgotten job does not sit
 *                        on a slot for ever).  A run that reaches its ceiling
 *                        WITHOUT being cancelled cleans up after itself and
 *                        says so -- it must not be mistaken for the arm.
 *   'CLEAN'              THE CONTROL.  The same work, ending with TERMAPI.
 *                        Without it, "one slot in use" after a LEAVE run is
 *                        not evidence: it could be a registry that never
 *                        empties.  This arm makes the reading mean something.
 *   'PARK'               40-CHK.  INITAPI, SOCKET, BIND to a fixed port, then
 *                        a BLOCKING RECVFROM that nothing will satisfy -- so
 *                        the client is parked INSIDE the transport, with a
 *                        published request outstanding, when it is CANCELled.
 *                        The other arms die between requests; this one dies
 *                        during one, which is the only state in which the
 *                        ADR-0040 guard has anything to protect.
 *
 * WHY 'PARK' IS A ROLE HERE RATHER THAN A SECOND PROGRAM.  The arms must
 * differ ONLY in how the job dies; built separately they would differ in
 * everything and prove less.  It is the same reason CLEAN shares this file.
 *
 * EACH LEAVE / HANG RUN COSTS ONE APP SLOT until something reclaims it, which
 * is the entire point and also a warning: the registry is 16 slots, and after
 * sixteen leaked runs INITAPI starts failing EMFILE.  `P NSFS` / `S NSFS`
 * resets it -- the registry is STC-private storage, so a recycle is enough.
 *
 * THE IDENTITY IS WTOed, not printed.  A cancelled job's SYSPRINT is lost
 * (the M4-5 lesson); the console log survives.  It is also the cross-check the
 * measurement depends on: the ASCB/ASID the report shows must be the pair this
 * job announced, or the reading is about some other address space.
 */

#include "nsfeza.h"
#include "nsfreqc.h"
#include "nsfsoc.h"         /* NSF_AF_INET / NSF_SOCK_DGRAM                    */

#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1), __ascb              */
#include <clibecb.h>        /* ecb_timed_wait (the pause)                      */
#include <clibwto.h>        /* wtof -- survives a CANCEL, SYSPRINT does not    */
#include <mbtcheck.h>
#include <stdio.h>
#include <stdlib.h>         /* malloc/free -- a private-storage address only    */
#include <string.h>

#define APPD_ASCBASID   0x24U       /* ASCBASID halfword in the ASCB           */
#define APPD_CC_NORUN   20          /* "could not run" -- never 0, never 1     */
#define APPD_PARK_PORT  7799U       /* PARK: bound, and nothing sends to it    */

/* HANG: how long to stay alive waiting to be cancelled, and how often to say
 * so.  Bounded on purpose -- see the header. */
#define APPD_HANG_STEP_HSEC   3000U /* 30 s per pause                          */
#define APPD_HANG_STEPS       30U   /* 30 * 30 s = 15 minutes                  */

static UINT appd_own_asid(void *ascb)
{
    if (ascb == NULL) {
        return 0u;
    }
    return (UINT)(*(unsigned short *)((char *)ascb + APPD_ASCBASID));
}

static void appd_pause(unsigned hsec)
{
    ECB local = 0;                      /* nobody posts it; the timer expires  */
    ecb_timed_wait(&local, hsec, 0);
}

/* PARM comes in as argv[1] (crt1); absent means the default role. */
static int parm_is(int argc, char **argv, const char *want)
{
    if (argc < 2 || argv[1] == NULL) {
        return 0;
    }
    return (strcmp(argv[1], want) == 0) ? 1 : 0;
}

int main(int argc, char **argv)
{
    void *own_ascb;
    UINT  own_asid;
    int   hang  = parm_is(argc, argv, "HANG");
    int   clean = parm_is(argc, argv, "CLEAN");
    int   park  = parm_is(argc, argv, "PARK");
    int   rc;
    int   s;
    UINT  i;

    printf("=== nsf370 M5-2c1 app-death arm (TSTAPPD) ===\n");
    printf("  role: %s\n",
           park ? "PARK" : (hang ? "HANG" : (clean ? "CLEAN" : "LEAVE")));

    /* The red line every cross-AS client in this milestone asserts: an
     * application needs no APF library to reach the stack (ADR-0038). */
    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    own_ascb = __ascb(0);
    own_asid = appd_own_asid(own_ascb);
    CHECK(own_ascb != NULL, "own ASCB located (PSAAOLD)");
    CHECK(own_asid != 0u,   "own ASID read from ASCBASID (ASCB+X'24')");

    rc = nsfreqc_init();
    if (rc != 0) {
        printf("  NSFS STC not reachable -- is it started? (S NSFS)\n");
        wtof("TSTAPPD: NSFS NOT REACHABLE (RC=%d) -- ARM DID NOT RUN", rc);
        return APPD_CC_NORUN;           /* not a pass and not a failure        */
    }

    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTAPPD ", NULL);
    CHECK(rc >= 0, "INITAPI across the boundary");
    if (rc < 0) {
        /* Sixteen leaked slots and the seventeenth INITAPI fails.  Say which
         * failure this is rather than letting it read as a guard defect. */
        wtof("TSTAPPD: INITAPI FAILED (ERRNO=%d) -- APP REGISTRY FULL?"
             " RECYCLE NSFS", (int)nsf_lasterrno());
        return APPD_CC_NORUN;
    }

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "SOCKET across the boundary returns a descriptor");

    /* THE ANNOUNCEMENT.  Everything the operator needs to tie the report's
     * line to this job, on the console, before the job can end. */
    wtof("TSTAPPD: %s ARM -- ASCB=%08X ASID=%04X SOCKET=%d",
         park ? "PARK" : (hang ? "HANG" : (clean ? "CLEAN" : "LEAVE")),
         (unsigned)own_ascb, (unsigned)own_asid, s);

    /* PRIVATE-STORAGE ADDRESSES, for 40-CHK 2.3.  A stack address and a heap
     * address bracket this job's private region, so the reply ECB's recorded
     * address can be compared against where this address space's storage
     * actually is -- rather than against an assumption about where it is. */
    {
        char  onstack = 0;
        void *onheap  = malloc(256);

        wtof("TSTAPPD: PRIVATE STACK=%08X HEAP=%08X",
             (unsigned)&onstack, (unsigned)onheap);
        if (onheap != NULL) {
            free(onheap);
        }
    }

    if (clean) {
        /* THE CONTROL: end the way an application is supposed to. */
        rc = nsf_termapi();
        CHECK_EQ((long)rc, 0L, "TERMAPI across the boundary");
        wtof("TSTAPPD: CLEAN ARM -- TERMAPI DONE, REGISTRY SHOULD BE EMPTY");
        return mbt_test_summary("TSTAPPD");
    }

    if (park) {
        /* THE 40-CHK INDUCTION.  Bind a port nothing sends to, then block in
         * RECVFROM: the request is published, the STC parks it in the socket
         * layer, and this task waits in the SVC routine on the CSA reply ECB.
         * That is the state to be CANCELled in.
         *
         * IT DOES NOT RETURN in the arm's intended use -- the operator cancels
         * it.  The bind port is fixed so the datagram that later COMPLETES the
         * parked request (and so drives the STC to the guard and the POST) can
         * be aimed at it from the host. */
        NSF_SOCKADDR_IN me;
        NSF_SOCKADDR_IN from;
        char            rbuf[64];
        int             flen = (int)sizeof(from);

        memset(&me, 0, sizeof(me));
        me.sin_family = NSF_AF_INET;
        me.sin_port   = APPD_PARK_PORT;
        me.sin_addr   = 0;
        rc = nsf_bind(s, &me, sizeof(me));
        CHECK_EQ((long)rc, 0L, "BIND across the boundary");
        if (rc != 0) {
            wtof("TSTAPPD: PARK ARM -- BIND FAILED (ERRNO=%d)",
                 (int)nsf_lasterrno());
            return APPD_CC_NORUN;
        }

        wtof("TSTAPPD: PARK ARM -- BLOCKING RECVFROM ON PORT %u,"
             " ISSUE C <jobname> NOW", (unsigned)APPD_PARK_PORT);

        rc = nsf_recvfrom(s, rbuf, (int)sizeof(rbuf), 0, &from, &flen);

        /* Reached only if the RECVFROM completed -- i.e. the arm was NOT
         * cancelled and a datagram (or an error) arrived first.  Say so
         * loudly: a completed RECVFROM is not the induction, and a run that
         * ends here must not be read as one that was cancelled while parked. */
        wtof("TSTAPPD: PARK ARM RETURNED rc=%d ERRNO=%d -- NOT CANCELLED"
             " WHILE PARKED; THE INDUCTION DID NOT RUN",
             rc, (int)nsf_lasterrno());
        (void)nsf_termapi();
        return APPD_CC_NORUN;
    }

    if (hang) {
        /* Stay alive to be CANCELled.  Bounded, and a run that is NOT
         * cancelled must not be mistaken for the arm -- so it cleans up and
         * says on the console that it did. */
        wtof("TSTAPPD: HANG ARM -- ALIVE FOR UP TO %u MIN, ISSUE C <jobname>",
             (unsigned)(APPD_HANG_STEPS / 2u));
        for (i = 0u; i < APPD_HANG_STEPS; i++) {
            appd_pause(APPD_HANG_STEP_HSEC);
            wtof("TSTAPPD: HANG ARM ALIVE (%u/%u) ASID=%04X",
                 (unsigned)(i + 1u), (unsigned)APPD_HANG_STEPS,
                 (unsigned)own_asid);
        }
        (void)nsf_termapi();
        wtof("TSTAPPD: HANG ARM TIMED OUT -- NOT CANCELLED, CLEANED UP."
             " THE CANCEL ARM DID NOT RUN");
        return APPD_CC_NORUN;
    }

    /* THE MAIN CASE.  Return with the app instance and its socket still
     * registered: no TERMAPI, no close, nothing.  The job ends normally. */
    wtof("TSTAPPD: LEAVE ARM -- ENDING WITHOUT TERMAPI, SLOT LEFT IN USE");
    return mbt_test_summary("TSTAPPD");
}
