/*
 * tstubuf.c -- M5 Stage-0b Step 2: the ubuf keyed CSA-bounce CLIENT (TSTUBUF).
 *
 * ADR-0039.  MVS-only (project.toml host = false): the keyed CSA bounce
 * (MVCK, storage keys, cross-AS SVC/anchor) has no host analog; this IS the
 * transport test.  Host coverage is the NSF_SIZE_ASSERTs in nsfvsvc.h firing at
 * cc370 cross-compile.  The probe STC (NSFV) must already be started (S NSFV) --
 * the same peer-must-run prerequisite as TSTSVC and the M3/M4 live gates.
 *
 * THE RED LINE (ADR-0038/0039): the client is UNAUTHORIZED and stays so.  It
 * asserts TESTAUTH (__isauth() == 0) reports NOT authorized and NEVER calls
 * clib_apf_setup / any self-authorization SVC.  An unauthorized problem-state
 * program moving a data buffer app->stack->app byte-exact is the Stage-0b
 * deliverable; validation / owner_ascb / hostile-pointer recovery are M5-2.
 *
 * What it proves: a byte pattern moves app -> CSA staging -> STC(+1) -> app
 * byte-exact, over the sizes a real ubuf hits (0, 1, < chunk, = chunk, multi-
 * chunk), with NO overrun past ulen.  Per ADR-0039 "one round trip per chunk":
 * the CLIENT loops the 2048-byte chunks (one XFER SVC call per chunk, advancing
 * req.ubuf / req.ulen); the SVC routine moves ONE <= 2048 chunk per call.  The
 * STC applies a byte-wise +1 to the staged bytes -- position-sensitive, so a
 * short, wrong, or offset copy is visible, not silently plausible.
 *
 * Each size fills a buffer with an asymmetric, position-dependent pattern, puts
 * a guard byte immediately after ulen, runs the chunk loop, then verifies every
 * byte transformed exactly once (+1) AND the guard byte untouched (no write-out
 * overrun).  The short-after-large case proves the routine honours ulen on the
 * write-out (stale staging from the large transfer is not dumped past ulen).
 */
#include "nsfvsvc.h"
#include <clibos.h>         /* __isauth (TESTAUTH FCTN=1)                       */
#include <clibwto.h>        /* wtof (console markers survive a hang)           */
#include <mbtcheck.h>
#include <string.h>

#define TSTUBUF_GUARD  0x7Eu    /* distinctive guard byte; +1 -> 0x7F if hit   */

/* Room for the largest test (5000) + its guard byte. */
static unsigned char g_buf[8192];

/* Asymmetric, position-dependent pattern.  The i>>4 term varies the value
 * across 256-byte boundaries, so a mis-chunked or offset copy (e.g. chunk 2
 * written over chunk 1's region) is caught, not aliased.  pat(i)+1 is always
 * != pat(i) (a +1 mod 256 never equals the original), so a no-op routine that
 * left the buffer unchanged would fail the +1 check. */
static unsigned char
pat(unsigned i)
{
    return (unsigned char)(i * 5u + (i >> 4) + 0x11u);
}

/* Issue the private SVC with R1 = A(req) via the EX-SVC-0 trick (SVC number in
 * R6 -- the relink-only pattern; EX ORs its low byte into a stored "SVC 0", so
 * no storage is modified, RENT-safe).  The routine writes the result into *req
 * (a key-0 store into this problem-state key-8 block) AND transforms *ubuf; we
 * read both from memory afterward.  noinline: the named asm labels appear once.
 * Identical to TSTSVC's issuer (ADR-0038 6). */
static void __attribute__((noinline))
nsfv_svc_issue(NSFV_REQ *req)
{
    unsigned reqp = (unsigned)(void *)req;
    unsigned svcn = (unsigned)NSFV_SVCNUM;

    __asm__ __volatile__(
        "         LR    1,%0\n"          /* R1 = A(req)                        */
        "         LR    6,%1\n"          /* R6 = SVC number                    */
        "         EX    6,NSFUBUF0\n"    /* execute SVC <R6-low>               */
        "         B     NSFUBUFX\n"
        "NSFUBUF0 SVC   0\n"             /* EX target; storage unmodified       */
        "NSFUBUFX DS    0H\n"
        :
        : "r"(reqp), "r"(svcn)
        : "0", "1", "6", "15", "memory");
}

/* Move `size` bytes through the XFER round trip and verify.  Returns 1 on a
 * byte-exact, no-overrun result.  size == 0 issues exactly one call (ulen 0,
 * exercising the routine's zero-length guard); the do-while's `chunk > 0` guard
 * stops it re-looping. */
static int
run_xfer(unsigned size)
{
    unsigned off, i;
    unsigned chunk = 0u;
    int      dataok = 1;

    /* fill [0,size) with the pattern; guard byte immediately after ulen. */
    for (i = 0u; i < size; i++) g_buf[i] = pat(i);
    g_buf[size] = (unsigned char)TSTUBUF_GUARD;

    /* one XFER SVC call per <= 2048-byte chunk, advancing ubuf/ulen. */
    off = 0u;
    do {
        NSFV_REQ req;

        chunk = size - off;
        if (chunk > NSFV_XFER_CHUNK) chunk = NSFV_XFER_CHUNK;

        memset(&req, 0, sizeof req);
        memcpy(req.eye, NSFV_REQ_EYE, 4);
        req.func = NSFV_REQ_XFER;
        req.ubuf = &g_buf[off];
        req.ulen = chunk;
        req.rc   = -1;

        nsfv_svc_issue(&req);

        CHECK_EQ((long)req.rc, (long)NSFV_RC_OK, "XFER chunk router rc OK");
        off += chunk;
    } while (off < size && chunk > 0u);

    /* every byte transformed exactly once (+1), position-preserved. */
    for (i = 0u; i < size; i++) {
        if (g_buf[i] != (unsigned char)(pat(i) + 1u)) { dataok = 0; break; }
    }
    CHECK(dataok, "every byte +1 byte-exact through app->stage->STC->app");

    /* the guard byte after ulen must be untouched (no write-out overrun). */
    CHECK_EQ((long)(unsigned)g_buf[size], (long)TSTUBUF_GUARD,
             "guard byte after ulen UNTOUCHED (no overrun past ulen)");
    return dataok;
}

int
main(void)
{
    wtof("TSTUBUF: UBUF XFER CLIENT START (SVC %u, CHUNK %u)",
         (unsigned)NSFV_SVCNUM, (unsigned)NSFV_XFER_CHUNK);

    /* THE RED LINE: the client must be UNAUTHORIZED and must not self-auth.
    ** __isauth() is TESTAUTH FCTN=1: it returns 0 when NOT APF-authorized. */
    CHECK_EQ((long)__isauth(), 0L,
             "client is UNAUTHORIZED (TESTAUTH FCTN=1) and does not self-auth");

    /* Sizes: 0 (zero-length guard), 1, < chunk, exactly one chunk, multi-chunk;
    ** then a short transfer AFTER the large one, so "no write past ulen" is
    ** meaningful against stale staging (truncation, ADR-0039 4). */
    wtof("TSTUBUF: size 0");     (void)run_xfer(0u);
    wtof("TSTUBUF: size 1");     (void)run_xfer(1u);
    wtof("TSTUBUF: size 100");   (void)run_xfer(100u);
    wtof("TSTUBUF: size 2048");  (void)run_xfer(NSFV_XFER_CHUNK);
    wtof("TSTUBUF: size 5000");  (void)run_xfer(5000u);
    wtof("TSTUBUF: size 10 (short after large -- truncation)");
    (void)run_xfer(10u);

    wtof("TSTUBUF: UBUF XFER CLIENT DONE");

    return mbt_test_summary("TSTUBUF");
}
