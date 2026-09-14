/*
 * tstrqx2.c -- M5-2e Job A: TWO clients at once.  The (e) exit gate.
 *
 * MVS-only (host = false): two address spaces sharing one CSA slot pool
 * through a private SVC has no host analog, and neither does the ownership
 * check this gate exercises -- it takes the zero-identity red line in Phase 1
 * and is INERT there, so the host suite cannot reach it at all.
 *
 * WHICH TSTRQX* IS THIS?  Five files now differ by one character, so each is
 * named by what it proves and nothing else:
 *
 *   TSTRQXC  the 64-slot pool under CONTENTION (M5-2b4), and the (e)
 *            measurement instrument (stage a).  Its verb is deliberately
 *            side-effect-free: no socket, no app slot, ubuf = NULL.
 *   TSTRQXM  ONE client: that a real NSFRQE crosses the boundary at all, and
 *            the moved-length contract (M5-2a, obligation #2).
 *   TSTRQXF  the FAULT paths and the slot / anchor read-back (M5-2b1/b2).
 *   TSTRQXR  the receive-direction key window (#80).  Opt-in: it abends NSFS.
 *   TSTRQX2  THIS FILE.  TWO clients, concurrently, in two address spaces:
 *            that they are multiplexed, that neither can reach the other's
 *            sockets or app slot, and that the ONE shared landing area does
 *            not carry one client's bytes into the other's buffer.
 *
 * ------------------------------------------------------------------------
 * THE THREE PROPERTIES (the (e) memo), and the paired control for each.
 *
 * The red line this file is built to: FOR EVERY ASSERTION OF THE SHAPE "X DID
 * NOT HAPPEN", THE SAME RUN SHOWS THAT X CAN HAPPEN.  An arm that is green
 * because its condition was never created prints the same line as one that is
 * green because the condition was refused.
 *
 *   1.1 MULTIPLEXING.  Both clients progress at CHECKPOINTS across the
 *       window, never a total at the end: a client that starved for most of
 *       the window and caught up at teardown prints the same total as one
 *       that ran evenly.  Its own vacuity guard is that the sampling FIRED
 *       (> 1 checkpoint) -- a table of zeros because nothing sampled reads
 *       exactly like a starved client.
 *
 *   1.2 ISOLATION OF IDENTITY.  ONE verb, LISTEN, and not a sweep.  A sweep
 *       would prove that the same function is called nine times; what a
 *       RUNTIME test can add is that the check holds live, with two real
 *       clients.  That there is no BYPASS is a source property and is stated
 *       as one below, not tested.
 *
 *       THE CONTROL IS THE SAME DESCRIPTOR VALUE, DRIVEN BY ITS OWNER.  B is
 *       refused EBADF on A's descriptor; A is then served RETOK on THAT
 *       EXACT VALUE.  One value, two address spaces, opposite results, one
 *       axis varied -- and A's RETOK is also what proves the value B aimed at
 *       resolves to a LIVE socket, so B's refusal cannot be the vacuous
 *       "nothing was there" (ADR-0046 makes foreign and never-existing
 *       indistinguishable BY CONSTRUCTION, which is exactly why a refusal
 *       alone is not evidence).
 *
 *       AND THE TERMAPI HALF HAS THE SAME SHAPE, before and after: A's LISTEN
 *       on that descriptor is RETOK before its TERMAPI and EBADF after, while
 *       B's sockets and app slot go on working.  Without the "after" row,
 *       "B survived A's TERMAPI" is green even if TERMAPI tore nothing down.
 *
 *   1.3 NO CROSS-CLIENT CONTAMINATION OF g_land.  One landing area, shared by
 *       every client, which exists because ADR-0042 10 permits exactly ONE
 *       request in flight.  The residue argument is sound and host-pinned in
 *       TSTREQX; it has never been exercised with two real clients
 *       alternating through it.  PREDICTED TO PASS -- see predictions.md --
 *       because g_busy is held from dispatch to completion, so a second
 *       request cannot enter g_land between one request's copy-in and its
 *       copy-out.  Proving it POSITIVELY is the point: it pins a property
 *       that today rests on an invariant one function away.
 *
 *       THE BULK VERB NEEDS NO DEVICE, DELIBERATELY: a non-blocking
 *       RECVFROM on an empty rxq is answered EWOULDBLOCK by udp_recv without
 *       touching the routing table or the wire, while the TRANSPORT still
 *       stages ulen bytes in and copies the same ulen back out -- which is
 *       the whole of what 1.3 is about.  A SENDTO cannot: udp_send takes its
 *       MTU from nsfip_route, so on a stand whose interface never came up
 *       every send is EHOSTUNREACH and the gate would go red for a reason
 *       that has nothing to do with the landing area.  SENDTO remains as the
 *       once-per-checkpoint WIRE ARM -- the only place a protocol op READS
 *       g_land -- and when there is no interface that arm is a REPORTED
 *       skip, never a silent one.
 *
 *       THREE THINGS MAKE A GREEN 1.3 WORTH SOMETHING.  The two clients use
 *       DIFFERENT GENERATORS (not one seed plus a constant), so a foreign
 *       byte is recognisable AS the other client's rather than merely wrong,
 *       and the mismatch report says which.  They use DIFFERENT LENGTHS, so
 *       B's bytes [XA_SEND_B, XA_BUF) are never written by the transport at
 *       all and stand as a TAIL SENTINEL against an over-long copy-out --
 *       which two equal-length clients cannot see.  And the fill is a
 *       pattern, not zeros, so a byte that failed to cross shows as a
 *       mismatch instead of an accidental match.
 *
 * ------------------------------------------------------------------------
 * WHY THIS IS NOT AN EXTENSION OF TSTRQXM.
 *
 * TSTRQXM is M5-2a's merged live gate and carries the unauthorised-client
 * claim, the moved-length contract and the cross-boundary EMSGSIZE case.
 * Folding (e)'s exit gate into it would hang TWO MILESTONES ON ONE RETURN
 * CODE, so a future red Job A would show M5-2a's proof red with it.
 *
 * ------------------------------------------------------------------------
 * IT TALKS TO THE SVC DIRECTLY, AND THAT IS A DELIBERATE LINK-LEVEL CHOICE.
 *
 * 1.2 forces raw descriptors anyway: the EZASOKET facade numbers sockets by a
 * halfword index into the CLIENT'S OWN table, so a client cannot NAME another
 * client's socket through the API and the arm is unreachable from there.
 * Given that, going through nsfreq_call would link the whole 30-source stack
 * into the client for nothing -- and a gate whose subject is entirely
 * STC-side is better off with no copy of the stack in the client at all, so
 * "which copy ran" can never be a question.  Sources are this file plus
 * asm/nsftime.asm, the TSTRQXC shape.
 *
 * The recipe is nsfreqc_call's, inlined: NSFV_REQ.func = NSFV_REQ_RQE,
 * rqeimg = &the 64-byte NSFRQE image, ubuf/ulen ALSO in the NSFV_REQ (the
 * ADR-0039 bounce moves the user buffer as its own move).  On return the
 * image carries retcode / errno_ / apptok / p1 / p2 / p3 -- the six fields
 * nsfreqx_result_out writes back -- so a raw client reads exactly what the
 * facade would have handed it.
 *
 * ------------------------------------------------------------------------
 * THE SCAFFOLDING IS DUPLICATED FROM tstrqxc.c ON PURPOSE.
 *
 * The barrier, the SVC stub and the checkpoint grid are copies.  Refactoring
 * tstrqxc.c into a shared header would put JOB B'S MERGED INSTRUMENT back in
 * scope (MS / MA / MB / MSP), which this round's red lines forbid: Job B runs
 * after this one, after an IPL, and nothing here may touch it.  ~80 lines of
 * test scaffolding is the cheaper of the two.
 *
 * WHAT IS NOT COPIED: the latency histogram.  Job A is PASS/FAIL and reports
 * NO TIMING FIGURES.  The checkpoint counts below are PROGRESS evidence for
 * 1.1 and are NOT a throughput measurement -- they are not comparable with
 * anything Job B produces, and nothing here may be quoted as a baseline.  The
 * clock is used for two things only: to bound the window, and to print the
 * opening and closing TOD so the OVERLAP of the two jobs is a measured
 * quantity rather than an assumption.
 *
 * ------------------------------------------------------------------------
 * RETURN CODES -- three, the TSTRQXF / TSTXFW idiom.
 *
 *    0                     everything ran and everything passed
 *    1                     the gate RAN and something FAILED
 *    XA_CC_GATE_SKIPPED    the gate COULD NOT RUN.  A skip reporting CC 0
 *                          would make a run containing no evidence look
 *                          exactly like a run that proved something.
 *
 * PREREQUISITES.  S NSFS, FRESHLY STARTED, with nothing else holding sockets:
 * the descriptor assertions below are exact values, not derivations, and they
 * are exact only on a fresh socket table (every slot generation seeds to 1,
 * sock_alloc hands out the lowest free index).  A stale STC is a SKIP, not a
 * failure.
 *
 * AN INTERFACE IS NOT REQUIRED.  Every assertion carrying one of the three
 * properties is answered inside the STC with no device involved.  The one arm
 * that does need NSF210I/NSF211I is the once-per-checkpoint real datagram,
 * and its absence is REPORTED rather than asserted -- so the gate runs on a
 * stand whose CTCI pair failed to come up, and says which arms ran.
 *
 * THE CLIENT IS UNAUTHORISED and never self-auths.  Asserted, not assumed.
 */
#include "nsfvsvc.h"
#include "nsfreq.h"         /* NSFRQE -- the image the request carries        */
#include "nsfsoc.h"         /* NSF_AF_INET / NSF_SOCK_STREAM / _DGRAM         */
#include "nsftime.h"        /* nsf_now -- STCK, the window clock only         */
#include <clibecb.h>        /* ecb_timed_wait -- the poll pause               */
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                     */
#include <clibwto.h>        /* wtof -- survives a hang, unlike SYSPRINT       */
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define XA_CC_GATE_SKIPPED 20

/* ---- the CSA flag slots -------------------------------------------------
 * Taken from the TOP of the pool so the claim scan, which always starts at 0,
 * never walks past them.  HELD is the flag state (the TSTRQXC barrier's), and
 * a HELD slot is off the work list rather than corrupting anything: the
 * SLOT probe verb branches out ABOVE the permit-one gate, so it is serviced
 * at the production STC exactly as at the probe STC.
 *
 * RECOVERY: a run that dies mid-window leaves its flags HELD.  TSTRQXC
 * PARM='RESET' releases every CLAIMED/HELD slot, or recycle the STC. */
#define XA_FLAG_A       63u     /* A present (barrier), held for the run      */
#define XA_FLAG_B       62u     /* B present (barrier), held for the run      */
#define XA_FLAG_TGT     61u     /* A raises: target socket created            */
#define XA_FLAG_ISO     60u     /* B raises: isolation attempt done           */
#define XA_FLAG_TRM     59u     /* A raises: TERMAPI done.  B lowers it.      */

/* ---- the descriptors are DETERMINED, not derived -------------------------
 * A creates exactly XA_A_SOCKS socket(s) before raising XA_FLAG_TGT, and B
 * creates its first immediately after.  On a fresh STC soc_init seeds every
 * slot generation to 1 and sock_alloc takes the lowest free index, so both
 * values are fixed at compile time and each client ASSERTS its own against
 * what RQ_SOCKET actually returned.
 *
 * WHY NOT DERIVE B's-index-minus-one, the TSTD1B way: a derivation gets the
 * INDEX right and says nothing about the GENERATION, and a stale generation
 * refuses through the same NULL as a foreign descriptor -- the vacuity this
 * round exists to avoid.  Asserting both values makes the generation part of
 * the claim.  If either assertion fails the table is not fresh, the aim is
 * unsound, and the gate SKIPS rather than reporting a refusal it cannot
 * attribute. */
#define XA_A_SOCKS      1u
#define XA_DESC_A       ((1u << 16) | 0u)
#define XA_DESC_B       ((1u << 16) | XA_A_SOCKS)

/* ---- the window --------------------------------------------------------- */
#define XA_WIN_S        90u     /* pass/fail: long enough for 6 checkpoints   */
#define XA_CHK_S        15u
#define XA_CHK_MAX      32u     /* 90/15 = 6 needed; the cap is a backstop    */
#define XA_LOOP_CAP  4000000u   /* runaway backstop; the clock ends the run   */

/* ---- 1.3's buffers ------------------------------------------------------
 * The bulk verb is a non-blocking RECVFROM on an empty rxq, so none of these
 * lengths ever reaches a device and the whole window sends NOTHING on the
 * wire.  That matters twice over: it lets the gate run on a stand whose CTCI
 * pair never came up, and CLAUDE.md records the pair degrading under
 * back-to-back heavy runs -- a gate that damages the stand it runs on is a
 * bad gate.
 *
 * B's 1536 leaves [1536, 2048) untouched by the transport for the whole run:
 * the tail sentinel against an over-long copy-out, which two equal-length
 * clients could not see.  A's 2048 is the whole landing area, the maximum
 * contamination surface. */
#define XA_BUF          2048u
#define XA_SEND_A       2048u   /* == NSFV_XFER_CHUNK: the whole landing area */
#define XA_SEND_B       1536u   /* leaves a tail sentinel in B's buffer       */
#define XA_WIRE_N       1024u   /* the real datagram, one per checkpoint      */
/* XA_WIRE_N is under MTU-28 (1472) so it really goes; the two bulk lengths
 * are irrelevant to the MTU because the bulk verb never reaches a device. */

#define XA_PEER_IP      0xC0A8C802u     /* 192.168.200.2 -- the host side     */
#define XA_PEER_PORT    9999u           /* nothing listens: we want the SEND  */
#define XA_PORT_A_TCP   7801u
#define XA_PORT_B_TCP   7802u
#define XA_PORT_A_UDP   7803u
#define XA_PORT_B_UDP   7804u

#define XA_POLL_HS      50u     /* 0.50 s, in hundredths                      */
#define XA_POLL_MAX     120u    /* 120 * 0.5 s = 60 s (the TSTRQXC barrier)   */

/* --------------------------------------------------------------------------
 * State that must outlive the roles.
 * -------------------------------------------------------------------------- */
static NSFRQE   g_image;                /* the 64-byte image we send          */
static UINT     g_apptok;               /* our app token, from RQ_INITAPI     */
static UCHAR    g_buf[XA_BUF];          /* 1.3: the buffer that round-trips   */
static UCHAR    g_exp[XA_BUF];          /* 1.3: what it must still hold       */
static int      g_isb;                  /* 1 when this job is role B          */

typedef struct xa_chk {
    UINT t_ms;
    UINT sent;                          /* requests completed so far          */
} XA_CHK;

static XA_CHK   g_chk[XA_CHK_MAX];
static UINT     g_nchk;

/* --------------------------------------------------------------------------
 * The private SVC, R1 = A(req), via the EX-SVC-0 trick (ADR-0038 6).  The asm
 * labels are per-file so they stay unique in the load module.
 * -------------------------------------------------------------------------- */
static void __attribute__((noinline))
xa_svc(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"
        "         LR    6,%1\n"
        "         EX    6,NSFX20\n"
        "         B     NSFX2X\n"
        "NSFX20   SVC   0\n"
        "NSFX2X   DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

static void
xa_req_init(NSFV_REQ *req, UINT func)
{
    memset(req, 0, sizeof *req);
    memcpy(req->eye, NSFV_REQ_EYE, 4);
    req->func = func;
    req->rc   = -1;
}

/* Wait out `hsec` hundredths without a busy loop: a local ECB nobody posts,
 * satisfied by the timer.  Same shape as nsfsx_pause in the STC. */
static void
xa_pause(unsigned hsec)
{
    ECB local = 0;

    ecb_timed_wait(&local, hsec, 0);
}

/* --------------------------------------------------------------------------
 * The two probe verbs the flags need.  BOTH branch out of the SVC routine's
 * dispatch chain ABOVE the M5-2d1c permit-one gate and claim no slot, so they
 * are serviced at the PRODUCTION STC as well as at the probe STC.  That is
 * what lets this file drop TSTRQXC's six-hop anchor chase entirely: QUERY
 * reports the named slot's state, so no client ever reads CSA directly.
 * -------------------------------------------------------------------------- */
static int
xa_query(UINT idx, UINT *state, UINT *infl)
{
    NSFV_REQ req;

    xa_req_init(&req, NSFV_REQ_QUERY);
    req.slot = idx;
    xa_svc(&req);
    if (state) *state = req.qstate;
    if (infl)  *infl  = req.qinfl;
    return req.rc;
}

/* CS one named slot from `expect` to `set`.  A CS and not a blind store, so a
 * flag can be ASSERTED to have taken rather than inferred from the absence of
 * a complaint (CLAUDE.md 8.5). */
static int
xa_slot_cas(UINT idx, UINT expect, UINT set)
{
    NSFV_REQ req;

    xa_req_init(&req, NSFV_REQ_SLOT);
    req.slot    = idx;
    req.sexpect = expect;
    req.snew    = set;
    xa_svc(&req);
    return req.rc;
}

static int xa_raise(UINT idx)
{
    return xa_slot_cas(idx, NSFV_REQ_FREE, NSFV_REQ_HELD);
}

static void xa_lower(UINT idx)
{
    (void)xa_slot_cas(idx, NSFV_REQ_HELD, NSFV_REQ_FREE);
}

/* Poll a flag slot up.  Returns 0 when it is raised, -1 on timeout -- and a
 * timeout is a SKIP, never a failure: it means the other client never got
 * there, which is a statement about the run and not about the stack. */
static int
xa_wait_flag(UINT idx, const char *what)
{
    unsigned n;
    UINT     st = 0u;

    for (n = 0u; n < XA_POLL_MAX; n++) {
        if (xa_query(idx, &st, NULL) == NSFV_RC_OK &&
            st == NSFV_REQ_HELD) {
            printf("  %s seen after %u polls\n", what, n);
            return 0;
        }
        xa_pause(XA_POLL_HS);
    }
    printf("  TIMED OUT waiting for %s after %u polls\n", what,
           (unsigned)XA_POLL_MAX);
    wtof("TSTRQX2: TIMEOUT waiting for %s", what);
    return -1;
}

/* The barrier: raise our own presence flag, wait for the other's. */
static int
xa_barrier(UINT mine, UINT theirs)
{
    if (xa_raise(mine) != NSFV_RC_OK) {
        printf("  BARRIER: could not raise flag slot %u -- is a previous run"
               " still holding it?  (TSTRQXC PARM='RESET' clears it)\n",
               (unsigned)mine);
        return -1;
    }
    printf("  flag slot %u raised; waiting for the other client on slot %u\n",
           (unsigned)mine, (unsigned)theirs);
    return xa_wait_flag(theirs, "the other client");
}

/* --------------------------------------------------------------------------
 * One NSFRQE across the boundary.  nsfreqc_call's recipe, inlined: see the
 * header for why this file does not link the stack to get it.
 *
 * Returns the TRANSPORT rc (NSFV_RC_*).  The REQUEST's result is in g_image
 * (retcode / errno_ / apptok / p1 / p2 / p3), which is the exact field set
 * nsfreqx_result_out writes back.
 * -------------------------------------------------------------------------- */
static int
xa_call_f(UINT fn, UINT desc, UINT p1, UINT p2, UINT p3,
          void *ubuf, UINT ulen, UINT flags)
{
    NSFV_REQ req;

    memset(&g_image, 0, sizeof g_image);
    memcpy(g_image.eye, NSFRQE_EYE, 4);
    g_image.fn       = (USHORT)fn;
    g_image.flags    = (USHORT)flags;
    g_image.sockdesc = desc;
    g_image.apptok   = g_apptok;
    g_image.p1       = p1;
    g_image.p2       = p2;
    g_image.p3       = p3;
    g_image.ubuf     = ubuf;
    g_image.ulen     = ulen;
    g_image.retcode  = NSF_RETERR;
    g_image.errno_   = -1;              /* so an unwritten field is obvious   */

    xa_req_init(&req, NSFV_REQ_RQE);
    req.rqeimg = (void *)&g_image;
    req.ubuf   = ubuf;                  /* the ADR-0039 bounce, caller-AS     */
    req.ulen   = ulen;
    xa_svc(&req);
    return req.rc;
}

/* The ordinary form: no RQ_F_* flags.  Kept as its own function rather than
 * adding an eighth argument to twenty call sites -- the one caller that needs
 * a flag says so at the call. */
static int
xa_call(UINT fn, UINT desc, UINT p1, UINT p2, UINT p3, void *ubuf, UINT ulen)
{
    return xa_call_f(fn, desc, p1, p2, p3, ubuf, ulen, 0u);
}

/* --------------------------------------------------------------------------
 * 1.3 -- the two patterns.
 *
 * TWO DIFFERENT GENERATORS, not one seed plus a constant offset.  With a
 * constant offset every foreign byte differs from the expected one by the
 * same amount, which is indistinguishable from a systematic arithmetic bug;
 * with independent generators a byte that carries the other client's value at
 * that index can only have come from the other client, and the mismatch
 * report says which of the two it is.
 * -------------------------------------------------------------------------- */
static UCHAR pat_a(UINT i)
{
    return (UCHAR)(0xA1u + i * 7u + (i >> 5));
}

static UCHAR pat_b(UINT i)
{
    return (UCHAR)(0xB2u ^ (i * 13u + (i >> 3)));
}

static void
xa_fill(void)
{
    UINT i;

    for (i = 0u; i < XA_BUF; i++) {
        g_exp[i] = g_isb ? pat_b(i) : pat_a(i);
        g_buf[i] = g_exp[i];
    }
}

/* The FIRST mismatch, in full.  Says whether the byte is the OTHER CLIENT'S
 * pattern at that index -- which is the whole reason the two generators are
 * different -- or merely wrong, which would be a bug in the length arithmetic
 * rather than contamination. */
static void
xa_report_dirty(UINT after_len)
{
    UINT i;

    for (i = 0u; i < XA_BUF; i++) {
        if (g_buf[i] == g_exp[i]) {
            continue;
        }
        {
            UCHAR other = g_isb ? pat_a(i) : pat_b(i);
            const char *kind = (g_buf[i] == other)
                             ? "THE OTHER CLIENT'S PATTERN"
                             : "neither pattern (length arithmetic, not"
                               " contamination)";

            printf("  BUFFER DIRTY at +%u after a %u-byte call:"
                   " got %02X want %02X -- %s\n",
                   (unsigned)i, (unsigned)after_len,
                   (unsigned)g_buf[i], (unsigned)g_exp[i], kind);
            wtof("TSTRQX2: BUFFER DIRTY +%u got %02X want %02X",
                 (unsigned)i, (unsigned)g_buf[i], (unsigned)g_exp[i]);
        }
        return;
    }
}

/* Microseconds between two STCK readings: (b - a) >> 12 (TOD bit 51 == 1 us).
 * Lifted from test/mvs/tsttmacc.c, where it was first checked against a known
 * 100 ms interval.  USED ONLY to bound the window and to stamp the
 * checkpoints -- no per-request timing is taken and none is reported. */
static UINT
xa_tod_us(const NSFTIME *a, const NSFTIME *b)
{
    UINT dlo    = b->lo - a->lo;
    UINT borrow = (b->lo < a->lo) ? 1u : 0u;
    UINT dhi    = b->hi - a->hi - borrow;

    return (dhi << 20) | (dlo >> 12);
}

/* --------------------------------------------------------------------------
 * 1.1 + 1.3 -- the window.  Both clients run this concurrently.
 *
 * THE BULK VERB IS A NON-BLOCKING RECVFROM ON AN EMPTY RXQ, AND THE REASON IS
 * THAT IT NEEDS NO DEVICE.  udp_recv answers RQ_F_NONBLOCK on an empty rxq
 * with EWOULDBLOCK without touching the routing table, the device or the
 * wire, while the TRANSPORT still stages `ulen` bytes in and copies the same
 * `ulen` back out -- which is the whole of what 1.3 is about.  A SENDTO
 * cannot do that: udp_send takes its MTU from nsfip_route, so on a stand
 * whose interface never came up every send answers EHOSTUNREACH and the gate
 * goes red for a reason that has nothing to do with the landing area.
 *
 * It keeps the reason the locked decision gave for choosing SENDTO over a
 * TCP send -- a FIXED expected buffer range, because a short return would
 * make the expectation move from call to call and a contamination check with
 * a moving expectation is where a bookkeeping error hides.  EWOULDBLOCK is
 * fixed in exactly that way, and it is a slightly stronger "the request was
 * really serviced" control than EMSGSIZE: it is returned by a PROTOCOL op
 * reached THROUGH the ownership check, so every one of these thousands of
 * calls also re-exercises 1.2 for this client's own socket.
 *
 * The verification is per call; only the ASSERTION is aggregated, because a
 * CHECK per iteration would print thousands of lines.  The first mismatch is
 * reported in full the moment it happens.
 *
 * ONCE PER CHECKPOINT IT ALSO SENDS ONE REAL DATAGRAM.  That is the only case
 * in the whole file where a protocol op actually READS the landing area, and
 * ~6 of them per client is a load the CTCI pair will not notice.  IT IS THE
 * ONE ARM THAT NEEDS AN INTERFACE, so it is reported rather than asserted
 * when there is none: `wire=0 (NO INTERFACE)` in the summary and a WTO, so a
 * green run can never be mistaken for one that included it.
 * -------------------------------------------------------------------------- */
static void
xa_window(UINT udesc)
{
    NSFTIME t_win, t_now;
    UINT    sendlen = g_isb ? XA_SEND_B : XA_SEND_A;
    UINT    k, el_us = 0u, next_chk = 0u;
    UINT    n_bulk = 0u, n_txerr = 0u, n_wrong = 0u, n_dirty = 0u;
    UINT    n_wire = 0u, n_wire_ok = 0u;
    int     no_iface = 0, said = 0;
    int     rc;

    g_nchk = 0u;
    printf("\n--- window: %u s, checkpoints every %u s, bulk %u bytes"
           " (buffer %u) ---\n", (unsigned)XA_WIN_S, (unsigned)XA_CHK_S,
           (unsigned)sendlen, (unsigned)XA_BUF);
    printf("  bulk  = non-blocking RECVFROM on an empty rxq -> EWOULDBLOCK"
           " (no device, no wire)\n");
    printf("  wire  = one real %u-byte SENDTO per checkpoint (needs the"
           " interface)\n", (unsigned)XA_WIRE_N);

    nsf_now(&t_win);
    printf("  window opens at TOD %08X%08X\n",
           (unsigned)t_win.hi, (unsigned)t_win.lo);
    wtof("TSTRQX2: %s window opens TOD %08X%08X", g_isb ? "B" : "A",
         (unsigned)t_win.hi, (unsigned)t_win.lo);

    for (k = 0u; k < XA_LOOP_CAP; k++) {
        rc = xa_call_f(RQ_RECVFROM, udesc, 0u, 0u, 0u, g_buf, sendlen,
                       RQ_F_NONBLOCK);
        n_bulk++;
        if (rc != NSFV_RC_OK) {
            n_txerr++;
        } else if (g_image.retcode != NSF_RETERR ||
                   g_image.errno_  != NSF_EWOULDBLOCK) {
            n_wrong++;
            if (!said) {
                printf("  UNEXPECTED bulk result: rc=%d retcode=%d errno=%d"
                       " (wanted RETERR/EWOULDBLOCK=%d)\n", rc,
                       (int)g_image.retcode, (int)g_image.errno_,
                       (int)NSF_EWOULDBLOCK);
                said = 1;
            }
        }
        /* AFTER EVERY CALL, the WHOLE buffer -- not just the moved range, so
         * an over-long copy-out past `sendlen` is caught as well as a short
         * or misplaced one.  For B, [XA_SEND_B, XA_BUF) is the tail sentinel:
         * the transport never writes it at all. */
        if (memcmp(g_buf, g_exp, XA_BUF) != 0) {
            if (n_dirty == 0u) {
                xa_report_dirty(sendlen);
            }
            n_dirty++;
            memcpy(g_buf, g_exp, XA_BUF);   /* re-arm, so one hit is one hit */
        }

        nsf_now(&t_now);
        el_us = xa_tod_us(&t_win, &t_now);

        /* THE BOUNDARY IS TESTED SEPARATELY FROM THE ARRAY'S CAPACITY, and
         * that is not a style choice.  With the wire send inside a
         * `g_nchk < XA_CHK_MAX` guard, a later change to XA_CHK_S that
         * overran the array would silently stop the wire case as well --
         * n_wire would freeze while `n_wire_ok == n_wire` stayed green.  An
         * assertion that stops being exercised without saying so is the
         * absent-vs-succeeded shape (CLAUDE.md 8.5), so the crossing drives
         * the wire send and only the RECORDING is capped. */
        if (el_us >= next_chk) {
            if (g_nchk < XA_CHK_MAX) {
                g_chk[g_nchk].t_ms = el_us / 1000u;
                g_chk[g_nchk].sent = n_bulk;
                g_nchk++;
            }
            /* Advance to the next grid point PAST el_us, not by a fixed
             * increment: a request that stalls across two boundaries would
             * otherwise leave next_chk behind the clock and fire again
             * immediately, reporting an interval that is not XA_CHK_S.  A
             * SKIPPED grid point shows as a t_ms column that jumps by two
             * intervals, which is visible; a silently short one would not be.
             * (stage a 2.3; under #64 a multi-second stall is not
             * hypothetical.) */
            do {
                next_chk += XA_CHK_S * 1000000u;
            } while (next_chk <= el_us);

            /* THE WIRE ARM.  One real datagram per checkpoint, and the only
             * place a protocol op READS the landing area.
             *
             * NO INTERFACE IS A REPORTED SKIP, NOT A FAILURE.  nsfip_route
             * returns NULL when no device registered, so udp_send answers
             * EHOSTUNREACH before it looks at anything this gate is about.
             * That is a statement about the STAND, and letting it turn the
             * whole gate red would be exactly the "goes red for a reason
             * unrelated to the property it tests" failure.  It is LOUD
             * instead: attempts stop, the summary prints NO INTERFACE, a WTO
             * records it, and the assertion below is replaced by one that
             * says the arm did not run -- so a green run can never be
             * mistaken for one that included it. */
            if (!no_iface) {
                rc = xa_call(RQ_SENDTO, udesc, XA_PEER_IP, XA_PEER_PORT, 0u,
                             g_buf, XA_WIRE_N);
                n_wire++;
                if (rc == NSFV_RC_OK && g_image.retcode == (INT)XA_WIRE_N) {
                    n_wire_ok++;
                } else if (rc == NSFV_RC_OK &&
                           g_image.errno_ == NSF_EHOSTUNREACH) {
                    no_iface = 1;
                    n_wire--;               /* it never ran -- do not count it */
                    printf("  NO INTERFACE (EHOSTUNREACH): the wire arm CANNOT"
                           " RUN on this stand.  Is the CTCI pair up?"
                           " (NSF210I / NSF211I)\n");
                    wtof("TSTRQX2: NO INTERFACE -- WIRE ARM DID NOT RUN");
                } else {
                    printf("  the in-range SENDTO failed rc=%d retcode=%d"
                           " errno=%d\n", rc, (int)g_image.retcode,
                           (int)g_image.errno_);
                }
            }
            if (memcmp(g_buf, g_exp, XA_BUF) != 0) {
                if (n_dirty == 0u) {
                    xa_report_dirty(XA_WIRE_N);
                }
                n_dirty++;
                memcpy(g_buf, g_exp, XA_BUF);
            }
        }

        if (el_us >= XA_WIN_S * 1000000u) {
            break;
        }
    }

    nsf_now(&t_now);
    el_us = xa_tod_us(&t_win, &t_now);
    printf("  window closes at TOD %08X%08X (%u ms elapsed)\n",
           (unsigned)t_now.hi, (unsigned)t_now.lo, (unsigned)(el_us / 1000u));
    wtof("TSTRQX2: %s window closes TOD %08X%08X bulk=%u dirty=%u",
         g_isb ? "B" : "A", (unsigned)t_now.hi, (unsigned)t_now.lo,
         (unsigned)n_bulk, (unsigned)n_dirty);

    /* ---- 1.1 PROGRESS.  Per interval, never a cumulative total.
     * THESE ARE PROGRESS COUNTS, NOT A THROUGHPUT MEASUREMENT: Job A reports
     * no timing figures and nothing here is comparable with Job B. -------- */
    printf("\n  PROGRESS (requests completed per interval -- NOT a throughput"
           " figure):\n");
    printf("      t_ms    completed\n");
    {
        UINT i, lo = 0xFFFFFFFFu;

        for (i = 0u; i < g_nchk; i++) {
            UINT d = (i == 0u) ? g_chk[0].sent
                               : g_chk[i].sent - g_chk[i - 1u].sent;

            printf("    %7u  %11u\n", (unsigned)g_chk[i].t_ms, (unsigned)d);
            if (i > 0u && d < lo) {
                lo = d;
            }
        }
        printf("\n  bulk=%u txerr=%u wrong=%u dirty=%u | wire=%u ok=%u%s\n",
               (unsigned)n_bulk, (unsigned)n_txerr, (unsigned)n_wrong,
               (unsigned)n_dirty, (unsigned)n_wire, (unsigned)n_wire_ok,
               no_iface ? "  *** NO INTERFACE -- WIRE ARM DID NOT RUN ***"
                        : "");

        /* THE SAMPLING'S OWN POSITIVE CONTROL.  A progress table of zeros
         * because nothing ever sampled reads exactly like a starved client
         * (CLAUDE.md 8.5). */
        CHECK(g_nchk > 1u,
              "1.1: progress was SAMPLED at more than one point -- an"
              " unsampled window prints a table that looks like starvation");
        CHECK(lo != 0xFFFFFFFFu && lo > 0u,
              "1.1: EVERY interval after the first made progress -- no"
              " interval in which this client was starved");
    }

    CHECK(n_bulk > 0u, "1.3: the window issued requests at all");
    CHECK_EQ((long)n_txerr, 0L,
             "1.3: every request reached the STC (no transport failure)");
    CHECK_EQ((long)n_wrong, 0L,
             "1.3: and every one was answered EWOULDBLOCK by a PROTOCOL op"
             " reached THROUGH the ownership check -- which is what proves"
             " the request was really serviced and really this client's");
    CHECK_EQ((long)n_dirty, 0L,
             "1.3: this client's buffer held ITS OWN FULL pattern after EVERY"
             " call -- no cross-client contamination of the landing area");

    /* The wire arm, or an explicit statement that it did not run.  Never a
     * silent absence: whichever branch is taken, the run SAYS which. */
    if (no_iface) {
        printf("  *** THE WIRE ARM DID NOT RUN (no interface).  This run says"
               " NOTHING about a protocol op READING the landing area.\n");
        CHECK(n_wire == 0u,
              "1.3: the wire arm DID NOT RUN -- no interface on this stand,"
              " so no datagram reached the wire and no op read g_land");
    } else {
        CHECK(n_wire > 0u, "1.3: the in-range (real datagram) case ran");
        CHECK_EQ((long)n_wire_ok, (long)n_wire,
                 "1.3: and every in-range SENDTO reported its full length --"
                 " the one case where an op actually READS the landing area");
    }
}

/* --------------------------------------------------------------------------
 * A -- the OWNER.  It creates the socket B will be refused, and then, on the
 * SAME descriptor value, is served.  That RETOK is 1.2's control: it is what
 * proves the value B aimed at resolves to a live socket, so B's EBADF is a
 * refusal and not the vacuous "nothing was there".
 *
 * A DELIBERATELY DOES NOT LISTEN BEFORE B TRIES.  The target is left in
 * TCP_CLOSED, so if the ownership check were absent B's LISTEN would SUCCEED
 * -- a demonstrated hijack, which is the loudest failure available.  Had A
 * listened first the same missing check would show only as EINVAL, and a
 * different errno is a far quieter signal than a socket changing state under
 * its owner.
 * -------------------------------------------------------------------------- */
static int
xa_run_a(void)
{
    UINT desc_a = 0u, desc_au = 0u;
    int  rc, skipped = 0;

    printf("\n--- A: the owner ---\n");
    if (xa_barrier(XA_FLAG_A, XA_FLAG_B) != 0) {
        wtof("TSTRQX2: A BARRIER FAILED -- gate skipped");
        xa_lower(XA_FLAG_A);
        return -1;
    }

    rc = xa_call(RQ_INITAPI, 0u, 0u, 0u, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK,
          "A: INITAPI across the boundary");
    g_apptok = g_image.apptok;
    CHECK(g_apptok != 0u, "A: and it came back with an app token");
    if (rc != NSFV_RC_OK || g_apptok == 0u) {
        skipped = 1;
        goto done;
    }

    rc = xa_call(RQ_SOCKET, 0u, (UINT)NSF_AF_INET, (UINT)NSF_SOCK_STREAM,
                 6u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode >= 0,
          "A: SOCKET (stream) across the boundary");
    if (rc != NSFV_RC_OK || g_image.retcode < 0) {
        skipped = 1;
        goto done;
    }
    desc_a = (UINT)g_image.retcode;
    printf("  A's target descriptor = %08X (the gate needs %08X)\n",
           (unsigned)desc_a, (unsigned)XA_DESC_A);
    wtof("TSTRQX2: A TARGET DESC %08X", (unsigned)desc_a);

    /* NOT AN ASSERTION ABOUT THE STACK -- a precondition on the STAND.  The
     * exact value is what makes B's aim a determination rather than a
     * derivation, and it holds only on a freshly started STC whose socket
     * table nobody else has touched.  A stale table is a SKIP: the gate could
     * not be aimed, so its refusal would prove nothing. */
    if (desc_a != XA_DESC_A) {
        printf("  THE SOCKET TABLE IS NOT FRESH.  B aims at %08X by"
               " construction, so this run cannot be aimed and its refusal"
               " would be unattributable.  P NSFS / S NSFS and re-run.\n",
               (unsigned)XA_DESC_A);
        wtof("TSTRQX2: A DESC %08X NOT %08X -- STALE TABLE, GATE SKIPPED",
             (unsigned)desc_a, (unsigned)XA_DESC_A);
        skipped = 1;
        goto done;
    }

    rc = xa_call(RQ_BIND, desc_a, 0u, XA_PORT_A_TCP, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK,
          "A: BIND the target (it stays TCP_CLOSED -- A does NOT listen yet)");

    if (xa_raise(XA_FLAG_TGT) != NSFV_RC_OK) {
        printf("  could not raise the target flag\n");
        skipped = 1;
        goto done;
    }
    printf("  target published; waiting for B's isolation attempt\n");
    if (xa_wait_flag(XA_FLAG_ISO, "B's isolation attempt") != 0) {
        skipped = 1;
        goto done;
    }

    /* ---- 1.2 THE CONTROL.  Same descriptor value, its owner asking. ------ */
    rc = xa_call(RQ_LISTEN, desc_a, 5u, 0u, 0u, NULL, 0u);
    printf("  A LISTEN on its OWN %08X: rc=%d retcode=%d errno=%d\n",
           (unsigned)desc_a, rc, (int)g_image.retcode, (int)g_image.errno_);
    wtof("TSTRQX2: A LISTEN OWN %08X RETCODE=%d ERRNO=%d",
         (unsigned)desc_a, (int)g_image.retcode, (int)g_image.errno_);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK,
          "1.2 CONTROL: the SAME descriptor B was refused is SERVED for its"
          " owner -- so B's refusal was a refusal, not an empty slot");

    /* ---- A's UDP socket for the window.  Created only now, so nothing was
     * allocated between A's target and B's first socket. ------------------- */
    rc = xa_call(RQ_SOCKET, 0u, (UINT)NSF_AF_INET, (UINT)NSF_SOCK_DGRAM,
                 17u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode >= 0, "A: SOCKET (datagram)");
    if (rc != NSFV_RC_OK || g_image.retcode < 0) {
        skipped = 1;
        goto done;
    }
    desc_au = (UINT)g_image.retcode;
    rc = xa_call(RQ_BIND, desc_au, 0u, XA_PORT_A_UDP, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK, "A: BIND it");

    xa_fill();
    xa_window(desc_au);

    /* ---- 1.2 second half: A tears ITSELF down. -------------------------- */
    rc = xa_call(RQ_TERMAPI, 0u, 0u, 0u, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK,
          "A: TERMAPI across the boundary");

    /* THE "BEFORE AND AFTER" CONTROL, and it is what stops B's survival arm
     * being vacuous.  Without this row, "B survived A's TERMAPI" is green
     * even if TERMAPI tore nothing down.  One descriptor, one address space,
     * before and after: RETOK above, EBADF here. */
    g_apptok = 0u;
    rc = xa_call(RQ_LISTEN, desc_a, 5u, 0u, 0u, NULL, 0u);
    printf("  A LISTEN on %08X AFTER its TERMAPI: rc=%d retcode=%d errno=%d\n",
           (unsigned)desc_a, rc, (int)g_image.retcode, (int)g_image.errno_);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETERR,
          "1.2: A's OWN descriptor is refused after A's TERMAPI");
    CHECK_EQ((long)g_image.errno_, (long)NSF_EBADF,
             "1.2: refused EBADF -- the socket is GONE, so TERMAPI really did"
             " tear A's sockets down (the control for B's survival arm)");

    /* B lowers this one, after it has seen it. */
    (void)xa_raise(XA_FLAG_TRM);
    wtof("TSTRQX2: A TERMAPI DONE");

done:
    xa_lower(XA_FLAG_TGT);
    xa_lower(XA_FLAG_A);
    return skipped ? -1 : 0;
}

/* --------------------------------------------------------------------------
 * B -- the other address space.  It aims one verb at A's socket and is
 * refused; the same verb on its own is served.
 *
 * ONE VERB, NOT A SWEEP, and the reason is that a sweep would only show the
 * same function being called nine times.  THAT THERE IS NO BYPASS IS A SOURCE
 * PROPERTY AND NO RUNTIME TEST CAN ESTABLISH IT -- verified on main and
 * stated here rather than tested:
 *
 *   - req_socket() has NINE callers, all the descriptor-resolving verbs, and
 *     it is `return nsfreq_sock_owned(r->sockdesc, g_cur_ascb, g_cur_asid);`
 *     and nothing else (src/nsfreq.c).
 *   - nsfreq_sock_owned() has TWO callers: req_socket, and sel_scan in
 *     src/nsfsel.c -- the wider door, one descriptor per SELECT mask item.
 *   - sock_lookup() has TWO call sites in production code, not one.  One is
 *     inside nsfreq_sock_owned (SOCK_LOOKUP: CHECKED).  The other is in
 *     soc_complete (src/nsfsoc.c, SOCK_LOOKUP: INTERNAL) and is NOT a
 *     client-directed resolution: the stack is completing a request it
 *     already holds and resolves that request's own socket to clear its pend_
 *     slot.  It cannot serve as a bypass -- all it can do is null out a
 *     pend_ pointer that is equal to THIS request, which only the socket
 *     that parked it can be.  tools/check-sock-lookup.sh (CI job
 *     sock-lookup-callers) fails the build on an unclassified third one.
 *
 * So what this arm adds is the RUNTIME half: the check holds live, across a
 * real address-space boundary, with two real clients.
 * -------------------------------------------------------------------------- */
static int
xa_run_b(void)
{
    UINT desc_b = 0u, desc_bu = 0u, desc_tmp = 0u;
    int  rc, skipped = 0;

    printf("\n--- B: the other address space ---\n");
    if (xa_barrier(XA_FLAG_B, XA_FLAG_A) != 0) {
        wtof("TSTRQX2: B BARRIER FAILED -- gate skipped");
        xa_lower(XA_FLAG_B);
        return -1;
    }

    rc = xa_call(RQ_INITAPI, 0u, 0u, 0u, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK,
          "B: INITAPI across the boundary");
    g_apptok = g_image.apptok;
    CHECK(g_apptok != 0u, "B: and it came back with an app token");
    if (rc != NSFV_RC_OK || g_apptok == 0u) {
        skipped = 1;
        goto done;
    }

    if (xa_wait_flag(XA_FLAG_TGT, "A's target socket") != 0) {
        skipped = 1;
        goto done;
    }

    rc = xa_call(RQ_SOCKET, 0u, (UINT)NSF_AF_INET, (UINT)NSF_SOCK_STREAM,
                 6u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode >= 0,
          "B: SOCKET (stream) across the boundary");
    if (rc != NSFV_RC_OK || g_image.retcode < 0) {
        skipped = 1;
        goto done;
    }
    desc_b = (UINT)g_image.retcode;
    printf("  B's own descriptor = %08X (the gate needs %08X);"
           " A's is therefore %08X\n",
           (unsigned)desc_b, (unsigned)XA_DESC_B, (unsigned)XA_DESC_A);
    wtof("TSTRQX2: B OWN DESC %08X AIMING AT %08X",
         (unsigned)desc_b, (unsigned)XA_DESC_A);

    /* Same precondition as A's, from the other end.  If B's own socket is not
     * the index immediately after A's, the table is not fresh and B cannot be
     * sure what XA_DESC_A names -- a refusal it cannot attribute is not
     * evidence, so SKIP.  (A stale GENERATION refuses through the very same
     * NULL as a foreign descriptor, which is why this is an equality on the
     * whole descriptor and not just on the index.) */
    if (desc_b != XA_DESC_B) {
        printf("  THE SOCKET TABLE IS NOT FRESH -- B cannot be sure %08X is"
               " A's socket, so a refusal here would be unattributable."
               "  P NSFS / S NSFS and re-run.\n", (unsigned)XA_DESC_A);
        wtof("TSTRQX2: B DESC %08X NOT %08X -- STALE TABLE, GATE SKIPPED",
             (unsigned)desc_b, (unsigned)XA_DESC_B);
        skipped = 1;
        goto done;
    }

    /* ---- 1.2 THE NEGATIVE ARM ------------------------------------------- *
     * A's socket is TCP_CLOSED, so with no ownership check this LISTEN would
     * SUCCEED and B would have taken over A's socket.  It is therefore also
     * this round's positive fingerprint on the DEPLOYED NSFS: an NSFS
     * predating nsfreq_sock_owned (M5-2d1) cannot make this assertion pass. */
    rc = xa_call(RQ_LISTEN, XA_DESC_A, 5u, 0u, 0u, NULL, 0u);
    printf("  B LISTEN on A's %08X: rc=%d retcode=%d errno=%d\n",
           (unsigned)XA_DESC_A, rc, (int)g_image.retcode, (int)g_image.errno_);
    wtof("TSTRQX2: B LISTEN FOREIGN %08X RETCODE=%d ERRNO=%d",
         (unsigned)XA_DESC_A, (int)g_image.retcode, (int)g_image.errno_);
    CHECK(rc == NSFV_RC_OK, "1.2: the foreign LISTEN reached the STC at all");
    CHECK_EQ((long)g_image.retcode, (long)NSF_RETERR,
             "1.2: B is REFUSED a verb on A's descriptor");
    CHECK_EQ((long)g_image.errno_, (long)NSF_EBADF,
             "1.2: refused EBADF -- foreign and never-existing are the same"
             " answer, so no verb becomes an existence oracle (ADR-0046)");

    /* ---- 1.2 THE POSITIVE CONTROL.  Same verb, B's own socket, and it must
     * be B's FIRST listen on it: tcp_listen refuses state != TCP_CLOSED
     * ("only a fresh socket may listen", src/nsftcp.c), so a REPEATED
     * positive control goes red with nothing broken. ---------------------- */
    rc = xa_call(RQ_BIND, desc_b, 0u, XA_PORT_B_TCP, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK, "B: BIND its own");
    rc = xa_call(RQ_LISTEN, desc_b, 5u, 0u, 0u, NULL, 0u);
    printf("  B LISTEN on its OWN %08X: rc=%d retcode=%d errno=%d\n",
           (unsigned)desc_b, rc, (int)g_image.retcode, (int)g_image.errno_);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK,
          "1.2 CONTROL: the SAME verb on B's OWN descriptor is SERVED -- so"
          " the refusal above is about ownership, not about LISTEN");

    /* ---- B's UDP socket for the window ---------------------------------- */
    rc = xa_call(RQ_SOCKET, 0u, (UINT)NSF_AF_INET, (UINT)NSF_SOCK_DGRAM,
                 17u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode >= 0, "B: SOCKET (datagram)");
    if (rc != NSFV_RC_OK || g_image.retcode < 0) {
        skipped = 1;
        goto done;
    }
    desc_bu = (UINT)g_image.retcode;
    rc = xa_call(RQ_BIND, desc_bu, 0u, XA_PORT_B_UDP, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK, "B: BIND it");

    if (xa_raise(XA_FLAG_ISO) != NSFV_RC_OK) {
        printf("  could not raise the isolation flag\n");
        skipped = 1;
        goto done;
    }

    xa_fill();
    xa_window(desc_bu);

    /* ---- 1.2 second half: A has gone.  B must be untouched. ------------- */
    if (xa_wait_flag(XA_FLAG_TRM, "A's TERMAPI") != 0) {
        skipped = 1;
        goto done;
    }

    /* EXERCISED, NOT MERELY RESOLVED.  A descriptor that resolves is not a
     * descriptor that works, so the UDP socket is driven and the TCP one is
     * asked a question only a LIVE, LISTENING, OWNED socket can answer.
     *
     * EINVAL IS THE ASSERTION AND EBADF WOULD BE THE FAILURE (the TSTD1B
     * idiom): EBADF means req_socket returned NULL -- gone, or no longer B's
     * -- while EINVAL means the descriptor RESOLVED, ownership HELD, and the
     * TCB exists and is not CLOSED.  So EINVAL is the survival statement in
     * positive form.  `rc == RETERR` alone would be green for any failure at
     * all, which is the absent-vs-succeeded shape moved inside the check. */
    rc = xa_call_f(RQ_RECVFROM, desc_bu, 0u, 0u, 0u, g_buf, XA_SEND_B,
                   RQ_F_NONBLOCK);
    CHECK(rc == NSFV_RC_OK && g_image.errno_ == NSF_EWOULDBLOCK,
          "1.2: B's datagram socket still WORKS after A's TERMAPI -- driven,"
          " not merely resolved, and by a verb needing no interface");
    CHECK(memcmp(g_buf, g_exp, XA_BUF) == 0,
          "1.2: and its buffer still holds B's own pattern");

    rc = xa_call(RQ_LISTEN, desc_b, 5u, 0u, 0u, NULL, 0u);
    printf("  B LISTEN on its own %08X after A's TERMAPI: retcode=%d"
           " errno=%d\n", (unsigned)desc_b, (int)g_image.retcode,
           (int)g_image.errno_);
    CHECK_EQ((long)g_image.errno_, (long)NSF_EINVAL,
             "1.2: B's stream socket is refused EINVAL, not EBADF -- it"
             " resolved, it is still B's, and it is still LISTENING");

    /* B's APP SLOT survived too: do_socket rejects a token app_index cannot
     * resolve, and the token is corroborated against B's address space at the
     * boundary, so a socket coming back at all is the app slot answering. */
    rc = xa_call(RQ_SOCKET, 0u, (UINT)NSF_AF_INET, (UINT)NSF_SOCK_DGRAM,
                 17u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode >= 0,
          "1.2: B's APP SLOT survived -- a new socket on B's token succeeds");
    if (rc == NSFV_RC_OK && g_image.retcode >= 0) {
        desc_tmp = (UINT)g_image.retcode;
        (void)xa_call(RQ_CLOSE, desc_tmp, 0u, 0u, 0u, NULL, 0u);
    }

    rc = xa_call(RQ_TERMAPI, 0u, 0u, 0u, 0u, NULL, 0u);
    CHECK(rc == NSFV_RC_OK && g_image.retcode == NSF_RETOK, "B: TERMAPI");
    g_apptok = 0u;

done:
    xa_lower(XA_FLAG_TRM);      /* A raised it; B is the one that saw it     */
    xa_lower(XA_FLAG_ISO);
    xa_lower(XA_FLAG_B);
    return skipped ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
static int
xa_finish(int skipped)
{
    int rc = mbt_test_summary("TSTRQX2");

    if (skipped) {
        printf("*** THE GATE DID NOT RUN.  This run contains no evidence"
               " about multiplexing, isolation or the landing area.\n");
        printf("*** Returning CC %d rather than %d so it cannot be mistaken"
               " for a clean pass.\n", XA_CC_GATE_SKIPPED, rc);
        wtof("TSTRQX2: GATE SKIPPED -- CC %d, NOT a pass",
             XA_CC_GATE_SKIPPED);
        return XA_CC_GATE_SKIPPED;
    }
    return rc;
}

int
main(int argc, char **argv)
{
    const char *role = (argc > 1 && argv[1] != NULL) ? argv[1] : "";
    int         rc;

    wtof("TSTRQX2: TWO-CLIENT GATE START (ROLE '%s')", role);
    printf("=== TSTRQX2 -- M5-2e Job A: two clients at once ===\n");
    printf("role: '%s'\n", role);

    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    /* A role that is neither A nor B must NOT fall through to something that
     * reports a clean pass having tested nothing. */
    if (strcmp(role, "A") == 0) {
        g_isb = 0;
        rc = xa_run_a();
    } else if (strcmp(role, "B") == 0) {
        g_isb = 1;
        rc = xa_run_b();
    } else {
        printf("  PARM must be 'A' or 'B'.  Submit RQX2A.jcl and RQX2B.jcl"
               " together: each waits up to 60 s for the other.\n");
        wtof("TSTRQX2: NO ROLE ('%s') -- nothing ran", role);
        CHECK(0, "a role was selected (a missing PARM must not report a pass)");
        return xa_finish(1);
    }

    wtof("TSTRQX2: TWO-CLIENT GATE DONE (%s)", role);
    return xa_finish(rc != 0);
}
