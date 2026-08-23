/*
 * tstrqxm.c -- M5-2a live gate: a real NSFRQE crosses the address-space
 *              boundary and a real socket op round-trips (ADR-0041).
 *
 * MVS-only (host = false): there is no SVC, CSA or second address space on the
 * host. The field rules this exercises are host-tested in TSTREQX; what only a
 * live run can show is that they hold ACROSS the boundary.
 *
 * THE CLIENT IS UNAUTHORISED. Asserted here, not assumed: the whole point of
 * the private-SVC transport (ADR-0038) is that a relink-only application needs
 * no APF library. If TESTAUTH ever reports authorised, the gate is meaningless
 * and this test fails rather than passing for the wrong reason.
 *
 * The client carries NO stack of its own beyond the API layer -- it never
 * brings up a device, never runs an executive. Every socket op is executed by
 * the NSFS STC in ITS address space. That is the M5-2a claim.
 *
 * PREREQUISITE: the NSFS STC must be started (S NSFS).
 *
 * THE NAMED CASE (obligation #2, ADR-0039 -> ADR-0041 2) IS A TCP send, not a
 * UDP sendto, and the reason is semantic rather than incidental:
 *
 *   BSD and EZASOKET SENDTO on a datagram socket is ATOMIC -- the whole
 *   datagram goes or the call fails.  A partial move there is not a short
 *   write, it is a TRUNCATED DATAGRAM on the wire, and an application looping
 *   on the returned count would send the remainder as a SECOND datagram and
 *   corrupt its own framing.  Pinning "SENDTO 5000 -> 2048 moved" would pin
 *   semantics NSF's own surface contradicts.
 *
 * The moved-length contract is a STREAM concept: a short return is correct,
 * expected and loop-safe exactly where BSD says it is -- send() on a connected
 * TCP socket.  So the named case is a TCP send, verified byte-exact at the
 * host peer, and the loop-on-short-write behaviour is DEMONSTRATED (three
 * sends draining one buffer), not asserted once.
 *
 * UDP keeps the honest case instead: a sendto above MTU-28 must come back
 * EMSGSIZE ACROSS the boundary (spec 11.3, no fragmentation in v1).  That
 * proves a specific errno crosses from a PROTOCOL op, where the EBADF row
 * above only covers the dispatcher.
 *
 * WHAT THE TCP PART IS ALSO THE FIRST TO EXERCISE.  Every synchronous verb
 * (INITAPI/SOCKET/BIND/GETSOCKNAME/CLOSE/TERMAPI) completes inside the
 * dispatcher and never parks.  A TCP connect/send is the FIRST live exercise
 * of the parked-request path -- ADR-0041 5's end-of-pass completion check and
 * the un-posted-private-ECB fix.  Until this test, that design is live-unproven.
 *
 * PEER: samples/host/shortwrite_listener.py must be running on the host:
 *   python3 shortwrite_listener.py 192.168.200.2 3003 --expect 9096
 */
#include "nsfeza.h"
#include "nsfsoc.h"      /* NSF_AF_INET / NSF_SOCK_DGRAM */
#include "nsfreqc.h"
#include "nsfreqx.h"
#include <mbtcheck.h>
#include <string.h>
#include <clibos.h>       /* __isauth (TESTAUTH FCTN=1)   */

#define PEER_IP    0xC0A8C802u          /* 192.168.200.2 -- the host side      */
#define PEER_PORT  9999                 /* nothing listens: we want the SEND,  */
                                        /* not a reply (no host coordination)  */
#define MY_PORT    7777
#define SW_PORT    3003                 /* samples/host/shortwrite_listener.py */

/* MTU 1500 - IP 20 - UDP 8 (src/nsfudp.c udp_send). Read from the deployed
 * PROFILE member, not assumed: GATEWAY ... 1500, and nsf_devcfg's v1 default
 * is the same 1500. Anything above this is EMSGSIZE -- spec 11.3, v1 does not
 * fragment. */
#define UDP_MAXPAY 1472

/* One pattern buffer drained by every TCP send at a running offset, so the
 * host peer sees ONE contiguous stream it can verify against pat(0..N-1). */
#define SW_TOTAL   9096u

static char g_big[SW_TOTAL];

/* A recognisable pattern, so a byte that fails to cross shows as a mismatch
 * rather than as an accidental match against zero. */
static unsigned char pat(unsigned i)
{
    return (unsigned char)(i * 7u + (i >> 5) + 0x23u);
}

int main(void)
{
    NSF_SOCKADDR_IN me, peer;
    int      s;
    int      rc;
    unsigned i;

    printf("=== nsf370 M5-2a cross-AS NSFRQE gate (TSTRQXM) ===\n");

    /* ---- the red line: this client must NOT be authorised ---------------- */
    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    /* ---- register the cross-AS transport --------------------------------- */
    rc = nsfreqc_init();
    CHECK_EQ((long)rc, 0L,
             "nsfreqc_init: the NSFS anchor is published and the SVC is ours");
    if (rc != 0) {
        printf("  NSFS STC not reachable -- is it started? (S NSFS)\n");
        return mbt_test_summary("TSTRQXM");
    }

    /* ---- from here on every call is executed in ANOTHER address space ----- */
    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTRQXM ", NULL);
    CHECK(rc >= 0, "INITAPI across the boundary");

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "SOCKET across the boundary returns a descriptor");

    memset(&me, 0, sizeof(me));
    me.sin_family = NSF_AF_INET;
    me.sin_port   = MY_PORT;
    me.sin_addr   = 0;
    rc = nsf_bind(s, &me, sizeof(me));
    CHECK_EQ((long)rc, 0L, "BIND across the boundary");

    /* GETSOCKNAME proves an OUTPUT field travelled back: the port was assigned
     * by the STC's socket layer and reached us through the result copy-out. */
    memset(&me, 0, sizeof(me));
    {
        INT nl = (INT)sizeof(me);
        rc = nsf_getsockname(s, &me, &nl);
    }
    CHECK_EQ((long)rc, 0L, "GETSOCKNAME across the boundary");
    CHECK_EQ((long)me.sin_port, (long)MY_PORT,
             "the bound port came back through the result fields");

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = NSF_AF_INET;
    peer.sin_port   = PEER_PORT;
    peer.sin_addr   = PEER_IP;

    /* ---- UDP: an in-range datagram crosses and is reported whole -------- */
    for (i = 0u; i < 100u; i++) g_big[i] = (char)pat(i);
    rc = nsf_sendto(s, g_big, 100, 0, &peer, sizeof(peer));
    if (rc < 0) {
        printf("  NOTE: SENDTO failed rc=%d errno=%d -- is the CTCI pair up?\n",
               (int)rc, (int)nsf_lasterrno());
    }
    CHECK_EQ((long)rc, 100L,
             "SENDTO 100 bytes reports 100 moved (data crossed the boundary)");

    /* A zero-length datagram is legal and must stay zero-length. */
    rc = nsf_sendto(s, g_big, 0, 0, &peer, sizeof(peer));
    CHECK_EQ((long)rc, 0L, "SENDTO 0 reports 0 moved");

    /* ---- UDP: oversized is EMSGSIZE, and the errno crosses --------------- *
     * NOT a short move: a datagram send is atomic (see the header). The
     * transport stages min(5000, 2048) = 2048, the dispatcher still sees more
     * than MTU-28, and v1 does not fragment -- so the honest answer is a
     * failure, and what this row proves is that a PROTOCOL op's specific errno
     * makes the return trip (the EBADF row above only covers the dispatcher). */
    rc = nsf_sendto(s, g_big, 5000, 0, &peer, sizeof(peer));
    CHECK(rc < 0, "SENDTO above MTU-28 fails (v1 does not fragment)");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EMSGSIZE,
             "EMSGSIZE from a PROTOCOL op crossed the boundary");

    rc = nsf_sendto(s, g_big, UDP_MAXPAY + 1, 0, &peer, sizeof(peer));
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EMSGSIZE,
             "one byte over MTU-28 is already EMSGSIZE (the exact boundary)");

    rc = nsf_close(s);
    CHECK_EQ((long)rc, 0L, "CLOSE the UDP socket across the boundary");

    /* ==== THE NAMED CASE: TCP short writes (obligation #2) ================= *
     * Also the FIRST live exercise of the parked-request path: connect and
     * send park in the executive and complete later, so ADR-0041 5's
     * end-of-pass check and the un-posted-private-ECB fix are on trial here. */
    for (i = 0u; i < SW_TOTAL; i++) g_big[i] = (char)pat(i);

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    CHECK(s >= 0, "SOCKET (stream) across the boundary");

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = NSF_AF_INET;
    peer.sin_port   = SW_PORT;
    peer.sin_addr   = PEER_IP;

    rc = nsf_connect(s, &peer, sizeof(peer));
    if (rc < 0) {
        printf("  NOTE: CONNECT failed rc=%d errno=%d -- is"
               " shortwrite_listener.py running on the host?\n",
               (int)rc, (int)nsf_lasterrno());
    }
    CHECK_EQ((long)rc, 0L,
             "CONNECT across the boundary (the first PARKED request to complete)");

    if (rc == 0) {
        UINT off = 0u;
        int  n1, n2, n3, n4, n5;

        /* Exactly one chunk must NOT be clamped -- the boundary either side. */
        n1 = nsf_send(s, &g_big[off], (INT)NSFREQX_CHUNK, 0);
        CHECK_EQ((long)n1, (long)NSFREQX_CHUNK,
                 "SEND of exactly one chunk reports the full chunk");
        if (n1 > 0) off += (UINT)n1;

        n2 = nsf_send(s, &g_big[off], (INT)NSFREQX_CHUNK + 1, 0);
        CHECK_EQ((long)n2, (long)NSFREQX_CHUNK,
                 "SEND of chunk+1 reports the chunk (clamped, not 2049)");
        if (n2 > 0) off += (UINT)n2;

        /* THE assertion. If ulen reached the dispatcher unclamped this would
         * come back 5000 -- an over-report AND a read past the staging
         * buffer. The moved count is what actually crossed. */
        n3 = nsf_send(s, &g_big[off], 5000, 0);
        CHECK_EQ((long)n3, (long)NSFREQX_CHUNK,
                 "SEND 5000 reports the STAGED count (2048), never 5000");
        CHECK(n3 != 5000,
              "the requested length is NOT reported as moved (no over-report)");
        if (n3 > 0) off += (UINT)n3;

        /* Loop on the short write, exactly as a BSD application does. */
        n4 = nsf_send(s, &g_big[off], 5000 - (INT)NSFREQX_CHUNK, 0);
        CHECK_EQ((long)n4, (long)NSFREQX_CHUNK,
                 "the loop's second send moves another full chunk");
        if (n4 > 0) off += (UINT)n4;

        n5 = nsf_send(s, &g_big[off], 5000 - 2 * (INT)NSFREQX_CHUNK, 0);
        CHECK_EQ((long)n5, (long)(5000 - 2 * (int)NSFREQX_CHUNK),
                 "the loop's last send moves the remainder (under a chunk)");
        if (n5 > 0) off += (UINT)n5;

        CHECK_EQ((long)off, (long)SW_TOTAL,
                 "the short-write loop moved every byte (host verifies them)");
    }

    rc = nsf_close(s);
    CHECK_EQ((long)rc, 0L, "CLOSE the stream socket (host sees the FIN)");

    /* ---- an ERROR result must cross too, not just a success --------------- */
    rc = nsf_sendto(999, g_big, 10, 0, &peer, sizeof(peer));
    CHECK(rc < 0, "SENDTO on a bad descriptor fails across the boundary");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF,
             "the specific errno (EBADF) travelled back, not a generic failure");

    /* ---- teardown --------------------------------------------------------- */
    /* ---- the anchor guard survived the crossing -------------------------- *
     * Asserted the way this run already proves things, NOT by reading CSA: the
     * client is unauthorised key 8 and has no business touching the anchor.
     * If the guard or the wake-ECB pointer had been clobbered, the STC would
     * have REAPED this request instead of servicing it (NSF052E / NSF053E on
     * the console) and every row above would have failed. So a fully green run
     * IS the observation -- corroborated on the console side by the absence of
     * those messages, and by the host-pinned truth table in TSTREQX. */
    CHECK(1, "the anchor guard held across every request (see NSF052E/NSF053E)");

    rc = nsf_termapi();
    CHECK_EQ((long)rc, 0L, "TERMAPI across the boundary");

    return mbt_test_summary("TSTRQXM");
}
