/*----------------------------------------------------------------------------
 * tstd1a.c -- M5-2d1 live gate 2.1: THE CROSS-AS ACCEPT.  MVS-only.
 *
 * THIS IS THE FIRST CASE OF THE ROUND AND EVERYTHING ELSE DEPENDS ON IT.
 *
 * d1 put an ownership check on every descriptor resolution: a socket resolves
 * only if the app slot it names records the address space the SVC routine
 * captured from the FLIH.  An ACCEPTED TCP CHILD is the case that breaks if
 * that check is one degree too strict -- the child is created by the STACK, in
 * tcp_child_create, and inherits the listener's apptok (src/nsftcp.c:1407); the
 * client never named it in a SOCKET call.  If the check refuses it, EVERY
 * ACCEPTED CONNECTION BREAKS, which is every server this stack exists for.
 *
 * WHY THIS COULD NOT BE PROVEN HOST-SIDE, and why d1 shipped with it open:
 * Phase 1 dispatches with a ZERO identity, so the check is INERT there.
 * TSTTCP's real accept path is green at 841 assertions and says nothing about
 * this, and TSTREQ's inherited-child case stamps a socket BY HAND to model
 * tcp_child_create rather than producing one.  Only a real accept, from a real
 * second address space, exercises the check on a child.
 *
 * The client is UNAUTHORISED and every verb below executes in the NSFS address
 * space.  The gate is not "accept returned a descriptor" -- it is that a VERB
 * ON THE CHILD SUCCEEDS, because that is the call the check adjudicates.
 *
 * PEER: samples/host/d1accept_peer.py must be running on the host:
 *   python3 samples/host/d1accept_peer.py 192.168.200.1 3009
 *
 * Return codes: 0 all passed, 1 ran and failed, 20 could not run (no NSFS).
 *--------------------------------------------------------------------------*/
#include "nsfeza.h"
#include "nsfsoc.h"     /* NSF_AF_INET / NSF_SOCK_STREAM                      */
#include "nsfreq.h"
#include "nsfreqc.h"
#include <mbtcheck.h>
#include <clibos.h>
#include <clibecb.h>    /* ecb_timed_wait -- the poll pause (tstrqxc pattern) */
#include <stdio.h>
#include <string.h>

#define D1A_PORT     3009u
#define D1A_SRC      0xC0A8C801u        /* 192.168.200.1 -- the guest         */
#define D1A_TRIES    150                /* ~30 s at 200 ms                    */
#define D1A_CC_SKIP  20

static char g_buf[128];

/* Wait out `hsec` hundredths without a busy loop: a local ECB nobody posts,
 * satisfied by the timer.  Lifted from tstrqxc.c's xc_pause. */
static void d1a_pause(unsigned hsec)
{
    ECB local = 0;

    ecb_timed_wait(&local, hsec, 0);
}

static void mk_sa(NSF_SOCKADDR_IN *sa, UINT addr, USHORT port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = NSF_AF_INET;
    sa->sin_port   = port;
    sa->sin_addr   = addr;
}

int main(void)
{
    NSF_SOCKADDR_IN local, peer, gp;
    INT  rc, lsn, conn, n, namelen, on = 1;
    int  i;

    printf("=== nsf370 M5-2d1 live gate 2.1: the cross-AS ACCEPT ===\n");
    wtof("TSTD1A: CROSS-AS ACCEPT GATE START");

    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    rc = nsfreqc_init();
    if (rc != 0) {
        printf("  NSFS not reachable -- is it started? (S NSFS)\n");
        wtof("TSTD1A: NO NSFS -- GATE SKIPPED");
        return D1A_CC_SKIP;             /* could not run != passed (8.5)      */
    }
    CHECK_EQ((long)rc, 0L, "cross-AS transport registered (NSFS anchor found)");

    /* ---- every call from here executes in the NSFS address space ---------- */
    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTD1A  ", NULL);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "INITAPI across the boundary");

    lsn = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    CHECK(lsn >= 0, "SOCKET(STREAM) across the boundary");

    mk_sa(&local, D1A_SRC, (USHORT)D1A_PORT);
    rc = nsf_bind(lsn, &local, (INT)sizeof(local));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "BIND across the boundary");

    rc = nsf_listen(lsn, 5);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "LISTEN across the boundary");
    wtof("TSTD1A: LISTENING ON %u -- WAITING FOR THE HOST PEER", (unsigned)D1A_PORT);

    /* Non-blocking accept in a bounded retry loop: SELECT is gate 2.3's
     * subject, so it is kept out of THIS gate to keep the axis single -- if
     * 2.1 fails it must be the accept, not the multiplexer. */
    (void)nsf_ioctl(lsn, (INT)NSF_FIONBIO, &on);
    conn = -1;
    for (i = 0; i < D1A_TRIES; i++) {
        namelen = 0;
        conn = nsf_accept(lsn, &peer, &namelen);
        if (conn >= 0) break;
        d1a_pause(20);                              /* 200 ms                 */
    }
    CHECK(conn >= 0, "ACCEPT returned a child descriptor (the host connected)");
    if (conn < 0) {
        printf("  no connection within %d tries -- is d1accept_peer.py running?\n",
               D1A_TRIES);
        wtof("TSTD1A: NO PEER CONNECTED -- GATE INCONCLUSIVE");
        (void)nsf_close(lsn);
        (void)nsf_termapi();
        return D1A_CC_SKIP;
    }
    wtof("TSTD1A: ACCEPTED CHILD %d", (int)conn);

    /* ---- THE GATE.  A verb on the INHERITED child, which the client never
     * named in a SOCKET call and whose apptok the STACK copied from the
     * listener.  This is the call the ownership check adjudicates, and a check
     * one degree too strict refuses it. ----------------------------------- */
    namelen = 0;
    rc = nsf_getpeername(conn, &gp, &namelen);
    CHECK_EQ((long)rc, (long)NSF_RETOK,
             "GETSOCKNAME/PEER on the INHERITED child is permitted");
    if (rc != NSF_RETOK) {
        printf("  errno=%d -- THE CHECK REFUSED AN ACCEPTED CHILD\n",
               (int)nsf_lasterrno());
        wtof("TSTD1A: CHILD REFUSED -- errno %d", (int)nsf_lasterrno());
    }

    n = nsf_recv(conn, g_buf, (INT)sizeof(g_buf), 0);
    CHECK(n > 0, "RECV on the inherited child returned data");
    wtof("TSTD1A: RECV n=%d", (int)n);
    if (n > 0) {
        rc = nsf_send(conn, g_buf, n, 0);
        CHECK_EQ((long)rc, (long)n, "SEND echoes it back on the same child");
    }

    rc = nsf_close(conn);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "CLOSE the child");
    rc = nsf_close(lsn);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "CLOSE the listener");
    rc = nsf_termapi();
    CHECK_EQ((long)rc, (long)NSF_RETOK, "TERMAPI");

    wtof("TSTD1A: CROSS-AS ACCEPT GATE DONE");
    return mbt_test_summary("TSTD1A");
}
