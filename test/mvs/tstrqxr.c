/*
 * tstrqxr.c -- 80-CHK: does a data-returning cross-AS receive store into
 *              key-0 CSA from the executive's key 8?  (issue #80)
 *
 * MVS-only (host = false): there is no SVC, no CSA and no second address space
 * on the host, and the property under test is a STORAGE KEY -- which the host
 * build does not have at all.
 *
 * THIS TEST FIXES NOTHING.  It is a probe, and it is built to produce a
 * DISCRIMINATING result rather than merely an abend.
 *
 * ---------------------------------------------------------------------------
 * THE CHAIN (re-derived from source, not taken from the issue)
 * ---------------------------------------------------------------------------
 *
 *   1. src/nsfreqx.c:62      priv->ubuf = stage;
 *      -- the STC-private NSFRQE's ubuf is pointed at THIS SLOT's staging
 *         buffer, which lives inside the CSA anchor taken by
 *         getmain(sizeof(NSFV_ANCHOR), 241): subpool 241, KEY 0.
 *
 *   2. src/nsfsx.c:1138-1152 the dispatch runs AFTER __prob(savekey, NULL)
 *      -- outside the key window, by design and with the reason in a comment:
 *         "the executive runs in its own key 8 on ordinary storage, exactly as
 *         in Phase 1."
 *
 *   3. src/nsfudp.c:200      got = buf_copyout(bpay, r->ubuf, want);
 *      src/nsfbuf.c:285      memcpy(d + total, b->data, take);
 *      -- THE STORE.  A key-8 store into key-0 storage faults S0C4; a key-8
 *         FETCH of the same storage succeeds, because CSA is not
 *         fetch-protected (measured, M5-2b0 / ADR-0039 annotation).
 *
 * That asymmetry is exactly why the SEND direction has always worked: a send
 * READS ubuf.  TSTRQXM's 9353 bytes go that way.  A receive that returns data
 * is the mirror image and has never been driven by any test in this tree.
 *
 * A grep for __super / __prob / SPKA / PSWKEY0 across the whole protocol layer
 * returns NOTHING: every other CSA write in this design sits inside a key
 * window, and this one store does not.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS A CONTROL, AND WHY IT IS THIS ONE
 * ---------------------------------------------------------------------------
 *
 * "NSFS abended S0C4" on its own does not separate K(i) (the store faults)
 * from K(iii) (it failed some other way -- a bad pointer, a broken request, a
 * fault somewhere else entirely).  The control has to differ from the arm by
 * the STORE and by nothing else.
 *
 * It does, and it is free, because udp_complete_recv guards the copy:
 *
 *     if (r->ubuf != NULL && r->ulen > 0u) {
 *         want = (paylen < (USHORT)r->ulen) ? paylen : (USHORT)r->ulen;
 *         got  = buf_copyout(bpay, r->ubuf, want);
 *     }
 *
 * A ZERO-LENGTH DATAGRAM gives paylen == 0, hence want == 0.  The guard is
 * still TRUE (the client asked for RECV_LEN bytes, so r->ulen > 0), the call
 * is still made, and buf_copyout's own loop -- `while (b != NULL && total < n)`
 * -- simply never runs.  Same request shape, same crossing, same function,
 * same call: only the memcpy is elided.
 *
 * So the pair is one line apart on one code path:
 *
 *     zero-length datagram -> buf_copyout(..., 0) -> no memcpy -> completes
 *     N-byte datagram      -> buf_copyout(..., N) -> memcpy    -> ?
 *
 * If the zero case completes and the N case does not, the store is the
 * difference, and nothing else is.
 *
 * ---------------------------------------------------------------------------
 * THE CLIENT WILL HANG, SO THE EVIDENCE IS ON THE CONSOLE
 * ---------------------------------------------------------------------------
 *
 * When the STC abends mid-request this client is parked in WAIT on SLRECB in
 * the retained CSA anchor, and recovery does NOT nudge parked clients --
 * nsfsx_recover_quiesce restores the SVC slot and clears ANCHOR_ACTIVE, and
 * that is all.  Nobody will ever POST it.  The job must be cancelled, and the
 * S222 takes buffered SYSPRINT with it (the M4-5 lesson).
 *
 * Therefore every step is marked with wtof().  A marker that is PRESENT and
 * its successor ABSENT pins the fault to the call between them, and survives
 * the cancel.  printf is kept for the case where the run completes.
 *
 * ONE SHOT PER STC INSTANCE: the fault kills NSFS, so the arm below runs once.
 * The inline shape (data already queued when the RECV is issued) needs no
 * separate run to reason about -- udp_complete_recv is documented in its own
 * header as "shared by the parked-RECV and rxq-dequeue paths", so both shapes
 * reach the identical store.  This test drives the PARKED shape, because the
 * host peer's delay makes it deterministic with no guest-side timing.
 *
 * OPT-IN.  A bare run does nothing and returns CC 20; the arm needs
 * PARM='ARM' (jcl/TSTRQXR.jcl).  See the guard at the top of main.
 *
 * PEER: samples/host/recvkey_peer.py must be running on the host:
 *   python3 recvkey_peer.py 192.168.200.2 3004 --len 256
 */
#include "nsfeza.h"
#include "nsfsoc.h"          /* NSF_AF_INET / NSF_SOCK_DGRAM                   */
#include "nsfreqc.h"
#include "nsfreqx.h"         /* NSFREQX_CHUNK                                  */
#include <mbtcheck.h>
#include <string.h>
#include <clibos.h>          /* __isauth (TESTAUTH FCTN=1)                     */
#include <clibwto.h>         /* wtof -- console markers survive the cancel     */

#define PEER_IP    0xC0A8C802u          /* 192.168.200.2 -- the host side      */
#define PEER_PORT  3004                 /* samples/host/recvkey_peer.py        */
#define MY_PORT    7788

/* The client asks for more than the peer sends, so r->ulen is comfortably
 * non-zero on BOTH the control and the arm -- see the ulen note below. */
#define RECV_LEN   512
#define DATA_LEN   256                  /* one whole MVC piece, and > 0        */

/* CC 20: the gate could not run.  Distinct from 0 (ran and passed) and 1 (ran
 * and failed) so a run that never reached the arm cannot be read as evidence
 * about it -- CLAUDE.md 8.5. */
#define XR_CC_GATE_SKIPPED 20

static char g_rx[RECV_LEN];

/* Same generator as the host peer. */
static unsigned char pat(unsigned i)
{
    return (unsigned char)(i * 7u + (i >> 5) + 0x23u);
}

/* Ask the peer for the next reply.  The trigger doubles as a liveness proof of
 * the SEND direction: if these stop working the run is not about a receive. */
static int trigger(int s, const NSF_SOCKADDR_IN *peer, const char *what)
{
    char msg[8];

    memset(msg, 0, sizeof(msg));
    memcpy(msg, what, 2);
    return nsf_sendto(s, msg, 2, 0, peer, (INT)sizeof(*peer));
}

int main(int argc, char **argv)
{
    NSF_SOCKADDR_IN me, peer, from;
    int  s, rc, skipped = 1;
    int  nzero = -1, ndata = -1;
    INT  fromlen;
    unsigned i;

    /* OPT-IN, AND THE DEFAULT MUST NOT EVEN TOUCH THE TRANSPORT.
     *
     * Two independent reasons, either one sufficient:
     *   - the arm ABENDS NSFS, so an accidental run costs the STC and ~139 KB
     *     of CSA retained until the next IPL;
     *   - every receive below is BLOCKING with no timeout, so without the host
     *     peer answering the triggers this program HANGS holding an initiator
     *     and TESTLIB, and has to be cancelled (S222).
     *
     * So a bare run does nothing at all and says so, returning CC 20 -- "the
     * gate did not run", never 0.  A vacuous green here would be exactly the
     * failure class CLAUDE.md 8.5 is about. */
    if (argc < 2 || argv[1] == NULL || strncmp(argv[1], "ARM", 3) != 0) {
        /* MARK THE NOT-RUN BRANCH, and say WHY it was taken.  CC 20 alone
         * cannot distinguish "no PARM was given" from "argc never arrives
         * under crt1" or "the PARM arrives in a shape this compare misses" --
         * the guard would return 20 for all three and look identical.  So the
         * branch reports what it actually saw.  (CLAUDE.md 8.5, aimed at this
         * guard rather than at the code it guards.) */
        wtof("TSTRQXR: NOT RUN -- argc=%d argv1=%s (need PARM='ARM')",
             argc, (argc > 1 && argv[1] != NULL) ? argv[1] : "<none>");
        printf("=== TSTRQXR -- 80-CHK probe: NOT RUN ===\n");
        printf("  This probe is OPT-IN: it abends NSFS on purpose and blocks\n");
        printf("  on a host peer.  Run it with PARM='ARM' via jcl/TSTRQXR.jcl,\n");
        printf("  with samples/host/recvkey_peer.py listening.\n");
        printf("*** CC %d -- the gate did not run, this is NOT a pass.\n",
               XR_CC_GATE_SKIPPED);
        return XR_CC_GATE_SKIPPED;
    }

    wtof("TSTRQXR: 80-CHK CROSS-AS RECEIVE KEY PROBE START");
    printf("=== TSTRQXR -- 80-CHK: cross-AS receive, key-0 CSA store ===\n");

    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    rc = nsfreqc_init();
    CHECK_EQ((long)rc, 0L, "nsfreqc_init: the NSFS anchor is published");
    if (rc != 0) {
        wtof("TSTRQXR: GATE SKIPPED -- no NSFS transport");
        printf("  NSFS STC not reachable -- is it started? (S NSFS)\n");
        (void)mbt_test_summary("TSTRQXR");
        return XR_CC_GATE_SKIPPED;
    }

    /* ---- every call from here executes in the NSFS address space ---------- */
    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTRQXR ", NULL);
    CHECK(rc >= 0, "INITAPI across the boundary");

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "SOCKET (datagram) across the boundary");

    memset(&me, 0, sizeof(me));
    me.sin_family = NSF_AF_INET;
    me.sin_port   = MY_PORT;
    me.sin_addr   = 0;
    rc = nsf_bind(s, &me, sizeof(me));
    CHECK_EQ((long)rc, 0L, "BIND across the boundary");

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = NSF_AF_INET;
    peer.sin_port   = PEER_PORT;
    peer.sin_addr   = PEER_IP;

    /* ===================================================================== *
     * CONTROL 1 -- the SEND direction still works across the boundary.
     * A send READS ubuf out of key-0 CSA, which key 8 is permitted to do.
     * If this row fails the run is not about a receive at all.
     * ===================================================================== */
    wtof("TSTRQXR: control 1 -- SEND direction");
    rc = trigger(s, &peer, "R0");
    if (rc < 0) {
        printf("  NOTE: SENDTO failed rc=%d errno=%d -- CTCI pair up?"
               " peer running?\n", (int)rc, (int)nsf_lasterrno());
    }
    CHECK_EQ((long)rc, 2L, "SENDTO across the boundary still moves data OUT");
    if (rc != 2) {
        wtof("TSTRQXR: GATE SKIPPED -- send direction dead");
        (void)mbt_test_summary("TSTRQXR");
        return XR_CC_GATE_SKIPPED;
    }

    /* ===================================================================== *
     * CONTROL 2 -- a receive that returns ZERO bytes.
     * Traverses udp_complete_recv identically; buf_copyout is CALLED with
     * n == 0, so its loop never runs and the memcpy never happens.
     * PREDICTION: completes clean.
     * ===================================================================== */
    wtof("TSTRQXR: control 2 -- ZERO-BYTE RECV issued (no store)");
    memset(g_rx, 0, sizeof(g_rx));
    fromlen = (INT)sizeof(from);
    memset(&from, 0, sizeof(from));
    nzero = nsf_recvfrom(s, g_rx, RECV_LEN, 0, &from, &fromlen);
    wtof("TSTRQXR: control 2 -- ZERO-BYTE RECV RETURNED n=%d", nzero);

    CHECK_EQ((long)nzero, 0L,
             "a zero-length datagram completes across the boundary (no store)");
    CHECK_EQ((long)from.sin_addr, (long)PEER_IP,
             "the peer address came back through the result fields");

    /* ===================================================================== *
     * THE ARM -- a receive that returns DATA.
     * The ONLY difference from control 2 is that buf_copyout now has n > 0,
     * so memcpy runs, storing into slot->stage: subpool 241, key 0, from the
     * executive's key 8.
     *
     * ulen (advisor's point): the client asks for RECV_LEN and the transport
     * stages min(RECV_LEN, NSFREQX_CHUNK) -- non-zero either way, so a clean
     * completion here cannot be a silent zero-length no-op masquerading as
     * K(ii).  It is asserted below from the RETURNED COUNT, which is the only
     * honest evidence that the store actually ran.
     *
     * PREDICTION K(i): the marker after this call never appears.
     * ===================================================================== */
    rc = trigger(s, &peer, "R1");
    CHECK_EQ((long)rc, 2L, "the second trigger crossed (send still alive)");

    skipped = 0;                        /* the arm is now committed            */
    wtof("TSTRQXR: ARM -- DATA RECV ISSUED (len=%d, expecting %d bytes)",
         (int)RECV_LEN, (int)DATA_LEN);
    memset(g_rx, 0, sizeof(g_rx));
    fromlen = (INT)sizeof(from);
    memset(&from, 0, sizeof(from));
    ndata = nsf_recvfrom(s, g_rx, RECV_LEN, 0, &from, &fromlen);

    /* If the store faults in the STC, control never reaches this line. */
    wtof("TSTRQXR: ARM -- DATA RECV RETURNED n=%d errno=%d",
         ndata, (int)nsf_lasterrno());

    CHECK_EQ((long)ndata, (long)DATA_LEN,
             "the data-returning receive completed with the full count");

    if (ndata == (int)DATA_LEN) {
        int ok = 1;
        for (i = 0u; i < (unsigned)DATA_LEN; i++) {
            if ((unsigned char)g_rx[i] != pat(i)) { ok = 0; break; }
        }
        CHECK(ok, "every received byte is byte-exact (the store really ran)");
        wtof("TSTRQXR: ARM -- payload byte-exact=%d", ok);
    }

    rc = nsf_close(s);
    CHECK_EQ((long)rc, 0L, "CLOSE across the boundary");

    rc = nsf_termapi();
    CHECK_EQ((long)rc, 0L, "TERMAPI across the boundary");

    wtof("TSTRQXR: RUN COMPLETE -- zero=%d data=%d", nzero, ndata);
    printf("  zero-byte recv = %d, data recv = %d\n", nzero, ndata);

    {
        int summary = mbt_test_summary("TSTRQXR");
        if (skipped) {
            printf("*** THE ARM DID NOT RUN -- CC %d, NOT a pass.\n",
                   XR_CC_GATE_SKIPPED);
            return XR_CC_GATE_SKIPPED;
        }
        return summary;
    }
}
