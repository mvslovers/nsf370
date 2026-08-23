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
 * THE NAMED CASE (obligation #2, ADR-0039 -> ADR-0041 2): a sendto with
 * ulen > 2048 must report the count ACTUALLY MOVED, not the count requested.
 * That single assertion is what lifts obligation #2 from "designed and
 * host-pinned" to "proven".
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

static char g_big[6000];

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

    /* ---- a small send: the ordinary case --------------------------------- */
    for (i = 0u; i < 100u; i++) g_big[i] = (char)pat(i);
    rc = nsf_sendto(s, g_big, 100, 0, &peer, sizeof(peer));
    if (rc < 0) {
        /* Make the diagnosis self-documenting rather than inferred. The data
         * moves need a live interface: with the CTCI pair down there is no
         * route, sendto fails EHOSTUNREACH, and the moved-count assertions
         * below cannot be evaluated at all -- that is a MISSING PREREQUISITE,
         * not a transport fault. Every non-data verb above has already
         * crossed successfully by this point. */
        printf("  NOTE: SENDTO failed rc=%d errno=%d -- is the CTCI pair up?\n",
               (int)rc, (int)nsf_lasterrno());
        printf("  NOTE: the moved-count case (obligation #2) needs a live"
               " interface; the crossing itself is already proven above.\n");
    }
    CHECK_EQ((long)rc, 100L,
             "SENDTO 100 bytes reports 100 moved (data crossed the boundary)");

    /* ---- THE NAMED CASE: ulen > 2048 ------------------------------------- *
     * The transport stages min(ulen, 2048) per SVC call, and the dispatcher is
     * handed THAT count -- so the op reports what actually crossed. If ulen
     * reached the dispatcher unclamped, this would come back 5000 (an
     * over-report) and the op would have read past the staging buffer.
     * M5-2a moves ONE chunk; a short move is the honest answer here. */
    for (i = 0u; i < 5000u; i++) g_big[i] = (char)pat(i);
    rc = nsf_sendto(s, g_big, 5000, 0, &peer, sizeof(peer));
    CHECK_EQ((long)rc, (long)NSFREQX_CHUNK,
             "SENDTO 5000 reports the STAGED count (2048), never 5000");
    CHECK(rc != 5000,
          "the requested length is NOT reported as moved (no over-report)");

    /* An exact-fit send must NOT be clamped -- the boundary case either side. */
    rc = nsf_sendto(s, g_big, (int)NSFREQX_CHUNK, 0, &peer, sizeof(peer));
    CHECK_EQ((long)rc, (long)NSFREQX_CHUNK,
             "SENDTO of exactly one chunk reports the full chunk");

    /* A zero-length datagram is legal and must stay zero-length. */
    rc = nsf_sendto(s, g_big, 0, 0, &peer, sizeof(peer));
    CHECK_EQ((long)rc, 0L, "SENDTO 0 reports 0 moved");

    /* ---- an ERROR result must cross too, not just a success --------------- */
    rc = nsf_sendto(999, g_big, 10, 0, &peer, sizeof(peer));
    CHECK(rc < 0, "SENDTO on a bad descriptor fails across the boundary");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF,
             "the specific errno (EBADF) travelled back, not a generic failure");

    /* ---- teardown --------------------------------------------------------- */
    rc = nsf_close(s);
    CHECK_EQ((long)rc, 0L, "CLOSE across the boundary");
    rc = nsf_termapi();
    CHECK_EQ((long)rc, 0L, "TERMAPI across the boundary");

    return mbt_test_summary("TSTRQXM");
}
