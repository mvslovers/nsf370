/*
 * tstreqx.c -- M5-2a: the Phase-2 NSFRQE crossing, host unit tests (ADR-0041).
 *
 * The crossing's field rules are pure logic, so they are pinned HERE rather
 * than by a live run: which of the frozen 64 bytes travel at each hop, which
 * are rewritten, and which must never be written back. Two of these tests are
 * the difference between M5-2a and a buffer overread -- see the ulen block.
 *
 * What this file deliberately does NOT test: the SVC, the CSA, the key
 * switches, the reply POST and the client-death guard. Those are the transport
 * (ADR-0038/0039/0040), proven live in Stage-0, and not host-simulable.
 */
#include "nsfreqx.h"
#include <mbtcheck.h>
#include <string.h>

/* A recognisable non-zero value per field, so a field that fails to travel (or
 * one that travels when it must not) shows up as a mismatch rather than as an
 * accidental match against zero. */
static void fill_request(NSFRQE *r)
{
    memset(r, 0, sizeof(*r));
    r->q.next   = (QELEM *)r;           /* caller-side linkage: must NOT cross */
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = RQ_SENDTO;
    r->flags    = RQ_F_NONBLOCK;
    r->sockdesc = 0x00010002u;
    r->ubuf     = (void *)0x00ABCDE0u;  /* a CALLER-AS address                 */
    r->ulen     = 100u;
    r->p1       = 0xC0A8C802u;
    r->p2       = 7777u;
    r->p3       = 0u;
    r->retcode  = -1;
    r->errno_   = 0;
    r->ecb      = 0x11223344u;
    r->reqid    = 42u;
    r->apptok   = 0x00010000u;
}

int main(void)
{
    NSFRQE  caller, slot, priv;
    char    stage[NSFREQX_CHUNK];
    UINT    staged;

    printf("=== nsf370 NSFRQE crossing tests (M5-2a) ===\n");

    /* ---- the frozen contract itself ------------------------------------
     * NOT sizeof(NSFRQE) == 64 here: on the host the two pointers (q.next,
     * ubuf) are 8 bytes and inflate the struct to 80. The 64-byte freeze is a
     * TARGET property, guarded where it is true -- NSF_SIZE_ASSERT(NSFRQE, 64)
     * in nsfreq.h, which fires at cc370 cross-compile (__MVS__ only, nsf.h).
     * What IS host-meaningful is that the crossing copies the whole struct
     * whatever its width, so the copy is expressed in sizeof and checked as a
     * full-image compare below. */
    CHECK(sizeof(NSFRQE) >= 64u,
          "NSFRQE is at least its frozen 64 bytes (host pointers inflate it)");

    /* ---- nsfreqx_stage_len: the moved-length contract ------------------ */
    CHECK_EQ((long)nsfreqx_stage_len(0u), 0L, "stage_len(0) = 0");
    CHECK_EQ((long)nsfreqx_stage_len(1u), 1L, "stage_len(1) = 1");
    CHECK_EQ((long)nsfreqx_stage_len(100u), 100L, "stage_len(100) = 100");
    CHECK_EQ((long)nsfreqx_stage_len(NSFREQX_CHUNK), (long)NSFREQX_CHUNK,
             "stage_len(chunk) = chunk (exact fit, not clamped)");
    CHECK_EQ((long)nsfreqx_stage_len(NSFREQX_CHUNK + 1u), (long)NSFREQX_CHUNK,
             "stage_len(chunk+1) clamps to chunk");
    CHECK_EQ((long)nsfreqx_stage_len(5000u), (long)NSFREQX_CHUNK,
             "stage_len(5000) clamps to chunk -- the truncating case");

    /* ---- hop 1: caller -> CSA slot ------------------------------------- */
    fill_request(&caller);
    memset(&slot, 0xFF, sizeof(slot));
    nsfreqx_slot_in(&slot, &caller);
    CHECK(memcmp(&slot, &caller, sizeof(NSFRQE)) == 0,
          "slot_in copies the full 64-byte image verbatim");
    CHECK(slot.ubuf == caller.ubuf,
          "slot keeps the CALLER-AS ubuf (rewritten at the next hop, not here)");

    /* ---- hop 2: CSA slot -> STC-private copy --------------------------- */
    memset(stage, 0, sizeof(stage));
    staged = nsfreqx_stage_len(slot.ulen);
    memset(&priv, 0xFF, sizeof(priv));
    nsfreqx_dispatch_in(&priv, &slot, stage, staged);

    /* the two rewrites -- the correctness crux */
    CHECK(priv.ubuf == (void *)stage,
          "dispatch_in rewrites ubuf to the staging buffer (not the caller's)");
    CHECK_EQ((long)priv.ulen, 100L,
             "dispatch_in sets ulen to the staged count");
    CHECK(priv.q.next == NULL,
          "dispatch_in clears q: the caller's linkage never crosses");
    CHECK_EQ((long)priv.ecb, 0L,
             "dispatch_in clears the private ecb (completion reads this word)");

    /* The regression that matters: the caller's ecb is VESTIGIAL in Phase 2, so
     * a client never initialises it. If the private copy inherited a word with
     * the POSTED bit already set, the STC's end-of-pass check would call the
     * request complete on its FIRST look and reply with an untouched retcode --
     * wrong answer, no abend, nothing to grep for. The earlier assertion alone
     * does not catch this: 0x11223344 happens to have bit 0x40000000 clear. */
    fill_request(&caller);
    caller.ecb = 0x40000000u;               /* POSTED bit set on arrival       */
    nsfreqx_slot_in(&slot, &caller);
    nsfreqx_dispatch_in(&priv, &slot, stage, nsfreqx_stage_len(slot.ulen));
    CHECK_EQ((long)(priv.ecb & 0x40000000u), 0L,
             "a POSTED caller ecb does NOT arrive posted in the private copy");
    CHECK_EQ((long)slot.ecb, (long)0x40000000u,
             "the slot still carries the caller's own ecb word untouched");

    fill_request(&caller);
    nsfreqx_slot_in(&slot, &caller);
    nsfreqx_dispatch_in(&priv, &slot, stage, nsfreqx_stage_len(slot.ulen));

    /* everything else must arrive intact -- the op needs its inputs */
    CHECK_EQ((long)priv.fn, (long)RQ_SENDTO, "fn survives the crossing");
    CHECK_EQ((long)priv.flags, (long)RQ_F_NONBLOCK, "flags survive");
    CHECK_EQ((long)priv.sockdesc, (long)0x00010002u, "sockdesc survives");
    CHECK_EQ((long)priv.p1, (long)0xC0A8C802u, "p1 survives");
    CHECK_EQ((long)priv.p2, 7777L, "p2 survives");
    CHECK_EQ((long)priv.apptok, (long)0x00010000u, "apptok survives");
    CHECK(memcmp(priv.eye, NSFRQE_EYE, 4) == 0, "eyecatcher survives");

    /* THE defect this whole design exists to prevent: an over-length request
     * must reach the dispatcher clamped, or the op reads past the staging
     * buffer AND reports more bytes moved than ever crossed (ADR-0041 2). */
    fill_request(&caller);
    caller.ulen = 5000u;
    nsfreqx_slot_in(&slot, &caller);
    staged = nsfreqx_stage_len(slot.ulen);
    nsfreqx_dispatch_in(&priv, &slot, stage, staged);
    CHECK_EQ((long)slot.ulen, 5000L,
             "the slot still records what the caller ASKED for");
    CHECK_EQ((long)priv.ulen, (long)NSFREQX_CHUNK,
             "the dispatched copy carries only what was STAGED (no overread)");
    CHECK(priv.ulen < slot.ulen,
          "truncation is visible to the dispatcher, not silent");

    /* a zero-length request is legal and must not be turned into a chunk */
    fill_request(&caller);
    caller.ulen = 0u;
    nsfreqx_slot_in(&slot, &caller);
    staged = nsfreqx_stage_len(slot.ulen);
    nsfreqx_dispatch_in(&priv, &slot, stage, staged);
    CHECK_EQ((long)priv.ulen, 0L, "a zero-length request stays zero-length");

    /* ---- hop 3a: private copy -> CSA slot (result fields only) --------- */
    fill_request(&caller);
    nsfreqx_slot_in(&slot, &caller);
    staged = nsfreqx_stage_len(slot.ulen);
    nsfreqx_dispatch_in(&priv, &slot, stage, staged);

    /* the executive completes the op */
    priv.retcode = 100;                 /* bytes moved -- BSD send semantics   */
    priv.errno_  = 0;
    priv.apptok  = 0x00020000u;
    priv.p1      = 0xDEADBEEFu;
    priv.p2      = 1234u;
    priv.p3      = 5678u;
    nsfreqx_result_out(&slot, &priv);

    CHECK_EQ((long)slot.retcode, 100L, "result_out carries retcode (the count)");
    CHECK_EQ((long)slot.errno_, 0L, "result_out carries errno_");
    CHECK_EQ((long)slot.apptok, (long)0x00020000u, "result_out carries apptok");
    CHECK_EQ((long)slot.p1, (long)0xDEADBEEFu, "result_out carries p1");
    CHECK_EQ((long)slot.p2, 1234L, "result_out carries p2");
    CHECK_EQ((long)slot.p3, 5678L, "result_out carries p3");

    /* the hazard: the private ubuf is the STC's staging address. Writing it
     * back would hand the client a pointer into another address space. */
    CHECK(slot.ubuf == caller.ubuf,
          "result_out does NOT write ubuf back (the cross-AS pointer hazard)");
    CHECK_EQ((long)slot.ulen, 100L, "result_out does NOT write ulen back");
    CHECK_EQ((long)slot.fn, (long)RQ_SENDTO, "result_out does not touch fn");
    CHECK_EQ((long)slot.sockdesc, (long)0x00010002u,
             "result_out does not touch sockdesc");
    CHECK_EQ((long)slot.ecb, (long)0x11223344u, "result_out does not touch ecb");
    CHECK_EQ((long)slot.reqid, 42L, "result_out does not touch reqid");

    /* ---- hop 3b: CSA slot -> the caller's own NSFRQE ------------------- */
    nsfreqx_result_in(&caller, &slot);
    CHECK_EQ((long)caller.retcode, 100L, "result_in delivers retcode");
    CHECK_EQ((long)caller.errno_, 0L, "result_in delivers errno_");
    CHECK_EQ((long)caller.apptok, (long)0x00020000u, "result_in delivers apptok");
    CHECK_EQ((long)caller.p1, (long)0xDEADBEEFu, "result_in delivers p1");
    CHECK_EQ((long)caller.p2, 1234L, "result_in delivers p2");
    CHECK_EQ((long)caller.p3, 5678L, "result_in delivers p3");
    CHECK(caller.ubuf == (void *)0x00ABCDE0u,
          "result_in leaves the caller's own ubuf untouched");
    CHECK_EQ((long)caller.ulen, 100L, "result_in leaves the caller's ulen");
    CHECK_EQ((long)caller.ecb, (long)0x11223344u, "result_in leaves the ecb");
    CHECK(caller.q.next == (QELEM *)&caller,
          "result_in leaves the caller's queue linkage untouched");

    /* an error result travels the same way */
    priv.retcode = NSF_RETERR;
    priv.errno_  = NSF_EWOULDBLOCK;
    nsfreqx_result_out(&slot, &priv);
    nsfreqx_result_in(&caller, &slot);
    CHECK_EQ((long)caller.retcode, (long)NSF_RETERR, "an error retcode travels");
    CHECK_EQ((long)caller.errno_, (long)NSF_EWOULDBLOCK, "its errno_ travels");

    /* ---- the anchor guard-word truth table ----------------------------- *
     * Host-pinned rather than left to a live run: the RQE slot and the
     * published wake-ECB address are neighbours in the anchor, and an overrun
     * onto that pointer does not fail cleanly -- it is still non-zero, so the
     * SVC routine takes the key-8 branch and POSTs key-0 to a wrong address in
     * the STC's private storage. Every row below is a way that can happen. */
    {
        char g[NSFREQX_GUARDLEN];
        UINT k;

        memcpy(g, NSFREQX_GUARD, NSFREQX_GUARDLEN);
        CHECK_EQ((long)nsfreqx_guard_ok(g), 1L,
                 "the correct guard pattern reads as intact");

        memset(g, 0, sizeof(g));
        CHECK_EQ((long)nsfreqx_guard_ok(g), 0L,
                 "a ZEROED guard is bad (why the pattern is not zero)");

        /* Any single byte altered, at any position -- the overrun case is a
         * clobber of the FIRST byte, but a guard that only caught that one
         * would miss a short write landing anywhere else in the word. */
        for (k = 0u; k < NSFREQX_GUARDLEN; k++) {
            memcpy(g, NSFREQX_GUARD, NSFREQX_GUARDLEN);
            g[k] = (char)(g[k] + 1);
            CHECK_EQ((long)nsfreqx_guard_ok(g), 0L,
                     "a single altered byte makes the guard read as bad");
        }

        CHECK_EQ((long)nsfreqx_guard_ok(NULL), 0L,
                 "a NULL guard answers 'not ok' and does not fault");
    }

    /* ======================================================================
     * M5-2b3 (ADR-0042): the slot pool's pure arithmetic.
     * ====================================================================== */

    /* ---- the client-death classifier, every row ---------------------------
     * A fake ASVT: three ASIDs, so the asid-1 index has a NEIGHBOUR on both
     * sides to be wrong about. The addresses are distinct and non-zero. */
    {
        UINT enty[3];
        const UINT ascb1 = 0x00F10000u;     /* the AS occupying ASID 1        */
        const UINT ascb2 = 0x00F20000u;     /* ASID 2                          */
        const UINT ascb3 = 0x00F30000u;     /* ASID 3                          */

        enty[0] = ascb1;
        enty[1] = ascb2;
        enty[2] = ascb3;

        /* LIVE: the recorded ASCB is the one the ASVT names for that ASID. */
        CHECK_EQ((long)nsfreqx_classify(ascb1, 1u, 3u, enty),
                 (long)NSFREQX_CL_LIVE, "classify: ASID 1 matching ASCB is LIVE");
        CHECK_EQ((long)nsfreqx_classify(ascb3, 3u, 3u, enty),
                 (long)NSFREQX_CL_LIVE, "classify: the last ASID is LIVE too");

        /* THE asid-1 INDEX. Off by one in either direction turns each of these
         * into a mismatch against a NEIGHBOUR's ASCB and answers DEAD, so a
         * passing pair pins the index rather than merely exercising it. */
        CHECK_EQ((long)nsfreqx_classify(ascb2, 2u, 3u, enty),
                 (long)NSFREQX_CL_LIVE, "classify: asid-1 indexes entry[1] for ASID 2");
        CHECK_EQ((long)nsfreqx_classify(ascb1, 2u, 3u, enty),
                 (long)NSFREQX_CL_DEAD,
                 "classify: ASID 2 holding ASID 1's ASCB is DEAD (reuse)");

        /* DEAD by the availability bit: the ASID is free, the AS ended. */
        enty[1] = NSFREQX_ASVT_AVAIL;
        CHECK_EQ((long)nsfreqx_classify(ascb2, 2u, 3u, enty),
                 (long)NSFREQX_CL_DEAD, "classify: ASVTAVAI set is DEAD");
        /* The AVAIL bit wins even when the rest of the word still matches --
         * a freed entry can keep its old address bits. */
        enty[1] = NSFREQX_ASVT_AVAIL | ascb2;
        CHECK_EQ((long)nsfreqx_classify(ascb2, 2u, 3u, enty),
                 (long)NSFREQX_CL_DEAD,
                 "classify: AVAIL wins over a still-matching address");
        enty[1] = ascb2;

        /* DEAD by ASID reuse -- the row an ASID-only check cannot see. */
        CHECK_EQ((long)nsfreqx_classify(0x00FF0000u, 2u, 3u, enty),
                 (long)NSFREQX_CL_DEAD, "classify: a different ASCB at that ASID is DEAD");

        /* UNKNOWN, all four ways in. */
        CHECK_EQ((long)nsfreqx_classify(0u, 1u, 3u, enty),
                 (long)NSFREQX_CL_UNKNOWN, "classify: no recorded ASCB is UNKNOWN");
        CHECK_EQ((long)nsfreqx_classify(ascb1, 1u, 3u, NULL),
                 (long)NSFREQX_CL_UNKNOWN, "classify: no ASVT is UNKNOWN");
        CHECK_EQ((long)nsfreqx_classify(ascb1, 1u, 0u, enty),
                 (long)NSFREQX_CL_UNKNOWN, "classify: a zero ASVTMAXU is UNKNOWN");
        CHECK_EQ((long)nsfreqx_classify(ascb1, 0u, 3u, enty),
                 (long)NSFREQX_CL_UNKNOWN, "classify: a zero ASID is UNKNOWN");
        CHECK_EQ((long)nsfreqx_classify(ascb1, 4u, 3u, enty),
                 (long)NSFREQX_CL_UNKNOWN, "classify: an ASID past ASVTMAXU is UNKNOWN");
        /* The boundary itself is IN range -- an off-by-one here would make the
         * highest ASID permanently unclassifiable. */
        CHECK_EQ((long)nsfreqx_classify(ascb3, 3u, 3u, enty),
                 (long)NSFREQX_CL_LIVE, "classify: ASID == ASVTMAXU is in range");
    }

    /* ---- the drain's per-slot decision ------------------------------------ */
    {
        /* Only PENDING is a work item. */
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_FREE,
                     NSFREQX_CL_LIVE, 1, 1), (long)NSFREQX_ACT_NONE,
                 "slot_action: FREE is not a work item");
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_CLAIMED,
                     NSFREQX_CL_LIVE, 1, 1), (long)NSFREQX_ACT_NONE,
                 "slot_action: CLAIMED is mid-claim, not a work item");
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_DONE,
                     NSFREQX_CL_LIVE, 1, 1), (long)NSFREQX_ACT_NONE,
                 "slot_action: DONE belongs to its owner");
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_HELD,
                     NSFREQX_CL_LIVE, 1, 1), (long)NSFREQX_ACT_NONE,
                 "slot_action: HELD is off the work list");

        /* The happy path. */
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_LIVE, 1, 1), (long)NSFREQX_ACT_DISPATCH,
                 "slot_action: a live client with trusted storage dispatches");

        /* Liveness first. */
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_DEAD, 1, 1), (long)NSFREQX_ACT_REAP,
                 "slot_action: a dead client is reaped");
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_UNKNOWN, 1, 1), (long)NSFREQX_ACT_HOLD,
                 "slot_action: an unknown client is held");

        /* Storage trust, once the client is known live. */
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_LIVE, 0, 1), (long)NSFREQX_ACT_REAP_BAD,
                 "slot_action: a clobbered guard reaps, never dispatches");
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_LIVE, 1, 0), (long)NSFREQX_ACT_REAP_BAD,
                 "slot_action: a corrupt wake-ECB pointer reaps");

        /* ORDER IS THE CONTRACT: a dead client is reaped whether or not its
         * storage survived. If trust were tested first these would answer
         * REAP_BAD, which is a different message and a different diagnosis. */
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_DEAD, 0, 0), (long)NSFREQX_ACT_REAP,
                 "slot_action: liveness is decided BEFORE storage trust");
        CHECK_EQ((long)nsfreqx_slot_action(NSFREQX_ST_PENDING,
                     NSFREQX_CL_UNKNOWN, 0, 0), (long)NSFREQX_ACT_HOLD,
                 "slot_action: an unknown client is held, not reaped, even so");

        /* No path answers DISPATCH for anything but a live, trusted PENDING --
         * the property that matters is that nothing ELSE reaches the executive. */
        {
            UINT st[5];
            int  v, g, pt, i;
            int  dispatches = 0;
            st[0] = NSFREQX_ST_FREE;    st[1] = NSFREQX_ST_PENDING;
            st[2] = NSFREQX_ST_DONE;    st[3] = NSFREQX_ST_HELD;
            st[4] = NSFREQX_ST_CLAIMED;
            for (i = 0; i < 5; i++)
                for (v = 0; v <= 2; v++)
                    for (g = 0; g <= 1; g++)
                        for (pt = 0; pt <= 1; pt++)
                            if (nsfreqx_slot_action(st[i], v, g, pt) ==
                                NSFREQX_ACT_DISPATCH)
                                dispatches++;
            CHECK_EQ((long)dispatches, 1L,
                     "slot_action: exactly ONE of 60 input rows dispatches");
        }
    }

    /* ---- the slot state machine ------------------------------------------- */
    {
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_FREE, NSFREQX_ST_CLAIMED),
                 1L, "legal: FREE -> CLAIMED (the CS claim)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_CLAIMED, NSFREQX_ST_PENDING),
                 1L, "legal: CLAIMED -> PENDING (publish last)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_CLAIMED, NSFREQX_ST_FREE),
                 1L, "legal: CLAIMED -> FREE (bail before publishing)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_PENDING, NSFREQX_ST_DONE),
                 1L, "legal: PENDING -> DONE (serviced)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_PENDING, NSFREQX_ST_HELD),
                 1L, "legal: PENDING -> HELD (liveness unknown)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_PENDING, NSFREQX_ST_FREE),
                 1L, "legal: PENDING -> FREE (reaped)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_HELD, NSFREQX_ST_FREE),
                 1L, "legal: HELD -> FREE (unstaged)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_DONE, NSFREQX_ST_FREE),
                 1L, "legal: DONE -> FREE (the owner releases)");
        /* The reaper takes ownership before clearing -- clearing storage it
         * does not own is the race the CS exists to prevent. */
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_PENDING, NSFREQX_ST_CLAIMED),
                 1L, "legal: PENDING -> CLAIMED (the reaper takes ownership)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_HELD, NSFREQX_ST_CLAIMED),
                 1L, "legal: HELD -> CLAIMED (the reaper)");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_DONE, NSFREQX_ST_CLAIMED),
                 1L, "legal: DONE -> CLAIMED (the reaper)");

        /* The illegal ones are the interesting ones. */
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_FREE, NSFREQX_ST_PENDING),
                 0L, "illegal: FREE -> PENDING publishes an unclaimed slot");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_CLAIMED, NSFREQX_ST_DONE),
                 0L, "illegal: CLAIMED -> DONE services mid-staging");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_FREE, NSFREQX_ST_DONE),
                 0L, "illegal: FREE -> DONE");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_DONE, NSFREQX_ST_PENDING),
                 0L, "illegal: DONE -> PENDING re-publishes a served slot");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_HELD, NSFREQX_ST_PENDING),
                 0L, "illegal: HELD -> PENDING puts it back on the work list");
        CHECK_EQ((long)nsfreqx_slot_legal(NSFREQX_ST_FREE, NSFREQX_ST_FREE),
                 0L, "illegal: FREE -> FREE is not a transition");
        CHECK_EQ((long)nsfreqx_slot_legal(99u, NSFREQX_ST_FREE),
                 0L, "illegal: an unknown state has no legal successor");
    }

    /* ---- the reap predicate ----------------------------------------------- */
    {
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_PENDING, NSFREQX_CL_DEAD, 1),
                 1L, "reap: a DEAD client's PENDING slot");
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_HELD, NSFREQX_CL_DEAD, 1),
                 1L, "reap: a DEAD client's HELD slot");
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_DONE, NSFREQX_CL_DEAD, 1),
                 1L, "reap: a DEAD client's DONE slot");

        /* Never on a verdict that is not DEAD -- the safe-side asymmetry --
         * PROVIDED the storage is trustworthy. */
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_PENDING, NSFREQX_CL_LIVE, 1),
                 0L, "reap: never a LIVE client whose storage is intact");
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_PENDING,
                                       NSFREQX_CL_UNKNOWN, 1),
                 0L, "reap: never an UNKNOWN client whose storage is intact");

        /* THE SECOND REASON. Untrusted storage reclaims regardless of liveness,
         * because the slot must never be POSTed through -- and a LIVE verdict
         * alone would have refused. This is the row that made the predicate's
         * old two-argument shape unusable at one of its three call sites. */
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_PENDING, NSFREQX_CL_LIVE, 0),
                 1L, "reap: untrusted storage reclaims even a LIVE client");
        /* But NOT an UNKNOWN one, and this row is the point of the third
         * state: HOLD already guarantees "never POST through this slot"
         * without freeing anything, so reclaiming would buy nothing and risk
         * a LIVE client's storage. Asserted here because the agreement sweep
         * below CAUGHT me writing the opposite. */
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_PENDING,
                                       NSFREQX_CL_UNKNOWN, 0),
                 0L, "reap: untrusted storage does NOT reclaim an UNKNOWN"
                     " client -- HOLD covers it without freeing");

        /* Never a state the client has not published -- and untrusted storage
         * does NOT override that: an unpublished slot is not a request. */
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_CLAIMED, NSFREQX_CL_DEAD, 1),
                 0L, "reap: never CLAIMED (excluded HERE and nowhere else --"
                     " a CLAIMED slot IS classifiable)");
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_CLAIMED, NSFREQX_CL_LIVE, 0),
                 0L, "reap: never CLAIMED, not even on untrusted storage");
        CHECK_EQ((long)nsfreqx_reap_ok(NSFREQX_ST_FREE, NSFREQX_CL_DEAD, 1),
                 0L, "reap: never FREE -- it is not a request");
    }

    /* ---- the two helpers must not contradict each other -------------------
     * THIS is what keeps one rule from becoming two, and getting the invariant
     * right took a failing run: the two functions answer DIFFERENT questions.
     *
     *   nsfreqx_slot_action  what the drain does with a slot ON THIS PASS --
     *                        and only a PENDING slot is a work item at all.
     *   nsfreqx_reap_ok      whether a slot MAY be reclaimed, ever -- which is
     *                        also true of a dead client's HELD or DONE slot,
     *                        reached by the completion path rather than by the
     *                        scan.
     *
     * So the relationship is an IMPLICATION, not an equality: every reap the
     * action helper MANDATES must be one the predicate PERMITS. The converse
     * is false by design, and asserting equality here failed on 14 of 60 rows
     * -- all of them HELD/DONE slots the predicate rightly allows and the
     * per-pass dispatcher rightly ignores. */
    {
        UINT st[5];
        int  v, g, pt, i;
        int  mismatches = 0;

        st[0] = NSFREQX_ST_FREE;    st[1] = NSFREQX_ST_PENDING;
        st[2] = NSFREQX_ST_DONE;    st[3] = NSFREQX_ST_HELD;
        st[4] = NSFREQX_ST_CLAIMED;

        for (i = 0; i < 5; i++) {
            for (v = 0; v <= 2; v++) {
                for (g = 0; g <= 1; g++) {
                    for (pt = 0; pt <= 1; pt++) {
                        int act  = nsfreqx_slot_action(st[i], v, g, pt);
                        int acts = (act == NSFREQX_ACT_REAP ||
                                    act == NSFREQX_ACT_REAP_BAD);
                        int pred = nsfreqx_reap_ok(st[i], v, (g && pt));
                        /* mandated => permitted */
                        if (acts && !pred) mismatches++;
                    }
                }
            }
        }
        CHECK_EQ((long)mismatches, 0L,
                 "every reap slot_action MANDATES is one reap_ok PERMITS,"
                 " across all 60 inputs (the drain can never order a reclaim"
                 " the predicate forbids)");
    }

    /* ---- can this pass CONSUME that outcome? (M5-2b4) ----------------------
     * nsfreqx_actionable is the second half of the drain's decision, and it
     * exists because a WAIT-gate probe that reports work the drain then
     * declines is not a latency fix -- the executive skips its WAIT whenever a
     * probe answers non-zero, so the pass makes no progress and repeats.  A
     * hot spin on the executive task.  These rows are what keeps that shape
     * out of nsfsx.c, where it cannot be host-tested at all. */
    {
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_DISPATCH, 0), 1L,
                 "actionable: a dispatchable request is taken when nothing is"
                 " in service");
        /* THE ANTI-SPIN ROW.  Serialised service (ADR-0042 10) means the one
         * private NSFRQE is held, so this outcome is NOT consumable -- and the
         * probe must therefore not report it. */
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_DISPATCH, 1), 0L,
                 "actionable: a dispatchable request is NOT consumable while"
                 " one is in service (reporting it would spin the executive)");

        /* All three finish inside the CSA slot -- no private NSFRQE, no
         * executive dispatch -- so nothing about them waits on the request in
         * service.  Before b4 they did, for as long as an unrelated client's
         * blocking operation ran. */
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_REAP, 1), 1L,
                 "actionable: a dead client's slot is reaped even while a"
                 " request is in service");
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_HOLD, 1), 1L,
                 "actionable: an unknown client's slot is held even so");
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_REAP_BAD, 1), 1L,
                 "actionable: untrusted storage is reclaimed even so");
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_REAP, 0), 1L,
                 "actionable: and equally when nothing is in service");
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_HOLD, 0), 1L,
                 "actionable: and equally when nothing is in service (hold)");
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_REAP_BAD, 0), 1L,
                 "actionable: and equally when nothing is in service (bad)");

        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_NONE, 0), 0L,
                 "actionable: NONE is not work, idle or busy (idle)");
        CHECK_EQ((long)nsfreqx_actionable(NSFREQX_ACT_NONE, 1), 0L,
                 "actionable: NONE is not work, idle or busy (busy)");
        CHECK_EQ((long)nsfreqx_actionable(4242, 0), 0L,
                 "actionable: an unrecognised action is never consumed");

        /* THE PROPERTY, not the rows: nothing consumable while a request is in
         * service may need the private NSFRQE.  That single sentence is the
         * whole anti-spin argument, and it holds over every action code rather
         * than over the four we happened to enumerate. */
        {
            int a, needs_priv = 0;

            for (a = -1; a <= 8; a++) {
                if (nsfreqx_actionable(a, 1) &&
                    a == NSFREQX_ACT_DISPATCH) needs_priv++;
            }
            CHECK_EQ((long)needs_priv, 0L,
                     "actionable: NO outcome consumable while busy needs the"
                     " private NSFRQE");
        }

        /* And composed with the truth table it gates: with nothing in service
         * every PENDING row is consumable (the drain declines none of them),
         * and with a request in service exactly the dispatch rows drop out. */
        {
            UINT st[5];
            int  v, g, pt, i;
            int  idle_consumable = 0, busy_consumable = 0, pending_rows = 0;

            st[0] = NSFREQX_ST_FREE;    st[1] = NSFREQX_ST_PENDING;
            st[2] = NSFREQX_ST_DONE;    st[3] = NSFREQX_ST_HELD;
            st[4] = NSFREQX_ST_CLAIMED;
            for (i = 0; i < 5; i++) {
                for (v = 0; v <= 2; v++) {
                    for (g = 0; g <= 1; g++) {
                        for (pt = 0; pt <= 1; pt++) {
                            int act = nsfreqx_slot_action(st[i], v, g, pt);
                            if (st[i] == NSFREQX_ST_PENDING) pending_rows++;
                            if (nsfreqx_actionable(act, 0)) idle_consumable++;
                            if (nsfreqx_actionable(act, 1)) busy_consumable++;
                        }
                    }
                }
            }
            CHECK_EQ((long)idle_consumable, (long)pending_rows,
                     "actionable: idle, every PENDING row is consumable and no"
                     " other row is");
            CHECK_EQ((long)(idle_consumable - busy_consumable), 1L,
                     "actionable: busy, exactly the one dispatching row drops"
                     " out -- no more and no less");
        }
    }

    /* ---- the rc -> errno mapping ------------------------------------------ */
    {
        CHECK_EQ((long)nsfreqx_rc_errno(NSFREQX_RC_OK), 0L,
                 "rc_errno: OK is not an error");
        /* THE reason this helper exists: a full pool is a healthy stack, and
         * ENOBUFS tells the application to retry where ESHUTDOWN tells it to
         * give up. */
        CHECK_EQ((long)nsfreqx_rc_errno(NSFREQX_RC_NOBUF), (long)NSF_ENOBUFS,
                 "rc_errno: a full pool is ENOBUFS, not ESHUTDOWN");
        CHECK(NSF_ENOBUFS != NSF_ESHUTDOWN,
              "rc_errno: the two answers are actually distinguishable");
        CHECK_EQ((long)nsfreqx_rc_errno(NSFREQX_RC_INVALID), (long)NSF_ESHUTDOWN,
                 "rc_errno: INVALID is ESHUTDOWN");
        CHECK_EQ((long)nsfreqx_rc_errno(NSFREQX_RC_CORRUPT), (long)NSF_ESHUTDOWN,
                 "rc_errno: CORRUPT is ESHUTDOWN");
        CHECK_EQ((long)nsfreqx_rc_errno(NSFREQX_RC_NOREQ), (long)NSF_ESHUTDOWN,
                 "rc_errno: NOREQ is ESHUTDOWN");
        CHECK_EQ((long)nsfreqx_rc_errno(4242), (long)NSF_ESHUTDOWN,
                 "rc_errno: an unknown rc still answers something safe");
    }

    /* ---- the guard word is now checked PER SLOT ---------------------------
     * nsfreqx_guard_ok already takes the guard's address, so the pool needs no
     * new entry point -- but "already works" is an inference, so check that two
     * independent slot guards are judged independently. */
    {
        char ga[NSFREQX_GUARDLEN];
        char gb[NSFREQX_GUARDLEN];

        memcpy(ga, NSFREQX_GUARD, NSFREQX_GUARDLEN);
        memcpy(gb, NSFREQX_GUARD, NSFREQX_GUARDLEN);
        CHECK_EQ((long)nsfreqx_guard_ok(ga), 1L, "per-slot guard: slot A intact");
        CHECK_EQ((long)nsfreqx_guard_ok(gb), 1L, "per-slot guard: slot B intact");

        gb[0] = (char)0;                /* only slot B is clobbered            */
        CHECK_EQ((long)nsfreqx_guard_ok(ga), 1L,
                 "per-slot guard: slot A survives slot B being clobbered");
        CHECK_EQ((long)nsfreqx_guard_ok(gb), 0L,
                 "per-slot guard: slot B is judged clobbered");
    }

    /* ---- the landing area: the 80-FIX bounded copy ------------------------
     * The executive dispatches against PRIVATE storage, never CSA, and this
     * helper is the crossing.  What has to be pinned here rather than left to
     * a live run: the bound is nsfreqx_stage_len and NOT the caller's word.
     * `xlen` arrives from a CSA slot an unauthorised client wrote, and after
     * the fix it bounds a memcpy into the STC's own private storage -- so an
     * inflated value must clamp, not overrun. */
    {
        static char land[NSFREQX_CHUNK];
        static char csa[NSFREQX_CHUNK];
        static char canary[NSFREQX_CHUNK];
        UINT n;
        unsigned k;

        for (k = 0u; k < NSFREQX_CHUNK; k++) {
            csa[k]    = (char)(k & 0xFFu);
            land[k]   = (char)0xEE;
            canary[k] = (char)0xEE;
        }

        n = nsfreqx_land_copy(land, csa, 100u);
        CHECK_EQ((long)n, 100L, "land_copy(100) moves 100");
        CHECK(memcmp(land, csa, 100u) == 0,
              "land_copy moves the bytes byte-exact");
        CHECK(memcmp(land + 100, canary + 100, NSFREQX_CHUNK - 100u) == 0,
              "land_copy writes NOTHING past the count it was given");

        /* THE ONE THAT MATTERS: a CSA-supplied length larger than the chunk
         * must clamp to the chunk, because the destination IS the chunk. */
        n = nsfreqx_land_copy(land, csa, 5000u);
        CHECK_EQ((long)n, (long)NSFREQX_CHUNK,
                 "land_copy clamps an over-long xlen to the chunk (no overrun)");
        n = nsfreqx_land_copy(land, csa, NSFREQX_CHUNK + 1u);
        CHECK_EQ((long)n, (long)NSFREQX_CHUNK,
                 "land_copy clamps chunk+1 -- the boundary, not just the far case");
        n = nsfreqx_land_copy(land, csa, NSFREQX_CHUNK);
        CHECK_EQ((long)n, (long)NSFREQX_CHUNK,
                 "land_copy at exactly the chunk is an exact fit, not clamped");
        CHECK(memcmp(land, csa, NSFREQX_CHUNK) == 0,
              "a whole-chunk copy is byte-exact end to end");

        /* Direction-neutral: the SAME call serves the copy out.  If this ever
         * needed a second function, the bound would have two encodings. */
        for (k = 0u; k < NSFREQX_CHUNK; k++) csa[k] = (char)0x11;
        n = nsfreqx_land_copy(csa, land, 64u);
        CHECK_EQ((long)n, 64L, "land_copy serves the OUT direction identically");
        CHECK(memcmp(csa, land, 64u) == 0, "the out direction is byte-exact");
        CHECK_EQ((long)csa[64] & 0xFF, 0x11L,
                 "the out direction also writes nothing past its count");

        /* NO CROSS-CLIENT RESIDUE ESCAPES THE LANDING AREA.
         *
         * g_land is ONE buffer shared by sequential clients, so the question
         * a reviewer will ask is whether client B's copy OUT can carry bytes
         * client A left behind.  It cannot, and the reason is structural: the
         * copy in and the copy out use the SAME count on the SAME slot, so
         * the range copied out is exactly the range the copy in just
         * overwrote.  Modelled here end to end rather than argued, because
         * "nothing escapes" is a property and an inspection is not a proof.
         *
         * Sizing the copy out by the moved count instead would ALSO be safe
         * -- but it is not available without a per-verb table (retcode is a
         * descriptor for SOCKET/ACCEPT), which is what the always-copy
         * decision excluded.  So this property is what carries it. */
        {
            static char slotA[NSFREQX_CHUNK];
            static char slotB[NSFREQX_CHUNK];
            UINT nA = 2048u, nB = 512u;   /* A uses the whole chunk, B a slice */
            unsigned j;
            int leaked = 0;

            for (j = 0u; j < NSFREQX_CHUNK; j++) {
                slotA[j] = (char)0xAA;    /* client A's staged content         */
                slotB[j] = (char)0xBB;    /* client B's staged content         */
                land[j]  = (char)0x00;
            }

            /* --- request A: copy in, the op writes 100 bytes, copy out --- */
            (void)nsfreqx_land_copy(land, slotA, nA);
            for (j = 0u; j < 100u; j++) land[j] = (char)0x11;   /* the op      */
            (void)nsfreqx_land_copy(slotA, land, nA);

            /* --- request B: a SHORTER request reusing the same landing area */
            (void)nsfreqx_land_copy(land, slotB, nB);
            for (j = 0u; j < 256u; j++) land[j] = (char)0x22;   /* the op      */
            (void)nsfreqx_land_copy(slotB, land, nB);

            /* B's slot must contain the op's bytes then B's OWN content --
             * and not one byte of A's 0xAA anywhere in the copied range. */
            for (j = 0u; j < nB; j++) {
                char want = (j < 256u) ? (char)0x22 : (char)0xBB;
                if (slotB[j] != want) { leaked = 1; break; }
            }
            CHECK(!leaked,
                  "a short request's slot holds the op's bytes then its OWN"
                  " staged content -- no previous client's bytes");

            leaked = 0;
            for (j = 0u; j < NSFREQX_CHUNK; j++) {
                if (slotB[j] == (char)0xAA) { leaked = 1; break; }
            }
            CHECK(!leaked,
                  "NOT ONE byte of the previous client survives into this"
                  " client's slot (the residue stays per-slot, not global)");

            /* A's own tail is its own staged content, exactly as it was
             * before a landing area existed -- the scope did not widen. */
            CHECK_EQ((long)slotA[2047] & 0xFF, 0xAAL,
                     "the long request's tail is still its OWN staged content");
        }

        CHECK_EQ((long)nsfreqx_land_copy(land, csa, 0u), 0L,
                 "land_copy(0) moves nothing -- the zero-byte receive");
        CHECK_EQ((long)nsfreqx_land_copy(NULL, csa, 10u), 0L,
                 "land_copy tolerates a NULL destination");
        CHECK_EQ((long)nsfreqx_land_copy(land, NULL, 10u), 0L,
                 "land_copy tolerates a NULL source");
    }

    /* ---- NULL guards: every entry is defensive ------------------------- */
    nsfreqx_slot_in(NULL, &caller);
    nsfreqx_slot_in(&slot, NULL);
    nsfreqx_dispatch_in(NULL, &slot, stage, 0u);
    nsfreqx_dispatch_in(&priv, NULL, stage, 0u);
    nsfreqx_result_out(NULL, &priv);
    nsfreqx_result_out(&slot, NULL);
    nsfreqx_result_in(NULL, &slot);
    nsfreqx_result_in(&caller, NULL);
    CHECK(1, "every entry tolerates NULL without a fault");

    return mbt_test_summary("TSTREQX");
}
