/*
 * tstxfw.c -- M5-2c0: the XFER write-out key window gate.
 *
 * MVS-only (project.toml host = false): SVC dispatch, CSA, storage keys and a
 * second address space have no host analog.  Host coverage of this area is the
 * NSF_SIZE_ASSERT / NSFV_OFF_ASSERT set in nsfvsvc.h firing at cc370
 * cross-compile.
 *
 * PREREQUISITE: the Stage-0 probe STC must be started -- S NSFV, NOT S NSFS.
 * That is not a preference.  `nsfsx.c` rejects any staged xfunc other than
 * NSFV_REQ_RQE by setting the slot to HELD, and `nsfsx_next_actionable` only
 * ever looks at PENDING slots, so a HELD slot is never revisited: against the
 * production STC an XFER request is never serviced, never posted, and this
 * test would park in WAIT forever.  Only `nsfv.c` moves an XFER slot to DONE,
 * which is what the client's post-WAIT path tests before it reaches XFEROUT.
 *
 * ------------------------------------------------------------------------
 * WHAT IT PROVES
 *
 * M5-2b1 gave the RQE read-out (RQEOUT) a narrow SPKA window keyed from the
 * caller's TCBPKF, so a caller-supplied DESTINATION is checked by the hardware
 * against the key that owns it instead of being stored under the routine's own
 * PSW key 0.  It deliberately left the XFER read-out (XFEROUT) alone, on the
 * recorded assumption that M5-2c would delete the verb.  That assumption does
 * not hold -- deleting XFER retires TSTUBUF, the only gate that proves the
 * keyed ubuf bounce -- so M5-2c0 routes XFEROUT's move through the same
 * MOVEOUT window, and this file is the gate on that change.
 *
 * TSTUBUF does NOT discriminate: it is green on unmodified code too, because
 * its buffer is the client's own key-8 storage and a key-0 store into key-8
 * storage succeeds.  The discriminating case is the one class of pointer that
 * gets PAST the routine's key-8 read-in and that the old key-0 write-out then
 * clobbered SILENTLY: KEY-0, NON-FETCH-PROTECTED storage.  MVCK's source-key
 * check permits a key-8 READ of it (fetch protection is off), while a key-8
 * STORE into it is denied.
 *
 * ------------------------------------------------------------------------
 * TWO PARTS, AND NEITHER ALONE PINS THE WINDOW.
 *
 *   (1) THE POSITIVE CONTROL -- an ordinary XFER into this client's OWN key-8
 *       buffer, run FIRST.  Pattern in, every byte +1 out, guard byte after
 *       ulen untouched.  It exists because the gate below is satisfied by a
 *       window that faults on EVERYTHING: a misread TCBPKF borrowing some
 *       wrong non-zero key would fault the gate and pass it while breaking
 *       every real client.  The control fails in exactly that case.
 *
 *   (2) THE GATE -- the same verb with `ubuf` in key-0, non-fetch-protected
 *       storage.  Under the window the write-out must take a PROTECTION
 *       EXCEPTION.  Remove the window and the store lands and the request
 *       returns rc=0.
 *
 * The pair discriminates in both directions from one run's output:
 *
 *   borrowed key = the caller's (correct)  control PASSES, gate FAULTS
 *   borrowed key = 0 (window a no-op)      control PASSES, gate STORES  -> FAIL
 *   borrowed key = some other key          control FAILS               -> FAIL
 *
 * ------------------------------------------------------------------------
 * HOW THE GATE IS SAFE, which is the whole reason it can exist.
 *
 * The obvious construction -- point `ubuf` at some piece of system storage --
 * is the one thing that must never be done: if the window ever failed to take,
 * the resulting key-0 store would corrupt it.  So `ubuf` points INTO THE
 * ANCHOR'S OWN STAGING BUFFER at a non-zero offset.  The only storage at risk
 * is NSF's own scratch, which the next request overwrites anyway, and both
 * halves of the move then lie inside staging.
 *
 * SELF-VALIDATING, AND IT HAS TO BE.  Three independent facts are established
 * before the request is issued, and no two of them together would do:
 *
 *   1. the anchor eyecatcher reads "NSFVANCR" -- the chase found NSF's anchor
 *      and not an arbitrary address;
 *   2. a key-8 READ of the target SUCCEEDS -- the page is mapped and not
 *      fetch-protected (any readable address satisfies this alone);
 *   3. a key-8 STORE into the target FAULTS -- it is store-protected against
 *      this client's key (any unwritable address satisfies this alone).
 *
 * Together they pin the target as key-0 storage with fetch protection off,
 * which is what makes the S0C4 the request then takes a KEY fault and not the
 * fault of a bad address.  If any of the three cannot be established the gate
 * SKIPS, and a skip has its own return code -- see below.
 *
 * ------------------------------------------------------------------------
 * A FAULT ALONE IS NOT EVIDENCE -- the request must also be shown unfinished.
 *
 * "The request took an S0C4" says a fault happened, not WHERE.  That gap is
 * not hypothetical: M5-2b2's first attempt broke the routine so that it
 * faulted just after the POST, in code with nothing to do with the write-out,
 * and every gate in the set went green over it.  So the sentinel is asserted
 * alongside the fault: `NSFV_REQ.rc` is pre-set to a value that is neither 0
 * nor any NSFV_RC_*, and REPLYC -- like every bail path in the routine --
 * writes rc.  Untouched => control never reached REPLYC or any bail, so it
 * died in between, which is where XFEROUT lives.
 *
 * NOT `req_state == DONE`: the STC sets DONE asynchronously, so DONE is
 * reached even when the client dies the instant after the POST.  It is
 * evidence the STC ran, never evidence the client got back.
 *
 * THE CONTENT OBSERVATION IS CORROBORATION, NOT THE VERDICT.  Unlike the RQE
 * case in tstrqxf.c -- where the in-move and out-move are exact reverses and a
 * content check is therefore vacuous -- the XFER transform applies a byte-wise
 * +1 to the staged bytes, so the round trip is NOT idempotent: under the
 * window the target reads back unchanged, without it +1.  It is printed and
 * never asserted, because whether a protection exception on MVC suppresses or
 * terminates is not pinned on this target, so a PARTIAL store is not excluded.
 *
 * ------------------------------------------------------------------------
 * IT CLEANS UP AFTER ITSELF, which tstrqxf.c (B) cannot.
 *
 * The fault happens in XFEROUT, after the STC replied: the slot is left at
 * DONE with the in-flight count taken, and nothing releases it.  Left alone,
 * every run of this test would cost a slot and force `P NSFV` to retain the
 * whole CSA anchor until IPL.  CLAIMOK stores the claimed index into the
 * caller's `req.slot` -- a key-0 store into this key-8 block, so it lands long
 * before the fault -- so the test knows WHICH slot to release, issues UNSTAGE
 * on it (which reaches a PUBLISHED slot and gives the count back), and then
 * ASSERTS the slot reads FREE and the in-flight count is back where it was.
 * Asserted, not printed: a cleanup nobody checks is a cleanup that silently
 * stops working.
 *
 * ------------------------------------------------------------------------
 * THE RETURN CODE HAS THREE STATES, NOT TWO (the TSTRQXF contract).
 *
 *    0                     everything ran and everything passed
 *    1                     the gate RAN and something FAILED
 *    XW_CC_GATE_SKIPPED    the gate COULD NOT RUN
 *
 * The gate skips when the anchor cannot be found or validated, and skipping is
 * the right behaviour -- asserting against an address that was never validated
 * would be worse than not asserting.  But a skip reporting CC 0 would make a
 * run containing NO evidence indistinguishable from a run that proved the
 * window works, and "TSTXFW CC 0" would later be read as "the window was
 * exercised".  So a skip carries its own code, and takes precedence over the
 * failure code: of the two facts, "the central claim went unproven" is the one
 * that invalidates the whole run.
 *
 * THE RED LINE IS UNCHANGED: the client is UNAUTHORISED (it asserts
 * __isauth() == 0) and never self-authorises.
 */
#include "nsfvsvc.h"
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                      */
#include <clibtry.h>        /* ___try -- capture the abend, no dump            */
#include <clibwto.h>        /* wtof -- console markers survive a hang          */
#include <cvt.h>            /* CVTPTR -- the chase starts at absolute 16       */
#include <ihascvt.h>        /* SCVT (scvtsvct), SVCTABLE, SVCENTRY             */
#include <mbtcheck.h>
#include <string.h>

/* The gate's target: an offset into the anchor's staging buffer, and the
 * number of bytes moved.  Non-zero and well clear of both ends of the 2048-byte
 * area, so the in-move (stage+OFF -> stage+0) and the out-move (stage+0 ->
 * stage+OFF) touch disjoint halves. */
#define XW_STAGE_OFF   1024u
#define XW_STAGE_LEN   64u

/* The positive control's size.  Deliberately not a multiple of the 256-byte
 * MOVEOUT piece: the last piece is then short, so the EX length-1 arithmetic is
 * exercised on a partial piece as well as on whole ones. */
#define XW_CTL_LEN     300u

/* A byte value for the store-protection pre-check.  It is never expected to
 * land; if the store ever succeeded the target would be NSF's own staging
 * scratch, overwritten by the next request. */
#define XW_POISON      0x5Au

/* The control's guard byte, immediately after ulen.  +1 would make it 0x7F, so
 * an overrun past ulen is visible rather than plausible. */
#define XW_GUARD       0x7Eu

/* The completion sentinel.  Not 0 and not any NSFV_RC_* (0/4/8/12/16), so it
 * cannot be confused with a real router return code. */
#define XW_RC_SENT     0x5AC0F001

/* "The load-bearing case could not run".  Deliberately NOT 1: mbt_test_summary
 * returns 0/1, so 1 already means "ran and failed".  Returned from the test
 * itself; the mbt harness is not modified. */
#define XW_CC_GATE_SKIPPED 20

static unsigned char g_ctl[XW_CTL_LEN + 1u];   /* control buffer + guard byte */

static NSFV_REQ      g_greq;            /* the gate's request block -- FILE    */
                                        /* SCOPE so main can read it AFTER the */
                                        /* routine faults inside ___try        */

static NSFV_ANCHOR  *g_anchor;          /* discovered, never given             */
static volatile unsigned char *g_stagep;/* &anchor->slots[0].stage[OFF]        */
static unsigned char g_stage_byte;      /* the byte the read pre-check saw     */
static int           g_gate_rc;         /* set ONLY if the request returned    */

static unsigned char g_before[XW_STAGE_LEN];   /* target before the request   */
static unsigned char g_after[XW_STAGE_LEN];    /* target after the request    */

/* Issue the private SVC with R1 = A(req) via the EX-SVC-0 trick (ADR-0038 6).
 * EX ORs its low byte into a stored "SVC 0", so no storage is modified and the
 * sequence is RENT-safe.  noinline: the named asm labels appear once, and they
 * are per-file so they stay unique in the load module. */
static void __attribute__((noinline))
nsfv_svc_issue(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"
        "         LR    6,%1\n"
        "         EX    6,NSFXW0\n"
        "         B     NSFXWX\n"
        "NSFXW0   SVC   0\n"
        "NSFXWX   DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

static void
xw_req_init(NSFV_REQ *req, UINT func)
{
    memset(req, 0, sizeof *req);
    memcpy(req->eye, NSFV_REQ_EYE, 4);
    req->func = func;
    req->rc   = -1;
}

static int
xw_query_slot(UINT idx, UINT *state, UINT *infl, UINT *reap)
{
    NSFV_REQ req;

    xw_req_init(&req, NSFV_REQ_QUERY);
    req.slot = idx;
    nsfv_svc_issue(&req);
    if (state) *state = req.qstate;
    if (infl)  *infl  = req.qinfl;
    if (reap)  *reap  = req.qreap;
    return req.rc;
}

static int
xw_unstage_slot(UINT idx)
{
    NSFV_REQ req;

    xw_req_init(&req, NSFV_REQ_UNSTAGE);
    req.slot = idx;
    nsfv_svc_issue(&req);
    return req.rc;
}

/* The control's pattern.  Asymmetric and position-dependent, so a mis-chunked
 * or offset copy is caught rather than aliased; pat(i)+1 is never pat(i), so a
 * routine that left the buffer untouched would fail the +1 check. */
static unsigned char
pat(unsigned i)
{
    return (unsigned char)(i * 5u + (i >> 4) + 0x11u);
}

/* --- bodies run under ___try -------------------------------------------- */

static void *g_hop_in;
static void *g_hop_out;
static char  g_eye[8];

static int t_hop_cvt(void)
{
    g_hop_out = (void *)CVTPTR;                     /* the read of location 16 */
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

/* Run one hop, report it either way, and say WHICH hop faulted -- "the chase
 * faulted" without knowing which hop is a useless datum. */
static void *
xw_hop(const char *name, int (*body)(void), void *in, int *failed)
{
    int rc;

    if (*failed) return NULL;
    g_hop_in  = in;
    g_hop_out = NULL;
    rc = ___try(body);
    if (rc != 0) {
        printf("  chase %-6s FAULTED (try rc=%08X)\n", name, (unsigned)rc);
        wtof("TSTXFW: chase %s FAULTED rc=%08X", name, (unsigned)rc);
        *failed = 1;
        return NULL;
    }
    printf("  chase %-6s -> %08X\n", name, (unsigned)g_hop_out);
    return g_hop_out;
}

/* Pre-check 1: the target must be READABLE from the client's own key --
 * otherwise the routine's key-8 read-in would fault and the run would be
 * measuring the pre-existing IN-direction exposure instead. */
static int
t_stage_read(void)
{
    g_stage_byte = g_stagep[0];
    return 0;
}

/* Pre-check 2: the target must NOT be STORABLE from the client's own key.
 * The load-bearing half: it is what makes the fault the request later takes a
 * KEY fault rather than the fault of a bad address. */
static int
t_stage_write(void)
{
    g_stagep[0] = (unsigned char)XW_POISON;
    return 0;
}

static int
t_snap_before(void)
{
    UINT i;
    for (i = 0u; i < XW_STAGE_LEN; i++) g_before[i] = g_stagep[i];
    return 0;
}

static int
t_snap_after(void)
{
    UINT i;
    for (i = 0u; i < XW_STAGE_LEN; i++) g_after[i] = g_stagep[i];
    return 0;
}

/* THE GATE: an XFER whose ubuf is in key-0, non-fetch-protected storage.  The
 * read-in succeeds (key-8 fetch is permitted), the STC services the request,
 * and the write-out must fault under the M5-2c0 window. */
static int
t_gate_xfer(void)
{
    nsfv_svc_issue(&g_greq);            /* set up by main, so it OUTLIVES this */
    g_gate_rc = g_greq.rc;              /* reached ONLY without the window     */
    return 0;
}

/* The ONE exit.  Prints the summary as usual, then decides the code. */
static int
xw_finish(int skipped)
{
    int rc = mbt_test_summary("TSTXFW");

    if (skipped) {
        printf("*** THE GATE DID NOT RUN.  This run contains NO evidence about"
               " the XFER\n");
        printf("*** write-out key window.  Returning CC %d rather than %d so it"
               " cannot be\n", XW_CC_GATE_SKIPPED, rc);
        printf("*** mistaken for a clean pass -- see the header.\n");
        wtof("TSTXFW: GATE SKIPPED -- CC %d, NOT a pass", XW_CC_GATE_SKIPPED);
        return XW_CC_GATE_SKIPPED;
    }
    return rc;
}

int
main(void)
{
    UINT     st0, in0, rp0, st1, in1, rp1;
    UINT     claimed;
    UINT     i;
    int      rc, failed, dataok, changed;
    void    *cvt, *scvt, *svct, *epa, *anch;

    wtof("TSTXFW: XFER WRITE-OUT KEY WINDOW GATE START (SVC %u)",
         (unsigned)NSFV_SVCNUM);
    printf("=== TSTXFW -- M5-2c0: the XFER write-out key window ===\n");

    CHECK_EQ((long)__isauth(), 0L,
             "the client is UNAUTHORISED (TESTAUTH FCTN=1 == 0)");

    /* ---- baseline: slot 0 and the in-flight count ----------------------- */
    rc = xw_query_slot(0u, &st0, &in0, &rp0);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers (the probe STC is up)");
    printf("  before: slot0 state=%u inflight=%u reaped=%u\n",
           (unsigned)st0, (unsigned)in0, (unsigned)rp0);

    /* ==================================================================== *
     * (1) THE POSITIVE CONTROL -- an ordinary XFER into this client's OWN
     *     key-8 buffer.  It must run FIRST: after the gate the claimed slot
     *     sits at DONE until UNSTAGE releases it.
     * ==================================================================== */
    printf("\n--- (1) positive control: XFER into the client's own key-8"
           " buffer ---\n");
    {
        NSFV_REQ req;

        for (i = 0u; i < XW_CTL_LEN; i++) g_ctl[i] = pat(i);
        g_ctl[XW_CTL_LEN] = (unsigned char)XW_GUARD;

        xw_req_init(&req, NSFV_REQ_XFER);
        req.ubuf = g_ctl;
        req.ulen = XW_CTL_LEN;
        nsfv_svc_issue(&req);

        printf("  control XFER of %u bytes: rc=%d slot=%u\n",
               (unsigned)XW_CTL_LEN, req.rc, (unsigned)req.slot);
        CHECK_EQ((long)req.rc, (long)NSFV_RC_OK,
                 "CONTROL (1/3): an ordinary XFER completes rc=OK");

        dataok = 1;
        for (i = 0u; i < XW_CTL_LEN; i++) {
            if (g_ctl[i] != (unsigned char)(pat(i) + 1u)) { dataok = 0; break; }
        }
        if (!dataok) {
            printf("  *** byte %u reads %02X, expected %02X\n", (unsigned)i,
                   (unsigned)g_ctl[i], (unsigned)(unsigned char)(pat(i) + 1u));
        }
        /* THE CONTROL'S POINT: the window borrows the CALLER's key, so a
        ** destination in the caller's own key-8 storage still moves.  A window
        ** that borrowed some wrong non-zero key would fault the gate below AND
        ** fail here -- which is the only way to tell the two apart. */
        CHECK(dataok,
              "CONTROL (2/3): every byte +1 byte-exact -- the window borrows"
              " the CALLER's key, so the caller's own storage still moves");
        CHECK_EQ((long)(unsigned)g_ctl[XW_CTL_LEN], (long)XW_GUARD,
                 "CONTROL (3/3): the guard byte after ulen is UNTOUCHED (the"
                 " EX length-1 arithmetic does not overrun)");
    }

    /* ==================================================================== *
     * (2) THE GATE.  Discover the anchor first -- an unauthorised client is
     *     not given the address, so it is chased the way the routine itself
     *     does, READ-ONLY and hop by hop under ___try.
     * ==================================================================== */
    printf("\n--- (2) the gate: a key-0 non-fetch-protected ubuf ---\n");

    failed = 0;
    cvt  = xw_hop("CVT",   t_hop_cvt,  NULL, &failed);
    scvt = xw_hop("SCVT",  t_hop_scvt, cvt,  &failed);
    svct = xw_hop("SVCT",  t_hop_svct, scvt, &failed);
    epa  = xw_hop("EPA",   t_hop_epa,  svct, &failed);
    if (!failed) {
        /* 24-bit machine: mask the SVC-table entry-point word before using it
         * as an address, so a flag byte can never become part of it. */
        epa = (void *)((unsigned)epa & 0x00FFFFFFu);
        printf("  chase EP     -> %08X (SVC %u routine, in CSA)\n",
               (unsigned)epa, (unsigned)NSFV_SVCNUM);
    }
    anch = xw_hop("ANCHOR", t_hop_anch, epa, &failed);

    /* One assertion over both ways this goes wrong: a faulting hop, and a chase
     * that completed every hop but read back a NULL anchor word. */
    CHECK(failed == 0 && anch != NULL,
          "an UNAUTHORISED client can chase CVT -> SCVT -> SVCTABLE -> the"
          " routine EP -> the anchor word");
    if (failed || anch == NULL) {
        printf("  GATE SKIPPED: the anchor chase did not complete\n");
        wtof("TSTXFW: GATE SKIPPED -- chase incomplete");
        return xw_finish(1);
    }

    /* Fact 1: this really is NSF's anchor and not an arbitrary address. */
    memset(g_eye, 0, sizeof g_eye);
    failed = 0;
    (void)xw_hop("EYE", t_hop_eye, anch, &failed);
    CHECK(failed == 0, "the anchor's eyecatcher is readable from key 8");
    CHECK(memcmp(g_eye, "NSFVANCR", 8) == 0,
          "FACT 1: the chase found NSF's anchor (eyecatcher \"NSFVANCR\")");
    if (failed || memcmp(g_eye, "NSFVANCR", 8) != 0) {
        printf("  GATE SKIPPED: %08X does not carry the anchor eyecatcher\n",
               (unsigned)anch);
        wtof("TSTXFW: GATE SKIPPED -- no anchor eyecatcher at %08X",
             (unsigned)anch);
        return xw_finish(1);
    }

    g_anchor = (NSFV_ANCHOR *)anch;
    g_stagep = (volatile unsigned char *)&g_anchor->slots[0].stage[XW_STAGE_OFF];
    printf("  anchor = %08X, target = &slots[0].stage[%u] = %08X, len = %u\n",
           (unsigned)g_anchor, (unsigned)XW_STAGE_OFF, (unsigned)g_stagep,
           (unsigned)XW_STAGE_LEN);
    wtof("TSTXFW: anchor=%08X target=%08X", (unsigned)g_anchor,
         (unsigned)g_stagep);

    /* Fact 2: readable from our own key (mapped, fetch protection off).  Any
     * readable address satisfies this ALONE -- fact 3 is what makes the pair
     * mean "key 0". */
    rc = ___try(t_stage_read);
    printf("  key-8 READ of the target: try rc=%08X (byte %02X)\n",
           (unsigned)rc, (unsigned)g_stage_byte);
    CHECK_EQ((long)rc, 0L,
             "FACT 2: a key-8 READ of the target SUCCEEDS (mapped, not"
             " fetch-protected)");
    if (rc != 0) {
        printf("  GATE SKIPPED: the target is not readable from key 8\n");
        wtof("TSTXFW: GATE SKIPPED -- target not readable");
        return xw_finish(1);
    }

    /* Fact 3: NOT storable from our own key.  The load-bearing half -- without
     * it, the fault below could be the fault of a bad address. */
    rc = ___try(t_stage_write);
    printf("  key-8 STORE into the target: try rc=%08X\n", (unsigned)rc);
    CHECK(rc != 0,
          "FACT 3: a key-8 STORE into the target FAULTS (it is key-0 storage)");
    if (rc > 0) {
        printf("  the store fault is S%03X\n", ((unsigned)rc >> 12) & 0xFFFu);
    }
    if (rc == 0) {
        printf("  GATE SKIPPED: the target is NOT key-0 storage -- a fault from"
               " the request\n");
        printf("  would not be attributable to the key.\n");
        wtof("TSTXFW: GATE SKIPPED -- target is storable from key 8");
        return xw_finish(1);
    }

    /* Snapshot the target, so the (corroborating, non-load-bearing) content
     * observation below has a baseline to compare against. */
    failed = 0;
    (void)xw_hop("SNAP", t_snap_before, NULL, &failed);
    CHECK(failed == 0, "the target's current contents are readable from key 8");

    /* --- the case itself ---
     * The read-in MUST succeed (key-8 fetch of fetch-unprotected storage), the
     * STC services the request (+1 over the staged bytes), and the write-out
     * must fault under the window.  WITHOUT the window the store lands and the
     * request returns rc=0. */
    xw_req_init(&g_greq, NSFV_REQ_XFER);
    g_greq.ubuf = (void *)g_stagep;
    g_greq.ulen = XW_STAGE_LEN;
    g_greq.slot = 0xFFFFu;              /* so an unwritten field is obvious   */
    g_greq.rc   = XW_RC_SENT;           /* REPLYC and every bail overwrite it */

    g_gate_rc = 0x7FFFFFFF;
    rc = ___try(t_gate_xfer);
    printf("  key-0-ubuf XFER: try rc=%08X\n", (unsigned)rc);
    wtof("TSTXFW: gate request rc=%08X", (unsigned)rc);
    if (rc == 0) {
        printf("  *** NO FAULT: the request RETURNED, req.rc=%d -- the"
               " write-out stored\n", g_gate_rc);
        printf("  *** into key-0 storage under the routine's own key.  That is"
               " exactly the\n");
        printf("  *** state M5-2c0 removes.\n");
    } else {
        printf("  the fault is S%03X\n", ((unsigned)rc >> 12) & 0xFFFu);
    }
    CHECK(rc != 0,
          "THE GATE (1/2): the XFER key-0 write-out FAULTS -- the M5-2c0 SPKA"
          " window checks the caller-supplied destination against the caller's"
          " key");
    CHECK(rc >= 0,
          "the fault was CAUGHT (ESTAE created) -- no dump, client alive");

    /* --- the fault was in the WRITE-OUT, not merely somewhere ------------ */
    printf("  rc sentinel: %08X (pre-set %08X)\n",
           (unsigned)g_greq.rc, (unsigned)XW_RC_SENT);
    wtof("TSTXFW: gate rc sentinel=%08X", (unsigned)g_greq.rc);
    CHECK_EQ((long)g_greq.rc, (long)XW_RC_SENT,
             "THE GATE (2/2): rc is untouched -- the routine never reached"
             " REPLYC or any bail path, so it died in between");

    /* --- corroboration, deliberately NOT asserted ------------------------
     * The XFER transform is +1, so the round trip is not idempotent: under the
     * window the target reads back unchanged, without it +1.  Printed only:
     * whether a protection exception on MVC suppresses or terminates is not
     * pinned on this target, so a PARTIAL store is not excluded. */
    failed = 0;
    (void)xw_hop("SNAP2", t_snap_after, NULL, &failed);
    if (failed == 0) {
        changed = 0;
        for (i = 0u; i < XW_STAGE_LEN; i++) {
            if (g_after[i] != g_before[i]) { changed++; }
        }
        printf("  CORROBORATION (not asserted): %d of %u target bytes changed"
               " -- %s\n", changed, (unsigned)XW_STAGE_LEN,
               (changed == 0) ? "unchanged, consistent with the window taking"
                              : "MODIFIED, consistent with the store landing");
        wtof("TSTXFW: target bytes changed=%d of %u", changed,
             (unsigned)XW_STAGE_LEN);
    }

    /* --- what the OUT direction left behind, and the cleanup -------------
     * The fault is inside XFEROUT, after the STC serviced and replied, so the
     * slot is stuck at DONE with the in-flight count taken.  REPORTED, never
     * asserted: recovery from a write-out fault is still the open M5-2 item
     * ADR-0039 names.  What IS asserted is that the test gives it back. */
    claimed = g_greq.slot;
    printf("  the routine claimed slot %u (CLAIMOK stored it before the"
           " fault)\n", (unsigned)claimed);
    CHECK(claimed < NSFV_NSLOTS,
          "the claimed slot index was reported back and is in range");
    if (claimed >= NSFV_NSLOTS) claimed = 0u;   /* keep the cleanup bounded   */

    rc = xw_query_slot(claimed, &st1, &in1, &rp1);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK,
             "the transport still answers after the fault (not wedged)");
    printf("  after the fault: slot%u state=%u inflight=%u reaped=%u\n",
           (unsigned)claimed, (unsigned)st1, (unsigned)in1, (unsigned)rp1);
    printf("  FINDING: slot %s (predicted DONE=%u), inflight %s\n",
           (st1 == NSFV_REQ_DONE) ? "DONE (published, never released)"
                                  : "not DONE",
           (unsigned)NSFV_REQ_DONE, (in1 > in0) ? "LEAKED" : "clean");
    wtof("TSTXFW: after fault slot=%u state=%u inflight=%u",
         (unsigned)claimed, (unsigned)st1, (unsigned)in1);

    rc = xw_unstage_slot(claimed);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "UNSTAGE accepted for the claimed slot");
    rc = xw_query_slot(claimed, &st1, &in1, &rp1);
    CHECK_EQ((long)rc, (long)NSFV_RC_OK, "QUERY answers after UNSTAGE");
    printf("  after UNSTAGE:  slot%u state=%u inflight=%u reaped=%u\n",
           (unsigned)claimed, (unsigned)st1, (unsigned)in1, (unsigned)rp1);
    /* ASSERTED, not printed.  Every run of this test costs a slot and an
    ** in-flight count if the cleanup stops working, and the STC then retains
    ** the whole CSA anchor at P NSFV until IPL. */
    CHECK_EQ((long)st1, (long)NSFV_REQ_FREE,
             "CLEANUP: the claimed slot is FREE again");
    CHECK_EQ((long)in1, (long)in0,
             "CLEANUP: the in-flight count is back to its pre-request value");

    wtof("TSTXFW: XFER WRITE-OUT KEY WINDOW GATE DONE");
    return xw_finish(0);
}
