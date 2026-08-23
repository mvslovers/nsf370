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
