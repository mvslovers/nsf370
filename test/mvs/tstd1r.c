/*----------------------------------------------------------------------------
 * tstd1r.c -- M5-2d1 live gate 2.4: THE R8 VALIDATION.  MVS-only.
 *
 * d1b put a TPROT probe in front of the SVC routine's 20 key-0 stores into the
 * caller's NSFV_REQ block.  The eyecatcher validates a POINTER, not a KEY, so a
 * client that stamps "NSFV" into key-0 storage used to get 20 key-0 words
 * written into it.  TPROT asks the machine, under the CALLER's key, whether the
 * caller may store there -- by condition code, without faulting.
 *
 * THIS IS ENTIRELY UNPROVEN BEFORE THIS TEST.  asm/*.asm never compiles on
 * host, so nothing offline can substitute for it: the d1b PR said so and this
 * is the gate it named.
 *
 * THREE CASES
 *  (1) NO EYECATCHER  -> refused, and the block NOT WRITTEN (a sentinel in it
 *      survives).  "Refused" and "refused after writing" are different answers.
 *  (2) EYECATCHER AT AN ADDRESS THE CALLER DOES NOT OWN.  The target is the
 *      client's OWN CSA staging buffer, reached by first XFERing a pattern
 *      beginning "NSFV" into it -- so the eyecatcher check PASSES and only the
 *      key check can refuse.
 *
 *      XFER IS NOT A COPY, AND THAT COST THE PREVIOUS ROUND ITS RESULT: the
 *      probe STC adds 1 to every staged byte (src/nsfv.c), so a verbatim
 *      "NSFV" lands as "NSFV"+1, the EYECATCHER check is what refuses, and the
 *      answer says nothing about TPROT.  The image is therefore staged MINUS
 *      ONE, derived from NSFV_REQ_EYE rather than written out as bytes.  Its
 *      self-validation is a GATE, not an assertion: if the eyecatcher did not
 *      land, or the target turns out to be key-8 writable, the case CANNOT RUN
 *      and says so -- and main then returns 20, because cases 1 and 3 passing
 *      while the case the gate exists for never ran must not read as green.  Self-validating (we prove the storage is
 *      key-8-readable and key-8-unwritable first) and SAFE: the only storage at
 *      risk is this client's own scratch, so the revert arm corrupts nothing
 *      that matters.  Pointing R8 at the anchor itself would also pass the
 *      eyecatcher -- "NSFVANCR" starts with "NSFV" -- and was REJECTED for
 *      exactly that reason: with the check reverted it would overwrite NSFS's
 *      own control block.
 *  (3) THE NEVER-REFERENCED TAIL.  A block straddling a page boundary whose
 *      last bytes the client never touches MUST BE ACCEPTED.  This is a
 *      NEGATIVE control on the check -- it exists because rejecting CC 3 would
 *      have been an intermittent false refusal of an honest caller, and
 *      FNECHO-shaped clients touch only offsets 0-7.  It is expected to pass in
 *      BOTH arms; that is its point, not a failure to discriminate.
 *
 * RUNS AGAINST NSFV, NOT NSFS, AND THAT IS FORCED RATHER THAN PREFERRED.
 * Case 2 needs the XFER verb to stage its target, and M5-2c0 established that a
 * non-RQE probe verb dispatched at NSFS reaches ACT_DISPATCH's else arm, goes
 * HELD, and the client PARKS FOREVER -- issue #67's shape. This test walked
 * into exactly that on its first run and had to be cancelled. NSFV services the
 * probe verbs, and the R8 check lives in the SVC routine both STCs share, so
 * testing it there exercises the same instructions.
 *
 * ORDER:  P NSFS -> S NSFV -> run -> P NSFV -> S NSFS   (both steal SVC 239)
 *
 * Return codes: 0 passed, 1 ran and failed, 20 could not run.
 *--------------------------------------------------------------------------*/
#include "nsfvsvc.h"
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                      */
#include <clibtry.h>        /* ___try -- capture the abend, no dump            */
#include <clibwto.h>        /* wtof -- survives a hang, unlike SYSPRINT        */
#include <cvt.h>            /* CVTPTR / cvtabend -- the chase starts at 16     */
#include <ihascvt.h>        /* SCVT (scvtsvct), SVCTABLE, SVCENTRY             */
#include <mbtcheck.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D1R_CC_SKIP  20
#define D1R_SENT     0x5AC0F001u        /* sentinel: "nothing wrote here"     */
#define D1R_PAGE     4096u

static void *g_hop_in, *g_hop_out;
static char  g_eye[8];
static NSFV_ANCHOR *g_anchor;

/* Issue the private SVC with R1 = A(req); labels are per-file so they stay
 * unique in the load module (the tstrqxc pattern, ADR-0038 6). */
static void __attribute__((noinline))
d1r_svc(void *req)
{
    unsigned reqp = (unsigned)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"
        "         LR    6,%1\n"
        "         EX    6,NSFDR0\n"
        "         B     NSFDRX\n"
        "NSFDR0   SVC   0\n"
        "NSFDRX   DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

static int h_cvt(void)  { g_hop_out = (void *)CVTPTR; return 0; }
static int h_scvt(void) { g_hop_out = ((CVT *)g_hop_in)->cvtabend; return 0; }
static int h_svct(void) { g_hop_out = ((SCVT *)g_hop_in)->scvtsvct; return 0; }
static int h_epa(void)
{
    g_hop_out = ((SVCTABLE *)g_hop_in)->svcentry[NSFV_SVCNUM].svcepa;
    return 0;
}
static int h_anch(void)
{
    g_hop_out = *(void **)((unsigned char *)g_hop_in + NSFV_ANCH_OFF);
    return 0;
}
static int h_eye(void)
{
    memcpy(g_eye, ((NSFV_ANCHOR *)g_hop_in)->eye, sizeof g_eye);
    return 0;
}

static void *hop(int (*body)(void), void *in, int *failed)
{
    if (*failed) return NULL;
    g_hop_in = in; g_hop_out = NULL;
    if (___try(body) != 0) { *failed = 1; return NULL; }
    return g_hop_out;
}

/* The unauthorised client chases CVT -> SCVT -> SVCTABLE -> EP -> anchor.  This
 * is the same hop-by-hop chase M5-2b measured working from problem state key 8;
 * it is READ-only and the eyecatcher is the check that it landed. */
static int find_anchor(void)
{
    int   failed = 0;
    void *cvt, *scvt, *svct, *epa, *anch;

    cvt  = hop(h_cvt,  NULL, &failed);
    scvt = hop(h_scvt, cvt,  &failed);
    svct = hop(h_svct, scvt, &failed);
    epa  = hop(h_epa,  svct, &failed);
    if (!failed) epa = (void *)((unsigned)epa & 0x00FFFFFFu);
    anch = hop(h_anch, epa, &failed);
    if (failed || anch == NULL) return -1;
    memset(g_eye, 0, sizeof g_eye);
    (void)hop(h_eye, anch, &failed);
    if (failed || memcmp(g_eye, "NSFVANCR", 8) != 0) return -1;
    g_anchor = (NSFV_ANCHOR *)anch;
    printf("  anchor = %08X (eyecatcher verified)\n", (unsigned)anch);
    return 0;
}

/* ---- case 1: no eyecatcher ------------------------------------------------ */
static void case_no_eye(void)
{
    NSFV_REQ req;

    memset(&req, 0, sizeof req);
    memcpy(req.eye, "XXXX", 4);                 /* deliberately not ours      */
    req.func = NSFV_REQ_QUERY;
    req.rc   = D1R_SENT;                        /* must survive untouched     */
    req.seq  = D1R_SENT;

    d1r_svc(&req);

    CHECK_EQ((long)req.rc, (long)D1R_SENT,
             "2.4(1): no eyecatcher -> the block was NOT written (rc sentinel)");
    CHECK_EQ((long)req.seq, (long)D1R_SENT,
             "2.4(1): ...and neither was seq");
    printf("  case 1: rc=%08X seq=%08X (sentinel %08X)\n",
           (unsigned)req.rc, (unsigned)req.seq, (unsigned)D1R_SENT);
}

static volatile UINT *g_store_probe;

/* A key-8 store into key-0 CSA: ___try turns the S0C4 into a return code. */
static int d1r_try_store(void)
{
    *g_store_probe = 0xDEADBEEFu;
    return 0;
}

/* ---- case 2: eyecatcher present, storage the caller cannot write ---------- */
/* Returns 0 if the case RAN, non-zero if it could not run.  The distinction is
 * the whole repair: the previous round's self-validation failed and a PASS was
 * printed underneath it, so the case reported a result it had not earned. */
static int case_foreign_r8(void)
{
    NSFV_REQ  req;
    NSFV_REQ  intended;         /* what must END UP in the CSA slot           */
    char      staged[64];       /* what we SEND: `intended`, each byte - 1    */
    NSFV_REQ *target;
    UINT      slot, i;
    int       readable = 0, storable = 1;

    /* THE IMAGE THAT MUST LAND: a valid eyecatcher, so the router's CLC accepts
     * the block and the KEY check is the only thing left that can refuse; plus
     * a sentinel in the two words a permitted write would change first. */
    memset(&intended, 0, sizeof intended);
    memcpy(intended.eye, NSFV_REQ_EYE, 4);
    intended.func = NSFV_REQ_QUERY;
    intended.rc   = (int)D1R_SENT;
    intended.seq  = D1R_SENT;

    /* STAGE IT MINUS ONE, BECAUSE THE STC ADDS ONE.  XFER IS NOT A COPY: the
     * probe STC's service applies +1 to every staged byte (src/nsfv.c), so a
     * verbatim "NSFV" lands as "NSFV"+1, the EYECATCHER check refuses, and the
     * result says nothing about TPROT.  That is exactly what happened in the
     * previous round -- and its own self-validation is what caught it.
     *
     * DERIVED FROM THE LITERAL, NEVER WRITTEN OUT AS BYTES.  The runtime is
     * EBCDIC, so the literal is D5 E2 C6 E5; hardcoding its minus-one would
     * re-create the very assumption that cost that round its result.  Char
     * arithmetic wraps byte-wise with no carry, so minus-one is exact for every
     * value, 0x00 included. */
    for (i = 0u; i < (UINT)sizeof staged; i++)
        staged[i] = (char)(((const unsigned char *)&intended)[i] - 1u);

    memset(&req, 0, sizeof req);
    memcpy(req.eye, NSFV_REQ_EYE, 4);
    req.func = NSFV_REQ_XFER;
    req.ubuf = staged;
    req.ulen = (UINT)sizeof staged;
    d1r_svc(&req);
    if (req.rc != NSFV_RC_OK) {
        printf("  case 2: could not stage (rc=%d) -- CASE SKIPPED\n",
               (int)req.rc);
        wtof("TSTD1R: (2) staging failed rc=%d -- CASE SKIPPED", (int)req.rc);
        return 1;
    }
    slot   = req.slot;
    target = (NSFV_REQ *)&g_anchor->slots[slot].stage[0];
    printf("  case 2: staged into slot %u, target = %08X\n",
           (unsigned)slot, (unsigned)target);

    /* THE DIAGNOSTIC IS DERIVED THROUGHOUT, SO IT CANNOT CONTRADICT ITSELF.
     * The previous round printed a "wanted" beside a landed value and the two
     * implied different encodings; that contradiction is what stopped it, and
     * settling it cost a round.  Every row below comes from the same `intended`
     * image this client built, so all three agree under ANY runtime encoding --
     * there is no byte value written out anywhere for a reader to compare
     * against by eye. */
    {
        const unsigned char *w = (const unsigned char *)&intended;
        const unsigned char *g = (const unsigned char *)staged;
        const unsigned char *b = (const unsigned char *)target;

        printf("  case 2: sent   %02X %02X %02X %02X  (= NSFV_REQ_EYE - 1)\n",
               g[0], g[1], g[2], g[3]);
        printf("  case 2: landed %02X %02X %02X %02X  (after the STC's +1)\n",
               b[0], b[1], b[2], b[3]);
        printf("  case 2: wanted %02X %02X %02X %02X  (= NSFV_REQ_EYE)\n",
               w[0], w[1], w[2], w[3]);
    }

    /* SELF-VALIDATION AS A GATE, NOT AS AN ASSERTION.  Both halves must hold
     * before the case means anything: the target must be READABLE from key 8
     * (or the eyecatcher check refuses and TPROT is never reached) and NOT
     * STORABLE from key 8 (or a refusal could equally mean a bad address).  If
     * either fails the case CANNOT RUN, and saying so is the only honest
     * answer -- a PASS printed under a failed precondition is worse than no
     * result, because it reads as one (CLAUDE.md 8.5). */
    readable = (memcmp(target->eye, NSFV_REQ_EYE, 4) == 0);
    if (!readable) {
        printf("  case 2: the eyecatcher did NOT land -- CASE SKIPPED (the"
               " refusal would be the eyecatcher check, not the key check)\n");
        wtof("TSTD1R: (2) eyecatcher did not land -- CASE SKIPPED");
        return 1;
    }

    g_store_probe = (volatile UINT *)&g_anchor->slots[slot].stage[8];
    storable = (___try(d1r_try_store) == 0);
    if (storable) {
        printf("  case 2: the target is key-8 WRITABLE -- CASE SKIPPED (a"
               " refusal could not be attributed to the key check)\n");
        wtof("TSTD1R: (2) target is key-8 writable -- CASE SKIPPED");
        return 1;
    }
    printf("  case 2: readable=%d storable=%d (both as required)\n",
           readable, storable);

    /* THE CASE.  R8 points at storage carrying "NSFV" that this client cannot
     * write.  Refused => the block is untouched; permitted => the router writes
     * 20 key-0 words into it (harmless here: it is our own scratch). */
    {
        UINT before_rc  = target->rc;
        UINT before_seq = target->seq;

        d1r_svc(target);

        printf("  case 2: target rc %08X -> %08X, seq %08X -> %08X\n",
               (unsigned)before_rc, (unsigned)target->rc,
               (unsigned)before_seq, (unsigned)target->seq);
        CHECK(target->rc == before_rc && target->seq == before_seq,
              "2.4(2): a foreign R8 was REFUSED and the block NOT written");
        /* And the refusal is now ATTRIBUTABLE: the eyecatcher provably landed
        ** (gated above), so the eyecatcher check cannot be what refused, and
        ** the address is provably key-0 (the store faults), so it is not simply
        ** a bad pointer.  TPROT is what is left. */
    }
    return 0;
}

/* ---- case 3: the never-referenced tail (a NEGATIVE control) --------------- */
static void case_tail_untouched(void)
{
    char     *big;
    unsigned  base;
    NSFV_REQ *req;

    big = (char *)malloc(3u * D1R_PAGE);
    if (big == NULL) {
        CHECK(0, "2.4(3): could not allocate the straddling buffer");
        return;
    }
    /* Place the block so offsets 0-7 sit at the very end of one page and
     * 8-63 in the next: the client writes ONLY 0-7, so the tail page is never
     * referenced by this client at all. */
    base = ((unsigned)big + D1R_PAGE - 1u) & ~(D1R_PAGE - 1u);
    req  = (NSFV_REQ *)(base + D1R_PAGE - 8u);

    memcpy(req->eye, NSFV_REQ_EYE, 4);          /* offsets 0-3                */
    req->func = NSFV_REQ_QUERY;                 /* offsets 4-7                */
    /* offsets 8..63 deliberately UNTOUCHED by this client */

    printf("  case 3: block at %08X (page boundary at %08X), tail unwritten\n",
           (unsigned)req, (unsigned)(base + D1R_PAGE));

    d1r_svc(req);

    CHECK_EQ((long)req->rc, (long)NSFV_RC_OK,
             "2.4(3): a block whose TAIL the client never touched is ACCEPTED");
    printf("  case 3: rc=%d qstate=%u inflight=%u\n",
           (int)req->rc, (unsigned)req->qstate, (unsigned)req->qinfl);
    free(big);
}

int main(void)
{
    int skipped = 0;
    int rc;

    printf("=== nsf370 M5-2d1 live gate 2.4: the R8 validation (TSTD1R) ===\n");
    wtof("TSTD1R: R8 GATE START");

    CHECK_EQ((long)__isauth(), 0L, "the client is UNAUTHORISED");

    if (find_anchor() != 0) {
        printf("  anchor chase failed -- is NSFS started?\n");
        wtof("TSTD1R: NO ANCHOR -- GATE SKIPPED");
        return D1R_CC_SKIP;
    }

    case_no_eye();
    skipped = case_foreign_r8();
    case_tail_untouched();

    wtof("TSTD1R: R8 GATE DONE");
    rc = mbt_test_summary("TSTD1R");
    /* A SKIPPED CASE 2 MUST NOT LEAVE A GREEN RETURN CODE.  Cases 1 and 3 can
    ** all pass while the one case the gate exists for never ran -- which is
    ** precisely what happened in the previous round.  Report "could not run"
    ** in preference to "passed"; a real failure (rc 1) still outranks it. */
    if (rc == 0 && skipped) {
        printf("  CASE 2 DID NOT RUN -- returning CC %d, NOT a pass\n",
               D1R_CC_SKIP);
        wtof("TSTD1R: CASE 2 SKIPPED -- CC %d, NOT a pass", D1R_CC_SKIP);
        return D1R_CC_SKIP;
    }
    return rc;
}
