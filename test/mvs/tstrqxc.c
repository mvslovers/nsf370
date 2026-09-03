/*
 * tstrqxc.c -- M5-2b4: the slot pool under CONTENTION, and the retain branch.
 *
 * MVS-only (host = false): two address spaces sharing one CSA pool through a
 * private SVC has no host analog.  The host coverage of this step is the pure
 * half -- nsfreqx_actionable and the truth tables it gates -- in TSTREQX.
 *
 * ONE PROGRAM, SEVERAL ROLES, selected by PARM.  They share the anchor chase,
 * the request shape and the verification, which is the point: the SOLO run is
 * the NEGATIVE CONTROL for the two-client run, and a negative control built
 * out of different code proves less.
 *
 *   (no PARM)  SOLO      one client, sequential.  Asserts the pool behaves as
 *                        b3 measured it -- same slot back every time -- AND
 *                        that `collisions` does NOT move.  That last line is
 *                        what gives the two-client run's `collisions > 0` its
 *                        meaning; without it the counter could be ticking for
 *                        reasons that have nothing to do with a second client.
 *   PARM='A'   LEADER    a free-for-all phase (the MECHANISM check), then THE
 *                        GATE: A pre-claims all but ONE slot and asserts it is
 *                        BOTH refused (B holds the one free slot) and served
 *                        (it can still win it -- no outright starvation).
 *   PARM='B'   FOLLOWER  hammers the pool for as long as A's flag is up.  It
 *                        asserts only what it can see on its own; the gate's
 *                        hard assertions belong to A, which is the only party
 *                        that knows what phase the pool is in.
 *   PARM='LEAK'          the retain-branch INDUCTION.  Leaves one slot CLAIMED
 *                        with the in-flight count leaked, and does not clean
 *                        up -- see "THE INDUCTION" below.
 *   PARM='Vxxxxxxxx'     after `P NSFS`, read the anchor back at that address:
 *                        the positive check that the retain branch really kept
 *                        the CSA rather than merely saying so.
 *   PARM='RESET'         release every CLAIMED/HELD slot.  A recovery aid for
 *                        a run that died mid-phase; NOT part of any gate.
 *
 * M5-2e ADDS FOUR MEASUREMENT ROLES.  b4's roles answer yes/no questions from
 * end-totals and read no clock; (e) needs progress across a window, a
 * per-request delay distribution, and a window bounded by TIME rather than by
 * a request count.  See "THE MEASUREMENT ROLES" further down for what each
 * one reports and why the discard boundary is a compile-time constant.
 *
 *   PARM='MS'            measure, one client -- the reference arm for MA/MB.
 *   PARM='MA' / 'MB'     measure, two address spaces, barriered at the start.
 *   PARM='MSP'           MS with a deliberate pause inside the timed region:
 *                        the discriminating gate on the INSTRUMENT.
 *   ...'MS90' etc.       a trailing decimal shortens the window and stamps
 *                        the run a TRIAL.  It never scales the discard.
 *
 * ------------------------------------------------------------------------
 * WHY THE GATE IS SHAPED THIS WAY -- the observability problem first.
 *
 * A FAILED `CS` ON A SLOT WORD IS INVISIBLE FROM OUTSIDE THE ROUTINE.  Client
 * B scans, finds slot K taken, moves on and takes K+1 -- externally identical
 * to B having found K free, lost the compare, and moved on; and identical
 * again to B simply starting at K+1.  No arrangement of clients and slots
 * recovers the difference.  So "two clients ran at the same time and both were
 * served correctly" proves NOTHING about the claim discipline, and building
 * the gate out of that would repeat the non-discriminating shape this
 * milestone has already paid for three times.
 *
 * The two phases below therefore do DIFFERENT jobs, and it took a review to
 * say so precisely.  They are not three interchangeable witnesses.
 *
 * PHASE 1 IS A MECHANISM CHECK, NOT CONTENTION EVIDENCE.  With the other
 * client sitting on slot 0, every one of A's scans fails exactly one compare
 * and lands on slot 1, so `collisions` MUST read one per request -- 150 over
 * 150 -- and A's slot index MUST be non-zero.  Those two numbers are the SAME
 * observation counted twice, not two independent ones, and neither is a lost
 * race: what they show is that the scan walks past an occupied slot and that
 * the counter tracks it, with the SOLO run's ZERO delta as the negative
 * control.  Read the highest index the other way and it is positive evidence
 * AGAINST a lost race in this phase: had the two clients ever raced for slot
 * 1, the loser would have walked on to slot 2, and none did (measured: A
 * always slot 1, B always slot 0 -- a stable disjoint assignment).
 *
 * PHASE 2 CARRIES THE GATE, ALONE.  A holds slots 1..61 CLAIMED and the flag
 * slots are taken, so slot 0 is the only slot a request can be given, and A
 * holds no slot between its own requests -- the routine releases before it
 * returns.  So an ENOBUFS handed to A has exactly one possible cause: THE
 * OTHER ADDRESS SPACE WAS OCCUPYING SLOT 0 AT THAT INSTANT.  Both clients
 * being served AND refused across the same run is the two of them genuinely
 * alternating on one slot word.  If B is not running, A is never refused and
 * the gate FAILS, which is the behaviour a gate should have.
 *
 * WHAT IS PROVEN, then, is that two address spaces share the pool and
 * interleave correctly on one slot under saturation.  WHAT IS NOT is that two
 * CPUs executed the compare on the same word at the same instant and the
 * hardware arbitrated: `collisions` cannot separate that from "the slot was
 * already busy" (see "COLLISIONS" in nsfvsvc.h), and no test on this stand can
 * isolate it.  The stand does run Hercules with NUMCPU 2 and MVS dispatches on
 * both, so it is physically possible rather than merely defended against --
 * but possible is not observed, and that half stays construction.
 *
 * NO SLOT IS EVER HELD BY TWO CLIENTS is checked differently, and needs no
 * counter.  An unauthorised client cannot store into CSA, so it cannot stamp
 * anything itself -- but every request carries a 64-byte NSFRQE image THROUGH
 * the slot and back, and the STC writes only the six result fields into it.
 * Each client therefore fills its image with a per-request identity (client id
 * + sequence number in `reqid`, client id in `sockdesc`) and asserts the image
 * that comes back is ITS OWN.  If two clients were ever handed the same slot
 * they would stage into the same `rqe` and at least one would read the other's
 * identity.  No race in the check: the copy-back happens while the client
 * still owns the slot.
 *
 * ------------------------------------------------------------------------
 * THE INDUCTION -- why not a blocking RECV.
 *
 * `nsfsx_stop` clears ANCHOR_ACTIVE BEFORE it nudges parked clients, so a
 * healthy client parked in the SVC routine wakes, finds the anchor quiescing,
 * takes the routine's WQUIES path -- slot to FREE, in-flight decremented,
 * RCCORR to the caller -- and DRAINS ITSELF.  A blocking `accept` or `RECV`
 * therefore cleans up on the nudge and lands in the drained branch again,
 * which is what b3's two induction attempts kept discovering.  That is correct
 * behaviour, and it is why the retain branch is hard to reach at all.
 *
 * What reaches it is a slot that is CLAIMED with the in-flight count already
 * taken and NOBODY WAITING on its reply ECB: no waiter for the nudge to wake,
 * and `nsfreqx_reap_ok` excludes CLAIMED, so the death guard never reclaims
 * it either.  A client that faults inside the routine's write-IN move leaves
 * exactly that (TSTRQXF (A) measures it), because the claim and the in-flight
 * increment both happen before the move.  So the induction is that fault with
 * the cleanup deliberately withheld.
 *
 * ------------------------------------------------------------------------
 * RETURN CODES -- three, for the reason TSTRQXF has three.
 *
 *    0                     everything ran and everything passed
 *    1                     the gate RAN and something FAILED
 *    XC_CC_GATE_SKIPPED    the gate COULD NOT RUN -- no anchor, or the other
 *                          client never arrived.  A skip that reported CC 0
 *                          would make a run containing no evidence look
 *                          exactly like a run that proved something.
 *
 * PREREQUISITE: the NSFS STC must be started (S NSFS).
 * THE RED LINE IS UNCHANGED: the client is UNAUTHORISED and never self-auths.
 */
#include "nsfvsvc.h"
#include "nsfreq.h"         /* NSFRQE -- the image the request carries         */
#include "nsftime.h"        /* nsf_now -- STCK, the measurement roles          */
#include <clibecb.h>        /* ecb_timed_wait -- the poll pause                */
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                      */
#include <clibtry.h>        /* ___try -- capture the abend, no dump            */
#include <clibwto.h>        /* wtof -- survives a hang, unlike SYSPRINT        */
#include <cvt.h>            /* CVTPTR / cvtabend -- the chase starts at 16     */
#include <ihascvt.h>        /* SCVT (scvtsvct), SVCTABLE, SVCENTRY             */
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

/* "The gate could not run" -- deliberately not 1, which already means "ran and
 * failed" (mbt_test_summary).  Returned from the test; the harness is not
 * modified. */
#define XC_CC_GATE_SKIPPED 20

/* The two presence flags, taken from the TOP of the array so the claim scan --
 * which always starts at 0 -- never has to walk past them to find work, and so
 * the "exactly one slot free" arithmetic in phase 2 stays a simple range. */
#define XC_FLAG_A       63u
#define XC_FLAG_B       62u

/* Phase 2 pre-claims 1..61 and leaves slot 0 as the only slot a request can be
 * given.  62/63 are the flags, already taken. */
#define XC_HOLD_LO      1u
#define XC_HOLD_HI      61u
#define XC_HOLD_N       (XC_HOLD_HI - XC_HOLD_LO + 1u)

#define XC_N_SOLO       8u          /* SOLO: sequential requests              */
#define XC_N_PHASE1     150u        /* A: free-for-all requests                */
/* Phase 2 is bounded by ATTEMPTS, not by served requests: with one slot free
 * most attempts are refusals, and refusals are the evidence.  A does NOT back
 * off between them -- the first run of this gate did, and it starved itself:
 * both clients scan from slot 0, so the one that asks more often gets it, and
 * A asking every 10 ms against B asking continuously meant A was served ZERO
 * times out of 150.  That is a real property of a claim scan (it is not a
 * queue and makes no fairness promise), but it is not what phase 2 is for. */
#define XC_N_PHASE2     3000u       /* A: attempts with one slot free          */
#define XC_B_CAP        20000u      /* B: safety cap if A never clears its flag*/
#define XC_BARRIER_POLL 120u        /* 120 * 0.5 s = 60 s                      */
#define XC_PAUSE_HALF   50u         /* 0.50 s, in hundredths                   */
#define XC_PAUSE_TICK   1u          /* 0.01 s -- pre-claim sweeps, B backoff   */
#define XC_CLAIM_SWEEPS 40u         /* pre-claim retries (B holds slots too)   */

/* An NSFRQE function code no dispatcher case claims, so nsfreq_dispatch takes
 * its `default:` arm and completes NSF_RETERR / NSF_EINVAL.  DELIBERATE: the
 * verb must have NO side effect in the STC, because this test issues hundreds
 * of them.  An RQ_INITAPI would leave an app-registry slot held per request.
 * Do not "improve" this into a realistic verb. */
#define XC_FN_UNKNOWN   250u

/* Client ids -- they only have to differ, and to be visible in a dump. */
#define XC_ID_SOLO      0x51u
#define XC_ID_A         0xA1u
#define XC_ID_B         0xB2u

/* Search for an unreadable address (the LEAK role): step upward from our own
 * storage and stop at the first candidate we cannot read.  READ-probe only, so
 * a mapped page belonging to someone else is skipped, never corrupted.  Same
 * bounds as TSTRQXF (A), and the same reason: stay below LSQA/SWA and CSA. */
#define XC_STEP         0x10000u
#define XC_CEIL         0x00900000u

/* --------------------------------------------------------------------------
 * State that must outlive a ___try body.
 * -------------------------------------------------------------------------- */
static NSFV_ANCHOR *g_anchor;           /* discovered, never given            */
static NSFRQE       g_image;            /* the 64-byte image we send          */
static NSFV_REQ     g_req;              /* file scope: readable after a fault */
static unsigned     g_probe;            /* LEAK: the unreadable candidate     */
static char         g_own[64];          /* a known-good address to start from */
static void        *g_hop_in;
static void        *g_hop_out;
static char         g_eye[8];
static UINT         g_vsnap[6];         /* V role: the words read back        */

/* Issue the private SVC with R1 = A(req) via the EX-SVC-0 trick (ADR-0038 6);
 * the asm labels are per-file, so they stay unique in the load module. */
static void __attribute__((noinline))
xc_svc(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"
        "         LR    6,%1\n"
        "         EX    6,NSFXC0\n"
        "         B     NSFXCX\n"
        "NSFXC0   SVC   0\n"
        "NSFXCX   DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

static void
xc_req_init(NSFV_REQ *req, UINT func)
{
    memset(req, 0, sizeof *req);
    memcpy(req->eye, NSFV_REQ_EYE, 4);
    req->func = func;
    req->rc   = -1;
}

/* Wait out `hsec` hundredths without a busy loop: a local ECB nobody posts,
 * satisfied by the timer.  Same shape as nsfsx_pause in the STC. */
static void
xc_pause(unsigned hsec)
{
    ECB local = 0;

    ecb_timed_wait(&local, hsec, 0);
}

/* --- the probe verbs, all PROBE SCAFFOLDING due out in M5-2c ------------- */

static int
xc_query(UINT idx, UINT *state, UINT *infl, UINT *reap)
{
    NSFV_REQ req;

    xc_req_init(&req, NSFV_REQ_QUERY);
    req.slot = idx;
    xc_svc(&req);
    if (state) *state = req.qstate;
    if (infl)  *infl  = req.qinfl;
    if (reap)  *reap  = req.qreap;
    return req.rc;
}

/* CS one named slot from `expect` to `set`.  A CS and not a blind store, so a
 * pre-claim can be ASSERTED to have taken rather than inferred from the
 * absence of a complaint (CLAUDE.md 8.5). */
static int
xc_slot_cas(UINT idx, UINT expect, UINT set)
{
    NSFV_REQ req;

    xc_req_init(&req, NSFV_REQ_SLOT);
    req.slot    = idx;
    req.sexpect = expect;
    req.snew    = set;
    xc_svc(&req);
    return req.rc;
}

/* --- the anchor chase, one hop per ___try -------------------------------- */

static int t_hop_cvt(void)
{
    g_hop_out = (void *)CVTPTR;
    return 0;
}
static int t_hop_scvt(void)
{
    g_hop_out = ((CVT *)g_hop_in)->cvtabend;
    return 0;
}
static int t_hop_svct(void)
{
    g_hop_out = ((SCVT *)g_hop_in)->scvtsvct;
    return 0;
}
static int t_hop_epa(void)
{
    g_hop_out = ((SVCTABLE *)g_hop_in)->svcentry[NSFV_SVCNUM].svcepa;
    return 0;
}
static int t_hop_anch(void)
{
    g_hop_out = *(void **)((unsigned char *)g_hop_in + NSFV_ANCH_OFF);
    return 0;
}
static int t_hop_eye(void)
{
    memcpy(g_eye, ((NSFV_ANCHOR *)g_hop_in)->eye, sizeof g_eye);
    return 0;
}

static void *
xc_hop(const char *name, int (*body)(void), void *in, int *failed)
{
    int rc;

    if (*failed) return NULL;
    g_hop_in  = in;
    g_hop_out = NULL;
    rc = ___try(body);
    if (rc != 0) {
        printf("  chase %-6s FAULTED (try rc=%08X)\n", name, (unsigned)rc);
        wtof("TSTRQXC: chase %s FAULTED rc=%08X", name, (unsigned)rc);
        *failed = 1;
        return NULL;
    }
    printf("  chase %-6s -> %08X\n", name, (unsigned)g_hop_out);
    return g_hop_out;
}

/* Find NSF's anchor the way the routine itself does: the SVC-table entry for
 * our stolen SVC holds the routine's CSA entry point, and NSFV_ANCH_OFF bytes
 * into it is the word the STC patched.  READ-ONLY, hop by hop, so a failure
 * says WHICH hop failed.  Returns 0 on success. */
static int
xc_find_anchor(void)
{
    int   failed = 0;
    void *cvt, *scvt, *svct, *epa, *anch;

    cvt  = xc_hop("CVT",   t_hop_cvt,  NULL, &failed);
    scvt = xc_hop("SCVT",  t_hop_scvt, cvt,  &failed);
    svct = xc_hop("SVCT",  t_hop_svct, scvt, &failed);
    epa  = xc_hop("EPA",   t_hop_epa,  svct, &failed);
    if (!failed) {
        /* 24-bit machine: mask the entry-point word before using it as an
        ** address, so a flag byte can never become part of it. */
        epa = (void *)((unsigned)epa & 0x00FFFFFFu);
        printf("  chase EP     -> %08X (SVC %u routine, in CSA)\n",
               (unsigned)epa, (unsigned)NSFV_SVCNUM);
    }
    anch = xc_hop("ANCHOR", t_hop_anch, epa, &failed);
    if (failed || anch == NULL) return -1;

    memset(g_eye, 0, sizeof g_eye);
    failed = 0;
    (void)xc_hop("EYE", t_hop_eye, anch, &failed);
    if (failed || memcmp(g_eye, "NSFVANCR", 8) != 0) return -1;

    g_anchor = (NSFV_ANCHOR *)anch;
    return 0;
}

/* --------------------------------------------------------------------------
 * One request, and the check that the reply is OURS.
 *
 * The image carries a per-request identity in fields the STC never writes
 * (ADR-0041 4 writes retcode / errno_ / apptok / p1 / p2 / p3 and nothing
 * else), so an image that comes back with someone else's identity means one
 * slot was handed to two clients.
 * -------------------------------------------------------------------------- */
static void
xc_image_fill(UINT id, UINT seq)
{
    memset(&g_image, 0, sizeof g_image);
    memcpy(g_image.eye, NSFRQE_EYE, 4);
    g_image.fn       = (USHORT)XC_FN_UNKNOWN;
    g_image.sockdesc = (id << 24) | 0x00C1C1u;      /* the client id           */
    g_image.reqid    = (id << 24) | (seq & 0x00FFFFFFu);
}

/* Issue one request.  Returns the router rc; *slot gets the index claimed and
 * *mine is set to 0 if the image that came back is not the one we sent. */
static int
xc_request(UINT id, UINT seq, UINT *slot, int *mine)
{
    UINT want_sock = (id << 24) | 0x00C1C1u;
    UINT want_id   = (id << 24) | (seq & 0x00FFFFFFu);

    xc_image_fill(id, seq);
    xc_req_init(&g_req, NSFV_REQ_RQE);
    g_req.ubuf   = NULL;
    g_req.ulen   = 0u;
    g_req.rqeimg = (void *)&g_image;
    g_req.slot   = 0xFFFFu;             /* so an unwritten field is obvious   */
    xc_svc(&g_req);

    if (slot) *slot = g_req.slot;
    if (mine) {
        *mine = 1;
        if (g_req.rc == NSFV_RC_OK) {
            /* Ours, and serviced: the identity fields survived the round trip
            ** and the STC wrote the result fields it owns. */
            if (g_image.reqid    != want_id)       *mine = 0;
            if (g_image.sockdesc != want_sock)     *mine = 0;
            if (g_image.fn       != XC_FN_UNKNOWN) *mine = 0;
            if (g_image.retcode  != NSF_RETERR)    *mine = 0;
            if (g_image.errno_   != NSF_EINVAL)    *mine = 0;
        }
    }
    return g_req.rc;
}

/* --------------------------------------------------------------------------
 * SOLO -- the negative control.
 * -------------------------------------------------------------------------- */
static int
xc_run_solo(void)
{
    UINT coll0, coll1, exh0, exh1;
    UINT k, slot, same = 1, first = 0xFFFFu;
    int  mine, rc, bad = 0;

    printf("\n--- SOLO: one client, and the counter that must NOT move ---\n");
    coll0 = g_anchor->collisions;
    exh0  = g_anchor->exhausted;

    for (k = 0; k < XC_N_SOLO; k++) {
        rc = xc_request(XC_ID_SOLO, k, &slot, &mine);
        if (rc != NSFV_RC_OK) { bad++; continue; }
        if (!mine)            { bad++; }
        if (k == 0) first = slot; else if (slot != first) same = 0;
    }
    coll1 = g_anchor->collisions;
    exh1  = g_anchor->exhausted;

    printf("  %u requests, first slot %u, all the same: %s\n",
           (unsigned)XC_N_SOLO, (unsigned)first, same ? "yes" : "NO");
    printf("  collisions %u -> %u   exhausted %u -> %u\n",
           (unsigned)coll0, (unsigned)coll1, (unsigned)exh0, (unsigned)exh1);
    wtof("TSTRQXC: SOLO slot=%u coll %u->%u", (unsigned)first,
         (unsigned)coll0, (unsigned)coll1);

    CHECK_EQ((long)bad, 0L,
             "SOLO: every request was served and came back with its own"
             " identity");
    CHECK(same, "SOLO: a released slot comes straight back (b3's reuse rule)");
    CHECK_EQ((long)first, 0L, "SOLO: and it is the lowest slot");
    /* THE NEGATIVE CONTROL.  One client releases before it asks again, so the
     * scan finds slot 0 FREE every time and never fails a compare.  This line
     * is what makes `collisions > 0` in the two-client run mean "a second
     * address space was there" rather than "the counter ticks". */
    CHECK_EQ((long)(coll1 - coll0), 0L,
             "SOLO: NO contended claim -- the collision counter does not move"
             " for a single sequential client (the negative control)");
    CHECK_EQ((long)(exh1 - exh0), 0L,
             "SOLO: and the pool is never exhausted by one client");
    return mbt_test_summary("TSTRQXC");
}

/* --------------------------------------------------------------------------
 * The barrier.  Each client CS's its own flag slot up and waits for the
 * other's, reading the state straight out of CSA (which is readable from key 8
 * -- TSTRQXF proved that hop).  Returns 0 when the other client showed up.
 * -------------------------------------------------------------------------- */
static int
xc_barrier(UINT mine, UINT theirs)
{
    unsigned n;

    if (xc_slot_cas(mine, NSFV_REQ_FREE, NSFV_REQ_HELD) != NSFV_RC_OK) {
        printf("  BARRIER: could not raise flag slot %u -- is a previous run"
               " still holding it?  (PARM='RESET' clears it)\n",
               (unsigned)mine);
        return -1;
    }
    printf("  flag slot %u raised; waiting for the other client on slot %u\n",
           (unsigned)mine, (unsigned)theirs);
    for (n = 0; n < XC_BARRIER_POLL; n++) {
        if (g_anchor->slots[theirs].req_state == NSFV_REQ_HELD) {
            printf("  the other client is here (after %u polls)\n", n);
            return 0;
        }
        xc_pause(XC_PAUSE_HALF);
    }
    printf("  BARRIER TIMED OUT after %u polls -- the other client never"
           " arrived\n", (unsigned)XC_BARRIER_POLL);
    (void)xc_slot_cas(mine, NSFV_REQ_HELD, NSFV_REQ_FREE);
    return -1;
}

/* --------------------------------------------------------------------------
 * A -- the leader, and the only party that asserts.
 * -------------------------------------------------------------------------- */
static int
xc_run_a(void)
{
    UINT coll0, coll1, exh0, exh1;
    UINT k, slot, sweep, held, n;
    UINT nonzero = 0, hi = 0;
    UINT ok2 = 0, nobuf2 = 0, wrong2 = 0;
    int  mine, rc, bad = 0;

    printf("\n--- A: the two-client contention gate ---\n");
    if (xc_barrier(XC_FLAG_A, XC_FLAG_B) != 0) {
        wtof("TSTRQXC: A BARRIER FAILED -- gate skipped");
        return -1;
    }

    /* ---- phase 1: free-for-all ---------------------------------------- */
    printf("\n  phase 1: %u requests, the whole pool free\n",
           (unsigned)XC_N_PHASE1);
    coll0 = g_anchor->collisions;
    for (k = 0; k < XC_N_PHASE1; k++) {
        rc = xc_request(XC_ID_A, k, &slot, &mine);
        if (rc != NSFV_RC_OK || !mine) { bad++; continue; }
        if (slot >= NSFV_NSLOTS)       { bad++; continue; }
        if (slot != 0u)                { nonzero++; if (slot > hi) hi = slot; }
    }
    coll1 = g_anchor->collisions;
    printf("  served=%u bad=%u | slots other than 0: %u (highest %u)\n",
           (unsigned)(XC_N_PHASE1 - (UINT)bad), (unsigned)bad,
           (unsigned)nonzero, (unsigned)hi);
    printf("  collisions %u -> %u (delta %u)\n",
           (unsigned)coll0, (unsigned)coll1, (unsigned)(coll1 - coll0));
    wtof("TSTRQXC: A phase1 bad=%d nonzero=%u coll delta=%u",
         bad, (unsigned)nonzero, (unsigned)(coll1 - coll0));

    CHECK_EQ((long)bad, 0L,
             "phase 1: every request was served and came back with its own"
             " identity -- NO slot was handed to two clients");
    /* MECHANISM, not contention.  These two are the SAME observation counted
     * twice -- the other client sits on slot 0, so each scan fails exactly one
     * compare and lands on slot 1.  What they establish is that the scan walks
     * past an occupied slot and the counter tracks it; the SOLO run's zero
     * delta is the negative control that gives them meaning.  The gate itself
     * is phase 2. */
    CHECK((coll1 - coll0) > 0u,
          "MECHANISM: the claim scan stepped over slots that were NOT FREE and"
          " `collisions` tracked it -- which one sequential client cannot do");
    CHECK(nonzero > 0u,
          "MECHANISM: A was given a slot other than 0 -- the same fact from the"
          " other side, and NOT a second independent witness");
    /* And the shape of the walk is evidence AGAINST a lost race here: a client
     * that lost the compare on slot 1 would have gone on to slot 2. */
    printf("  highest index reached: %u (a lost race for slot 1 would have"
           " pushed a client to slot 2)\n", (unsigned)hi);

    /* ---- phase 2: exactly one slot free -------------------------------- */
    printf("\n  phase 2: pre-claiming slots %u..%u, leaving ONLY slot 0\n",
           (unsigned)XC_HOLD_LO, (unsigned)XC_HOLD_HI);
    /* B is hammering, so it holds a slot from time to time and a single sweep
     * will not take them all.  Sweep until it does -- and ASSERT the final
     * count, so a partial pre-claim can never be mistaken for a full one. */
    held = 0;
    for (sweep = 0; sweep < XC_CLAIM_SWEEPS && held < XC_HOLD_N; sweep++) {
        held = 0;
        for (k = XC_HOLD_LO; k <= XC_HOLD_HI; k++) {
            if (g_anchor->slots[k].req_state == NSFV_REQ_CLAIMED) { held++; continue; }
            if (xc_slot_cas(k, NSFV_REQ_FREE, NSFV_REQ_CLAIMED) == NSFV_RC_OK) held++;
        }
        if (held < XC_HOLD_N) xc_pause(XC_PAUSE_TICK);
    }
    printf("  pre-claimed %u of %u after %u sweep(s)\n",
           (unsigned)held, (unsigned)XC_HOLD_N, (unsigned)sweep);
    CHECK_EQ((long)held, (long)XC_HOLD_N,
             "phase 2: all but one slot really is pre-claimed (a partial"
             " pre-claim would make the refusals below mean nothing)");
    if (held != XC_HOLD_N) {
        for (k = XC_HOLD_LO; k <= XC_HOLD_HI; k++)
            (void)xc_slot_cas(k, NSFV_REQ_CLAIMED, NSFV_REQ_FREE);
        (void)xc_slot_cas(XC_FLAG_A, NSFV_REQ_HELD, NSFV_REQ_FREE);
        wtof("TSTRQXC: A pre-claim incomplete -- gate skipped");
        return -1;
    }

    exh0 = g_anchor->exhausted;
    for (k = 0; k < XC_N_PHASE2; k++) {
        rc = xc_request(XC_ID_A, 0x800000u + k, &slot, &mine);
        if (rc == NSFV_RC_NOBUF) { nobuf2++; continue; }
        if (rc != NSFV_RC_OK || !mine) { wrong2++; continue; }
        if (slot != 0u)                { wrong2++; continue; }
        ok2++;
    }
    exh1 = g_anchor->exhausted;
    printf("  %u attempts: served=%u refused=%u wrong=%u | exhausted %u -> %u\n",
           (unsigned)XC_N_PHASE2, (unsigned)ok2, (unsigned)nobuf2,
           (unsigned)wrong2, (unsigned)exh0, (unsigned)exh1);
    wtof("TSTRQXC: A phase2 ok=%u nobuf=%u wrong=%u exh delta=%u",
         (unsigned)ok2, (unsigned)nobuf2, (unsigned)wrong2,
         (unsigned)(exh1 - exh0));

    CHECK_EQ((long)wrong2, 0L,
             "phase 2: every served request got slot 0 -- the only free slot --"
             " and its own identity back");
    /* Both directions have to happen, and each says something the other does
     * not: served-at-least-once says A can still win the single free slot (no
     * starvation), refused-at-least-once is witness 3. */
    CHECK(ok2 > 0u,
          "phase 2: A won the single free slot at least once -- the scan does"
          " not starve one address space outright");
    /* THE GATE, and the only part of this run that carries it.  A holds no
     * slot between its own requests, and slot 0 is the only slot a request can
     * be given, so a refusal has exactly one possible cause.  Together with
     * the served count above, it is the two address spaces alternating on ONE
     * slot word. */
    CHECK(nobuf2 > 0u,
          "THE GATE: A was REFUSED while slot 0 was the only free slot -- so"
          " the OTHER ADDRESS SPACE was occupying it at that instant");
    CHECK((exh1 - exh0) >= nobuf2,
          "phase 2: the anchor's `exhausted` counter accounts for every"
          " refusal A saw (B's refusals count too, so it may be higher)");

    /* ---- give the pool back, in this order ------------------------------ */
    n = 0;
    for (k = XC_HOLD_LO; k <= XC_HOLD_HI; k++) {
        if (xc_slot_cas(k, NSFV_REQ_CLAIMED, NSFV_REQ_FREE) == NSFV_RC_OK) n++;
    }
    CHECK_EQ((long)n, (long)XC_HOLD_N,
             "the pre-claimed slots were all released again");
    /* Release the pool BEFORE lowering the flag: B stops when the flag drops,
     * and it should not stop while the pool is still restricted. */
    CHECK_EQ((long)xc_slot_cas(XC_FLAG_A, NSFV_REQ_HELD, NSFV_REQ_FREE),
             (long)NSFV_RC_OK, "A lowered its flag");

    /* Report B's exit; do NOT assert on it.  A cannot assert the state of the
     * whole pool, because B is legitimately still using it -- B stops when it
     * next sees A's flag down, which is one request later, and a request can
     * take a while (see the report's note on the transport's wake latency).
     * "The pool is clean at the end" therefore belongs to whoever leaves LAST,
     * which is B. */
    for (n = 0; n < XC_BARRIER_POLL; n++) {
        if (g_anchor->slots[XC_FLAG_B].req_state == NSFV_REQ_FREE) break;
        xc_pause(XC_PAUSE_HALF);
    }
    held = 0;
    for (k = 0; k < NSFV_NSLOTS; k++) {
        if (g_anchor->slots[k].req_state == NSFV_REQ_FREE) held++;
    }
    printf("  B lowered its flag: %s | slots FREE as A leaves: %u of %u\n",
           (g_anchor->slots[XC_FLAG_B].req_state == NSFV_REQ_FREE)
               ? "yes" : "not yet (it is still running -- B checks the pool)",
           (unsigned)held, (unsigned)NSFV_NSLOTS);
    return 0;
}

/* --------------------------------------------------------------------------
 * B -- the follower.  It creates the contention A measures; the gate's hard
 * assertions belong to A, which is the only party that knows what phase the
 * pool is in.  B asserts only what is true of it regardless of phase.
 * -------------------------------------------------------------------------- */
static int
xc_run_b(void)
{
    UINT k, slot;
    UINT ok = 0, nobuf = 0, nonzero = 0;
    int  mine, rc, bad = 0;

    printf("\n--- B: the other address space ---\n");
    if (xc_barrier(XC_FLAG_B, XC_FLAG_A) != 0) {
        wtof("TSTRQXC: B BARRIER FAILED -- gate skipped");
        return -1;
    }

    for (k = 0; k < XC_B_CAP; k++) {
        if (g_anchor->slots[XC_FLAG_A].req_state != NSFV_REQ_HELD) break;
        rc = xc_request(XC_ID_B, k, &slot, &mine);
        if (rc == NSFV_RC_NOBUF) {
            nobuf++;
            xc_pause(XC_PAUSE_TICK);
            continue;
        }
        if (rc != NSFV_RC_OK || !mine) { bad++; continue; }
        ok++;
        if (slot != 0u) nonzero++;
    }
    printf("  served=%u refused=%u bad=%u | slots other than 0: %u\n",
           (unsigned)ok, (unsigned)nobuf, (unsigned)bad, (unsigned)nonzero);
    printf("  stopped because A %s (after %u iterations)\n",
           (k < XC_B_CAP) ? "lowered its flag" : "NEVER lowered its flag",
           (unsigned)k);
    wtof("TSTRQXC: B ok=%u nobuf=%u bad=%u nonzero=%u",
         (unsigned)ok, (unsigned)nobuf, (unsigned)bad, (unsigned)nonzero);

    CHECK_EQ((long)bad, 0L,
             "B: every served request came back with ITS OWN identity -- no"
             " slot was handed to two clients");
    CHECK(ok > 0u, "B: it was served at least once");
    CHECK(k < XC_B_CAP,
          "B: A finished and lowered its flag (the cap is a safety net, not an"
          " expected exit)");

    CHECK_EQ((long)xc_slot_cas(XC_FLAG_B, NSFV_REQ_HELD, NSFV_REQ_FREE),
             (long)NSFV_RC_OK, "B lowered its flag");

    /* B IS THE LAST ONE OUT, so the pool-clean check is its job: it stops only
     * after A's flag is down, and A releases everything before lowering it.
     * If this fails, a later gate in the round would inherit a restricted pool
     * and fail for a reason that has nothing to do with what it tests. */
    {
        UINT free_now = 0;

        for (k = 0; k < NSFV_NSLOTS; k++) {
            if (g_anchor->slots[k].req_state == NSFV_REQ_FREE) free_now++;
        }
        printf("  slots FREE at exit: %u of %u\n",
               (unsigned)free_now, (unsigned)NSFV_NSLOTS);
        CHECK_EQ((long)free_now, (long)NSFV_NSLOTS,
                 "the whole pool is FREE again -- the round leaves nothing"
                 " behind for the next gate to trip over");
    }
    return 0;
}

/* ==========================================================================
 * M5-2e stage a: THE MEASUREMENT ROLES.
 *
 * TSTRQXC was built to prove a claim discipline, not to measure one.  The
 * b4 roles above answer yes/no questions with end-totals, and three things
 * (e) needs are absent from them by construction:
 *
 *   - PROGRESS ACROSS THE WINDOW.  A total at the end cannot tell a client
 *     that ran evenly from one that starved for four minutes and caught up
 *     at teardown.  The measurement roles record a CHECKPOINT every
 *     XC_CHK_S seconds and print the per-interval deltas.
 *   - PER-REQUEST DELAY.  b4 reads no clock at all.  Each request is now
 *     timed across the SVC with nsf_now (STCK), and the samples go into a
 *     fixed histogram -- a histogram and not an array because memory is the
 *     #1 target constraint, and min/max/sum alongside it so a badly chosen
 *     bucket boundary costs resolution rather than the whole reading.
 *   - A TIMED WINDOW.  b4 loops a fixed REQUEST COUNT, which makes the
 *     window length an output rather than an input.  These roles run for
 *     XC_WIN_S seconds and stop on their own clock.
 *
 * THE START-UP PHASE IS DISCARDED FROM THE FIGURES, NOT FROM THE RUN.  The
 * first XC_DISCARD_S seconds are measured like any other and then reported
 * SEPARATELY: `_all` covers the whole window, `_rep` starts at the crossing.
 * Both are printed, so the discard is auditable rather than asserted, and
 * the boundary is a compile-time constant -- it cannot be chosen after
 * seeing the data.
 *
 * ROLES.  MS is the single-client reference for MA/MB: the same instrument,
 * the same verb, no second address space.  Without it "two clients are no
 * faster than one" has no one-client number of its own to stand on.
 *
 *   PARM='MS'   measure, one client (the reference arm)
 *   PARM='MA'   measure, leader   -- barriers with MB, then measures
 *   PARM='MB'   measure, follower -- barriers with MA, then measures
 *   PARM='MSP'  MS with a DELIBERATE pause inside the timed region: the
 *               discriminating gate on the instrument itself (see below).
 *
 * A TRAILING DECIMAL SHORTENS THE WINDOW AND STAMPS THE RUN AS A TRIAL --
 * 'MS90' is a 90-second window.  The discard is NOT scaled with it: it stays
 * XC_DISCARD_S whatever the window, because a discard chosen to fit the run
 * is the thing 5's red line forbids.  A plain 'MS'/'MA'/'MB' is always the
 * full window, so a measurement run cannot inherit a trial's shape by
 * accident.
 *
 * WHY MSP EXISTS.  A latency figure that is simply wrong looks exactly like
 * a fast transport, and a histogram whose samples all land in bucket 0
 * looks exactly like a well-behaved one.  MSP pauses ~XC_PACE_CS centiseconds
 * INSIDE the timed region, so the instrument must report at least that much
 * or it is broken: it asserts min, mean AND that every bucket below 5 ms is
 * EMPTY -- min/max alone would pass with the bucketing broken.  It measures
 * the instrument, not the transport, and says so.
 * ========================================================================== */

#define XC_WIN_S        300u    /* the measurement window, seconds            */
#define XC_DISCARD_S     60u    /* start-up phase, EXCLUDED FROM THE FIGURES  */
#define XC_CHK_S         15u    /* checkpoint interval                        */
#define XC_CHK_MAX       48u    /* 300/15 = 20 needed; the cap is a backstop  */
#define XC_PACE_CS        1u    /* MSP: 1 centisecond = 10 ms                 */
#define XC_MEAS_CAP 4000000u    /* runaway backstop; the clock ends the run   */

/* Bucket upper bounds in microseconds, FIXED HERE and not derived from any
 * run.  13 buckets: twelve bounds plus everything above the last. */
#define XC_NBUCKET      13u
static const UINT g_bound_us[XC_NBUCKET - 1u] = {
    100u, 250u, 500u, 750u, 1000u, 1500u,
    2500u, 5000u, 10000u, 50000u, 250000u, 1000000u
};

typedef struct xc_hist {
    UINT n;
    UINT sum_us;                /* <= the window in us, so it cannot wrap     */
    UINT min_us;
    UINT max_us;
    UINT b[XC_NBUCKET];
} XC_HIST;

typedef struct xc_chk {
    UINT t_ms;                  /* ms since the window opened                 */
    UINT served;
    UINT refused;
    UINT bad;
} XC_CHK;

static XC_HIST g_srv_all, g_srv_rep;    /* served-request latency             */
static XC_HIST g_ref_all, g_ref_rep;    /* refused-request (NOBUF) latency    */
static XC_CHK  g_chk[XC_CHK_MAX];
static UINT    g_nchk;
static UINT    g_pace;                  /* MSP: centiseconds inside the timing*/
static UINT    g_win_s = XC_WIN_S;      /* overridden by a trailing decimal   */
static int     g_trial;                 /* 1 when the window was shortened    */

/* Microseconds between two STCK readings: (b - a) >> 12 (TOD bit 51 == 1 us).
 * Lifted from test/mvs/tsttmacc.c, which is where this arithmetic was first
 * written and checked against a known 100 ms interval. */
static UINT
xc_tod_us(const NSFTIME *a, const NSFTIME *b)
{
    UINT dlo    = b->lo - a->lo;
    UINT borrow = (b->lo < a->lo) ? 1u : 0u;
    UINT dhi    = b->hi - a->hi - borrow;

    return (dhi << 20) | (dlo >> 12);
}

static void
xc_hist_reset(XC_HIST *h)
{
    UINT i;

    h->n = 0u;
    h->sum_us = 0u;
    h->min_us = 0xFFFFFFFFu;
    h->max_us = 0u;
    for (i = 0u; i < XC_NBUCKET; i++) h->b[i] = 0u;
}

static void
xc_hist_add(XC_HIST *h, UINT us)
{
    UINT i;

    h->n++;
    h->sum_us += us;
    if (us < h->min_us) h->min_us = us;
    if (us > h->max_us) h->max_us = us;
    for (i = 0u; i < XC_NBUCKET - 1u; i++) {
        if (us <= g_bound_us[i]) { h->b[i]++; return; }
    }
    h->b[XC_NBUCKET - 1u]++;
}

static void
xc_hist_print(const char *what, const XC_HIST *h)
{
    UINT i;

    if (h->n == 0u) {
        printf("  %-8s n=0 -- no samples\n", what);
        return;
    }
    printf("  %-8s n=%u  min=%u us  mean=%u us  max=%u us\n",
           what, (unsigned)h->n, (unsigned)h->min_us,
           (unsigned)(h->sum_us / h->n), (unsigned)h->max_us);
    for (i = 0u; i < XC_NBUCKET; i++) {
        if (h->b[i] == 0u) continue;
        if (i < XC_NBUCKET - 1u) {
            printf("             <=%8u us : %u\n",
                   (unsigned)g_bound_us[i], (unsigned)h->b[i]);
        } else {
            printf("             > %8u us : %u\n",
                   (unsigned)g_bound_us[XC_NBUCKET - 2u], (unsigned)h->b[i]);
        }
    }
}

/* Requests per second, computed so it cannot overflow at the rates this
 * transport reaches: n*1000 stays inside a UINT up to ~4.2M requests, and the
 * raw n and ms are printed beside it so any other rate can be recomputed. */
static UINT
xc_rate(UINT n, UINT ms)
{
    if (ms == 0u) return 0u;
    return (n * 1000u) / ms;
}

/* --------------------------------------------------------------------------
 * The driver.  One timed window; MS/MA/MB/MSP differ only in the barrier and
 * in what they assert afterwards.
 * -------------------------------------------------------------------------- */
static void
xc_measure(UINT id, const char *what)
{
    NSFTIME  t_win, t0, t1, t_now;
    UINT     srv = 0u, ref = 0u, bad = 0u;
    UINT     srv_w = 0u, ref_w = 0u, bad_w = 0u;   /* counts at the crossing */
    UINT     el_us = 0u, warm_us = 0u;
    UINT     next_chk = 0u;
    UINT     k, slot, us;
    int      mine, rc, warm = 0;

    xc_hist_reset(&g_srv_all); xc_hist_reset(&g_srv_rep);
    xc_hist_reset(&g_ref_all); xc_hist_reset(&g_ref_rep);
    g_nchk = 0u;

    printf("\n--- %s: %u s window, first %u s discarded from the figures ---\n",
           what, (unsigned)g_win_s, (unsigned)XC_DISCARD_S);
    if (g_trial) {
        printf("  *** TRIAL SHAPE (window shortened) -- NOT A MEASUREMENT"
               " RUN ***\n");
    }
    /* Printed whether or not it is armed: a paced arm whose pace silently
     * failed to arm reports the same clean pass as an unpaced one, which is
     * exactly how the first MSP trial passed 8/8 having tested nothing. */
    printf("  pace inside the timed region: %u cs\n", (unsigned)g_pace);
    nsf_now(&t_win);
    printf("  window opens at TOD %08X%08X\n",
           (unsigned)t_win.hi, (unsigned)t_win.lo);
    wtof("TSTRQXC: %s window opens TOD %08X%08X", what,
         (unsigned)t_win.hi, (unsigned)t_win.lo);

    for (k = 0u; k < XC_MEAS_CAP; k++) {
        nsf_now(&t0);
        rc = xc_request(id, k, &slot, &mine);
        if (g_pace != 0u) xc_pause(g_pace);   /* MSP only: times the clock   */
        nsf_now(&t1);
        us = xc_tod_us(&t0, &t1);

        if (rc == NSFV_RC_NOBUF) {
            ref++;
            xc_hist_add(&g_ref_all, us);
            if (warm) xc_hist_add(&g_ref_rep, us);
        } else if (rc != NSFV_RC_OK || !mine || slot >= NSFV_NSLOTS) {
            bad++;
        } else {
            srv++;
            xc_hist_add(&g_srv_all, us);
            if (warm) xc_hist_add(&g_srv_rep, us);
        }

        el_us = xc_tod_us(&t_win, &t1);

        /* The crossing.  Snapshot the counters; the `_rep` histograms simply
         * start filling here, so the reported region needs no subtraction. */
        if (!warm && el_us >= XC_DISCARD_S * 1000000u) {
            warm = 1;
            warm_us = el_us;
            srv_w = srv; ref_w = ref; bad_w = bad;
            printf("  discard boundary crossed at %u ms:"
                   " served=%u refused=%u bad=%u\n",
                   (unsigned)(el_us / 1000u), (unsigned)srv, (unsigned)ref,
                   (unsigned)bad);
        }

        /* Progress, at several points across the window.  This is the whole
         * reason the figures are not a single end-total. */
        if (g_nchk < XC_CHK_MAX && el_us >= next_chk) {
            g_chk[g_nchk].t_ms    = el_us / 1000u;
            g_chk[g_nchk].served  = srv;
            g_chk[g_nchk].refused = ref;
            g_chk[g_nchk].bad     = bad;
            g_nchk++;
            /* Advance to the next grid point PAST `el_us`, not by a fixed
             * increment: a single request that stalls over two boundaries
             * would otherwise leave next_chk behind el_us, fire again on the
             * very next iteration, and report an interval that is not
             * XC_CHK_S at all.  Under #64 a multi-second stall is not
             * hypothetical.  A skipped grid point shows up as a t_ms column
             * that jumps by 2*XC_CHK_S -- visible, which a silently short
             * interval would not be. */
            do {
                next_chk += XC_CHK_S * 1000000u;
            } while (next_chk <= el_us);
        }

        if (el_us >= g_win_s * 1000000u) break;
    }
    nsf_now(&t_now);
    el_us = xc_tod_us(&t_win, &t_now);

    printf("  window closes at TOD %08X%08X (%u ms elapsed, %u attempts)\n",
           (unsigned)t_now.hi, (unsigned)t_now.lo,
           (unsigned)(el_us / 1000u), (unsigned)k);
    wtof("TSTRQXC: %s closes TOD %08X%08X srv=%u ref=%u bad=%u", what,
         (unsigned)t_now.hi, (unsigned)t_now.lo,
         (unsigned)srv, (unsigned)ref, (unsigned)bad);

    /* ---- progress ------------------------------------------------------- */
    printf("\n  PROGRESS (per interval, not cumulative):\n");
    printf("      t_ms   served   refused   bad\n");
    for (k = 0u; k < g_nchk; k++) {
        UINT ps = (k == 0u) ? g_chk[0].served  : g_chk[k].served  - g_chk[k-1].served;
        UINT pr = (k == 0u) ? g_chk[0].refused : g_chk[k].refused - g_chk[k-1].refused;
        UINT pb = (k == 0u) ? g_chk[0].bad     : g_chk[k].bad     - g_chk[k-1].bad;

        printf("    %7u  %7u   %7u  %4u\n", (unsigned)g_chk[k].t_ms,
               (unsigned)ps, (unsigned)pr, (unsigned)pb);
    }

    /* ---- totals, both regions ------------------------------------------- */
    printf("\n  WHOLE WINDOW      : %u ms  served=%u refused=%u bad=%u"
           "  (%u served/s)\n",
           (unsigned)(el_us / 1000u), (unsigned)srv, (unsigned)ref,
           (unsigned)bad, (unsigned)xc_rate(srv, el_us / 1000u));
    xc_hist_print("served", &g_srv_all);
    xc_hist_print("refused", &g_ref_all);

    if (warm) {
        UINT rep_ms = (el_us - warm_us) / 1000u;

        printf("\n  REPORTED (first %u s discarded at %u ms): %u ms"
               "  served=%u refused=%u bad=%u  (%u served/s)\n",
               (unsigned)XC_DISCARD_S, (unsigned)(warm_us / 1000u),
               (unsigned)rep_ms, (unsigned)(srv - srv_w),
               (unsigned)(ref - ref_w), (unsigned)(bad - bad_w),
               (unsigned)xc_rate(srv - srv_w, rep_ms));
        xc_hist_print("served", &g_srv_rep);
        xc_hist_print("refused", &g_ref_rep);
    } else {
        printf("\n  REPORTED: NOTHING -- the window ended before the %u s"
               " discard boundary.\n", (unsigned)XC_DISCARD_S);
    }

    /* ---- what every measurement role asserts ----------------------------- */
    CHECK_EQ((long)bad, 0L,
             "every served request came back with ITS OWN identity -- no slot"
             " was handed to two clients");
    CHECK(srv > 0u, "the client was served at least once");
    /* THE POSITIVE CONTROL ON THE SAMPLING ITSELF.  A checkpoint table of
     * zeros because sampling never fired reads exactly like a client that
     * made no progress (CLAUDE.md 8.5). */
    CHECK(g_nchk > 1u,
          "progress was SAMPLED at more than one point -- an unsampled window"
          " would print a table that looks like a starved client");
    CHECK(g_srv_all.n == srv,
          "every served request contributed a latency sample -- the timing"
          " covered the run rather than part of it");
    CHECK(warm != 0,
          "the window outlived the discard boundary, so the reported region"
          " is not empty");
}

/* MSP -- the discriminating gate on the instrument, not on the transport. */
static void
xc_assert_paced(void)
{
    UINT i, below = 0u;

    for (i = 0u; i < 6u; i++) below += g_srv_rep.b[i];   /* every bucket <5 ms */

    printf("\n  PACED SELF-TEST: pause %u cs inside the timed region\n",
           (unsigned)XC_PACE_CS);
    printf("  samples below 5 ms: %u (must be 0)\n", (unsigned)below);
    CHECK(g_pace != 0u,
          "the pace was ARMED -- an un-armed paced arm measures nothing and"
          " would otherwise report the same clean pass as MS");
    CHECK(g_srv_rep.n > 0u, "the paced arm produced samples to judge");
    CHECK(g_srv_rep.min_us >= 9000u,
          "PACED: the FASTEST request measured at least 9 ms -- a clock that"
          " does not advance would report 0");
    CHECK(g_srv_rep.n > 0u && (g_srv_rep.sum_us / g_srv_rep.n) >= 9000u,
          "PACED: and so did the mean");
    CHECK_EQ((long)below, 0L,
          "PACED: NO sample landed in a bucket below 5 ms -- which min/max"
          " alone would not catch if the BUCKETING were wrong");
}

static int
xc_run_measure(const char *role)
{
    UINT free_now = 0u, k;

    if (role[1] == 'A' || role[1] == 'B') {
        UINT mine   = (role[1] == 'A') ? XC_FLAG_A : XC_FLAG_B;
        UINT theirs = (role[1] == 'A') ? XC_FLAG_B : XC_FLAG_A;

        if (xc_barrier(mine, theirs) != 0) {
            wtof("TSTRQXC: %s BARRIER FAILED -- measurement skipped", role);
            return -1;
        }
        xc_measure((role[1] == 'A') ? XC_ID_A : XC_ID_B, role);
        CHECK_EQ((long)xc_slot_cas(mine, NSFV_REQ_HELD, NSFV_REQ_FREE),
                 (long)NSFV_RC_OK, "the flag slot was lowered again");
    } else {
        /* ONE flag drives both the arming and the assertion.  The first
         * version tested role[1] here -- 'S' in "MSP" -- so the pace never
         * armed, xc_assert_paced never ran, and the job reported 8/8 having
         * tested nothing (CLAUDE.md 8.5, in the self-test itself). */
        int paced = (role[2] == 'P');

        if (paced) g_pace = XC_PACE_CS;
        xc_measure(XC_ID_SOLO, role);
        if (paced) xc_assert_paced();
    }

    /* REPORTED, not asserted: with two clients the other one is legitimately
     * still running, so "the pool is clean" belongs to whoever leaves last --
     * and in a timed window neither client is reliably last. */
    for (k = 0u; k < NSFV_NSLOTS; k++) {
        if (g_anchor->slots[k].req_state == NSFV_REQ_FREE) free_now++;
    }
    printf("\n  slots FREE as %s leaves: %u of %u\n", role,
           (unsigned)free_now, (unsigned)NSFV_NSLOTS);
    return 0;
}

/* --------------------------------------------------------------------------
 * LEAK -- the retain-branch induction.  See "THE INDUCTION" in the header.
 * -------------------------------------------------------------------------- */
static int
t_read_probe(void)
{
    volatile unsigned char *p = (volatile unsigned char *)g_probe;
    unsigned char           v;

    v = *p;
    return (int)v & 0;
}

static int
t_bad_ubuf(void)
{
    xc_req_init(&g_req, NSFV_REQ_RQE);
    g_req.ubuf   = (void *)g_probe;
    g_req.ulen   = 64u;
    g_req.rqeimg = (void *)&g_image;
    g_req.slot   = 0xFFFFu;
    xc_svc(&g_req);
    return 0;                           /* not reached: the routine faults    */
}

static int
xc_run_leak(void)
{
    UINT st0, in0, st1, in1;
    int  rc, found = 0;
    unsigned addr;

    printf("\n--- LEAK: induce the state nsfsx_stop's RETAIN branch is for ---\n");
    (void)xc_query(0u, &st0, &in0, NULL);
    printf("  before: slot0 state=%u inflight=%u\n",
           (unsigned)st0, (unsigned)in0);

    addr = ((unsigned)(void *)g_own + XC_STEP) & ~(XC_STEP - 1u);
    for (; addr < XC_CEIL; addr += XC_STEP) {
        g_probe = addr;
        if (___try(t_read_probe) != 0) { found = 1; break; }
    }
    CHECK(found != 0, "found an address this client cannot read");
    if (!found) return -1;
    printf("  probe address = %08X\n", g_probe);

    xc_image_fill(XC_ID_SOLO, 0u);
    rc = ___try(t_bad_ubuf);
    printf("  faulting request: try rc=%08X, slot claimed = %u\n",
           (unsigned)rc, (unsigned)g_req.slot);
    CHECK(rc != 0, "the request faulted inside the routine's write-IN move");
    CHECK(rc >= 0, "the fault was CAUGHT -- no dump, this client is alive");

    (void)xc_query(g_req.slot < NSFV_NSLOTS ? g_req.slot : 0u,
                   &st1, &in1, NULL);
    printf("  after:  slot%u state=%u inflight=%u\n",
           (unsigned)g_req.slot, (unsigned)st1, (unsigned)in1);
    wtof("TSTRQXC: LEAK slot=%u state=%u inflight %u->%u anchor=%08X",
         (unsigned)g_req.slot, (unsigned)st1, (unsigned)in0, (unsigned)in1,
         (unsigned)g_anchor);

    /* Both halves matter and neither implies the other: a CLAIMED slot with no
     * waiter is what makes the nudge useless, and a non-zero in-flight count is
     * what makes the drain time out.  Together they are the retain branch. */
    CHECK_EQ((long)st1, (long)NSFV_REQ_CLAIMED,
             "the slot is left CLAIMED -- nobody is waiting on its reply ECB,"
             " so the shutdown nudge has nothing to wake");
    CHECK(in1 > in0,
          "the in-flight count is LEAKED -- so the shutdown drain cannot reach"
          " zero and must decide between retaining CSA and freeing it under a"
          " client");

    printf("\n  *** THE CLEANUP IS DELIBERATELY WITHHELD.  Now:\n");
    printf("  ***   P NSFS   -- expect NSF054W ... CSA AND SVC ROUTINE"
           " RETAINED\n");
    printf("  ***   then re-run this program with PARM='V%08X' to read the\n",
           (unsigned)g_anchor);
    printf("  ***   retained anchor back, and S NSFS to recycle.\n");
    printf("  *** ANCHOR=%08X\n", (unsigned)g_anchor);
    return 0;
}

/* --------------------------------------------------------------------------
 * V -- the positive check after the retained stop.
 *
 * "NSF054W was issued" says the STC took the retain decision.  This says the
 * STORAGE IS STILL THERE, which is the decision's actual content.  It is
 * FORWARD evidence: the fix it supports (not unloading the SVC routine while a
 * client is parked inside it) is NOT proven by reverting it, because reverting
 * means freeing CSA a task is executing in -- see the report.
 * -------------------------------------------------------------------------- */
static int t_read_anchor(void)
{
    const volatile NSFV_ANCHOR *a = (const volatile NSFV_ANCHOR *)g_anchor;

    memcpy(g_eye, (const void *)a->eye, sizeof g_eye);
    g_vsnap[0] = a->version;
    g_vsnap[1] = a->flags;
    g_vsnap[2] = a->inflight;
    g_vsnap[3] = a->nslots;
    g_vsnap[4] = a->exhausted;
    g_vsnap[5] = a->collisions;
    return 0;
}

static int
xc_run_verify(unsigned addr)
{
    int rc;

    printf("\n--- V: is the retained anchor still there at %08X? ---\n", addr);
    g_anchor = (NSFV_ANCHOR *)addr;
    memset(g_eye, 0, sizeof g_eye);
    rc = ___try(t_read_anchor);
    printf("  read: try rc=%08X\n", (unsigned)rc);
    CHECK_EQ((long)rc, 0L, "the address is still readable after the stop");
    if (rc != 0) return -1;

    printf("  eye=%.8s ver=%u flags=%08X inflight=%u nslots=%u exh=%u coll=%u\n",
           g_eye, (unsigned)g_vsnap[0], (unsigned)g_vsnap[1],
           (unsigned)g_vsnap[2], (unsigned)g_vsnap[3], (unsigned)g_vsnap[4],
           (unsigned)g_vsnap[5]);
    wtof("TSTRQXC: V eye=%.8s flags=%08X inflight=%u",
         g_eye, (unsigned)g_vsnap[1], (unsigned)g_vsnap[2]);

    /* The eyecatcher is the discriminator.  nsfsx_anchor_free ZEROES it before
     * the freemain precisely so a client parked in the routine cannot accept
     * reused storage -- so an intact eyecatcher after a stop says the free path
     * was NOT taken. */
    CHECK(memcmp(g_eye, "NSFVANCR", 8) == 0,
          "RETAINED: the anchor eyecatcher is INTACT -- nsfsx_anchor_free"
          " zeroes it before freeing, so the free path was not taken");
    CHECK_EQ((long)(g_vsnap[1] & NSFV_ANCHOR_ACTIVE), 0L,
             "and ACTIVE is clear -- the STC did quiesce, it just did not free");
    CHECK(g_vsnap[2] != 0u,
          "and the leaked in-flight count is still recorded -- the reason the"
          " drain could not finish");
    return 0;
}

/* --------------------------------------------------------------------------
 * RESET -- recovery aid.  NOT part of any gate.
 * -------------------------------------------------------------------------- */
static int
xc_run_reset(void)
{
    UINT k, n = 0;

    printf("\n--- RESET: releasing every CLAIMED/HELD slot ---\n");
    printf("  RUN THIS ONLY WITH NOTHING ELSE USING THE TRANSPORT.\n");
    for (k = 0; k < NSFV_NSLOTS; k++) {
        UINT st = g_anchor->slots[k].req_state;

        if (st != NSFV_REQ_CLAIMED && st != NSFV_REQ_HELD) continue;
        if (xc_slot_cas(k, st, NSFV_REQ_FREE) == NSFV_RC_OK) {
            printf("  slot %2u: %u -> FREE\n", (unsigned)k, (unsigned)st);
            n++;
        }
    }
    printf("  released %u slot(s)\n", (unsigned)n);
    return 0;
}

/* 'MS' / 'MA' / 'MB' / 'MSP', each optionally followed by a decimal window in
 * seconds.  Returns 1 and fills `kind` (4 bytes) when `role` names a valid
 * measurement role, 0 when it is not one at all, and -1 when it LOOKS like one
 * but does not parse -- which must not fall through to SOLO, because a run
 * that silently measured nothing reads exactly like one that did (8.5).
 * A trailing decimal stamps the run as a TRIAL and never touches
 * XC_DISCARD_S: a discard chosen to fit the window is what 5 forbids. */
static int
xc_parse_measure(const char *role, char *kind)
{
    unsigned i, n = 0u, digits = 0u;

    if (role[0] != 'M') return 0;
    if (role[1] != 'S' && role[1] != 'A' && role[1] != 'B') return 0;
    kind[0] = 'M'; kind[1] = role[1]; kind[2] = '\0'; kind[3] = '\0';
    i = 2u;
    if (role[1] == 'S' && role[2] == 'P') { kind[2] = 'P'; i = 3u; }
    for (; role[i] != '\0'; i++) {
        if (role[i] < '0' || role[i] > '9') return -1;
        n = n * 10u + (unsigned)(role[i] - '0');
        digits++;
    }
    if (digits > 0u) {
        if (n < 5u) n = 5u;             /* a window shorter than this measures
                                         * the barrier, not the transport     */
        g_win_s = n;
        g_trial = 1;
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
static int
xc_finish(int skipped)
{
    int rc = mbt_test_summary("TSTRQXC");

    if (skipped) {
        printf("*** THE GATE DID NOT RUN.  This run contains no evidence about"
               " contention.\n");
        printf("*** Returning CC %d rather than %d so it cannot be mistaken"
               " for a clean pass.\n", XC_CC_GATE_SKIPPED, rc);
        wtof("TSTRQXC: GATE SKIPPED -- CC %d, NOT a pass", XC_CC_GATE_SKIPPED);
        return XC_CC_GATE_SKIPPED;
    }
    return rc;
}

int
main(int argc, char **argv)
{
    const char *role = (argc > 1 && argv[1] != NULL) ? argv[1] : "";
    char        kind[4];
    int         rc;
    int         meas;

    wtof("TSTRQXC: POOL CONTENTION TESTS START (ROLE '%s')", role);
    printf("=== TSTRQXC -- M5-2b4: the slot pool under contention ===\n");
    printf("role: '%s'\n", role);

    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    /* The V role is given its address, because after the stop the SVC slot is
     * restored and the chase no longer leads anywhere. */
    if (role[0] == 'V' && role[1] != '\0') {
        unsigned addr = 0;
        int      i;

        for (i = 1; role[i] != '\0'; i++) {
            char c = role[i];
            unsigned d;

            if (c >= '0' && c <= '9')      d = (unsigned)(c - '0');
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A') + 10u;
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a') + 10u;
            else break;
            addr = (addr << 4) | d;
        }
        rc = xc_run_verify(addr);
        wtof("TSTRQXC: POOL CONTENTION TESTS DONE (V)");
        return xc_finish(rc != 0);
    }

    if (xc_find_anchor() != 0) {
        printf("  the anchor chase did not complete -- is NSFS started?\n");
        wtof("TSTRQXC: NO ANCHOR -- gate skipped");
        CHECK(0, "an UNAUTHORISED client can chase CVT -> ... -> the anchor");
        wtof("TSTRQXC: POOL CONTENTION TESTS DONE (skipped)");
        return xc_finish(1);
    }
    printf("  anchor = %08X, version %u, %u slots\n", (unsigned)g_anchor,
           (unsigned)g_anchor->version, (unsigned)g_anchor->nslots);
    CHECK_EQ((long)g_anchor->version, (long)NSFV_ANCHOR_VER,
             "the anchor carries the layout version this build was made for");
    CHECK_EQ((long)g_anchor->nslots, (long)NSFV_NSLOTS,
             "and the slot count the scan is bounded by");
    if (g_anchor->version != NSFV_ANCHOR_VER ||
        g_anchor->nslots  != NSFV_NSLOTS) {
        wtof("TSTRQXC: ANCHOR LAYOUT MISMATCH -- gate skipped");
        return xc_finish(1);
    }

    meas = xc_parse_measure(role, kind);
    if (meas < 0) {
        printf("  role '%s' begins with M but is not a measurement role.\n",
               role);
        wtof("TSTRQXC: BAD MEASUREMENT ROLE '%s' -- nothing measured", role);
        CHECK(0, "the measurement role parsed (a malformed one must NOT fall"
                 " through to SOLO and report a clean pass)");
        return xc_finish(1);
    }
    if (meas > 0) {
        rc = xc_run_measure(kind);
        wtof("TSTRQXC: MEASUREMENT DONE (%s)", kind);
        return xc_finish(rc != 0);
    }

    if      (strcmp(role, "A")     == 0) rc = xc_run_a();
    else if (strcmp(role, "B")     == 0) rc = xc_run_b();
    else if (strcmp(role, "LEAK")  == 0) rc = xc_run_leak();
    else if (strcmp(role, "RESET") == 0) rc = xc_run_reset();
    else {
        wtof("TSTRQXC: POOL CONTENTION TESTS DONE (SOLO)");
        return xc_run_solo();
    }

    wtof("TSTRQXC: POOL CONTENTION TESTS DONE (%s)", role);
    return xc_finish(rc != 0);
}
