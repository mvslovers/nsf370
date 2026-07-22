/*
 * tstmvck.c -- M5 Stage-0b Step 1: the MVCK (Move with Key) probe.
 *
 * ADR-0039 Step 1 / ADR-0038 open promise.  MVS-only (host = false): MVCK and
 * storage keys have no host analog.  Settles empirically the question ADR-0038's
 * countersign annotation left open -- MVCP/MVCS are DAS instructions absent on
 * base S/370 (ADR-0036: no cross-memory services), so the keyed move for the
 * M5-2 ubuf transfer must be the non-DAS MVCK (Move with Key, D9).
 *
 * Two things to prove, before the ubuf work rides on MVCK:
 *   1. does MVCK EXIST + EXECUTE here (MVS 3.8j / Hercules) and copy byte-exact?
 *      -- a supervisor/key-0 MVCK reading a key-8 source under R3=8 (the SVC
 *      routine's write-in), verified byte-exact + length-honouring.
 *   2. does MVCK DECODE + ENFORCE the source key R3, or silently ignore it?
 *      -- a foreign source key (R3=0, the master key, from problem state) is a
 *      privileged operation and FAULTS (S0C2); if R3 were ignored the move would
 *      quietly proceed under the PSW key.  This is the "MVCK is not a silent
 *      key-0 clobber" proof, and it is DAT-independent.
 *   3. the SUPERVISOR routine's real hostile-pointer case -- a key-8 MVCK-read
 *      (R3=8) of a genuinely FETCH-PROTECTED caller buffer takes a PROTECTION
 *      exception (S0C4), not a silent key-0 clobber.  The frame's REAL page is
 *      SSK'd to key 0 + fetch-protect: under MVS DAT storage keys are on REAL
 *      frames, so LRA translates the virtual test page to its real address, SSK
 *      fetch-protects it (ISK confirms), and the supervisor MVCK then faults.
 *
 * libc370 `try()` runs the faulting move under ESTAE and returns the abend code
 * (0x00sssuuu) with NO dump -- so the fault IS the proof, cleanly captured.
 *
 * A MECHANISM probe: it self-authorises (SVC 244) because MVCK runs in the SVC
 * routine's supervisor/key-0 context.  It is NOT the ubuf client -- that (Step 2)
 * stays UNAUTHORIZED (ADR-0038 red line).  No anchor, no SVC steal, no ubuf.
 */
#include <clibos.h>         /* __super/__prob/clib_apf_setup                    */
#include <clibtry.h>        /* try() -- run under ESTAE, return the abend code  */
#include <clibwto.h>        /* wtof                                            */
#include <mbtcheck.h>
#include <string.h>

/* MVCK dst <- src, len bytes (1..256), source accessed under key `srckey`.
 * MVCK D1(R1,B1),D2(B2),R3: R1 = length (bits 24-31), R3 = source key (bits
 * 24-27), dst under the PSW key.  This is the M5-2 write-in copy primitive. */
static void
mvck_move(void *dst, const void *src, unsigned len, unsigned srckey)
{
    unsigned r1 = len & 0xFFu;              /* MVCK length (bits 24-31 of R1)  */
    unsigned r3 = (srckey & 0xFu) << 4;     /* source key in bits 24-27 of R3  */

    /* as370 mis-assembles the MVCK mnemonic (it drops the R1-length register
     * and the R3-key register, encoding them as 0 -- verified in the listing),
     * so emit the raw D9 opcode: D9 R1R3 B1D1 B2D2 with fixed registers
     * R1=1 (length), R3=3 (key), B1=4 (dst base), B2=5 (src base), D1=D2=0. */
    __asm__ __volatile__(
        "  LR    1,%0\n"                    /* R1 = length                     */
        "  LR    3,%1\n"                    /* R3 = source key                 */
        "  LR    4,%2\n"                    /* R4 = dst base (B1)              */
        "  LR    5,%3\n"                    /* R5 = src base (B2)             */
        "  DC    X'D9134000'\n"             /* MVCK 0(1,4),0(5),3  bytes 0-3   */
        "  DC    X'5000'\n"                 /* ...                 bytes 4-5   */
        :
        : "r"(r1), "r"(r3), "r"(dst), "r"(src)
        : "1", "3", "4", "5", "memory", "cc");
}

/* SSK (privileged): set the storage key of the frame at `real` to `keybyte`
 * (bits 0-3 key, bit 4 fetch-protect).  Under MVS DAT, SSK operates on REAL
 * addresses -- pass an LRA-translated real address, not a virtual one.  as370
 * lacks the SSK mnemonic, so emit the raw RR opcode X'08' (R1=1,R2=2 -> X'0812').*/
static void
ssk_set(void *real, unsigned keybyte)
{
    __asm__ __volatile__(
        "  LR    1,%0\n"
        "  LR    2,%1\n"
        "  DC    X'0812'\n"                 /* SSK 1,2                         */
        :
        : "r"(keybyte), "r"(real)
        : "1", "2", "cc");
}

/* LRA (privileged): translate a virtual address to its real address; returns 0
 * if the page is not translation-available (LRA CC != 0).  Named-label asm ->
 * noinline (one definition).  Caller must be supervisor. */
static void * __attribute__((noinline))
lra_real(void *virt)
{
    unsigned real;
    __asm__ __volatile__(
        "  LR    4,%1\n"
        "  SLR   3,3\n"                     /* default 0 (not resident)        */
        "  LRA   3,0(0,4)\n"                /* R3 = real addr; CC 0 if valid   */
        "  BC    8,MVCKLRA1\n"              /* CC 0: keep R3                   */
        "  SLR   3,3\n"                     /* else R3 = 0                     */
        "MVCKLRA1 DS 0H\n"
        "  LR    %0,3\n"
        : "=r"(real)
        : "r"(virt)
        : "3", "4", "cc");
    return (void *)real;
}

/* --- test bodies, run under libc370 try() --------------------------------- */
static char   g_src[256];
static char   g_dst[256];
static char   g_raw[8192];     /* holds a 2K-aligned frame for the S0C4 test   */
static void  *g_fp_frame;      /* virtual 2K-aligned frame                     */
static void  *g_fp_real;       /* its real address (SSK'd to key 0 + FP)       */
static char   g_fp_dst[16];

/* Positive: the write-in scenario -- supervisor/key 0, MVCK a key-8 source
 * (R3=8) into a dst.  Must NOT fault; __prob restores state on the clean path. */
static int
t_positive(void)
{
    unsigned char savekey;
    if (__super(PSWKEY0, &savekey)) return 99;
    mvck_move(g_dst, g_src, 200u, 8u);
    __prob(savekey, NULL);
    return 0;
}

/* Negative: MVCK with a source key the program is NOT permitted to use -- from
 * problem state, key 0 in R3 (the privileged master key) -> a privileged-
 * operation exception (S0C2).  This proves MVCK DECODES and ENFORCES R3 (a
 * foreign key is denied, not silently used under the PSW key -- which is exactly
 * what stops an unauthorized caller misusing MVCK).  It is DAT-independent. */
static int
t_foreign_key(void)
{
    mvck_move(g_dst, g_src, 16u, 0u);       /* R3=0 in problem state -> S0C2   */
    return 0;                               /* not reached: MVCK faults        */
}

/* Negative (3): the SUPERVISOR routine's real hostile-pointer case -- a key-8
 * MVCK-read (R3=8) of a genuinely FETCH-PROTECTED caller buffer takes a
 * PROTECTION exception (S0C4), NOT a silent key-0 clobber.  The frame's REAL
 * page is SSK'd to key 0 + fetch-protect (via LRA), so a key-8 fetch is denied.
 * Runs in supervisor (the routine's context; a foreign key there is not the
 * problem-state privileged case).  Faults before __prob; the caller forces the
 * state afterward (jmp_buf saves no PSW). */
static int
t_read_fp_super(void)
{
    unsigned char sk;
    if (__super(PSWKEY0, &sk)) return 99;
    mvck_move(g_fp_dst, g_fp_frame, 16u, 8u);
    __prob(sk, NULL);                       /* not reached: MVCK faults        */
    return 0;
}

int
main(void)
{
    unsigned char savekey;
    unsigned      rc;
    unsigned      i;

    wtof("TSTMVCK: MVCK PROBE START");

    CHECK_EQ((long)clib_apf_setup("TSTMVCK"), 0L,
             "self-auth (SVC 244) -- MVCK runs in the SVC routine's key-0 window");

    /* ---- (1) MVCK exists, executes, copies byte-exact (keyed, R3=8) ---- */
    for (i = 0u; i < 256u; i++) g_src[i] = (char)(i ^ 0x5Au);
    memset(g_dst, 0, sizeof g_dst);

    rc = (unsigned)___try(t_positive);
    CHECK_EQ((long)rc, 0L, "MVCK executes with no abend (exists on target)");
    CHECK(memcmp(g_dst, g_src, 200u) == 0, "MVCK copies 200 bytes byte-exact");
    CHECK_EQ((long)g_dst[200], 0L, "MVCK honours the length (byte 200 untouched)");

    /* ---- (2) MVCK decodes + enforces R3: a foreign source key faults ---- */
    (void)savekey;
    rc = (unsigned)___try(t_foreign_key);   /* R3=0 in problem state, no dump  */
    CHECK(rc != 0u,
          "MVCK with a foreign source key FAULTS (R3 is enforced, not ignored)");
    CHECK_EQ((long)((rc >> 12) & 0xFFFu), (long)0x0C2,
             "the fault is a privileged-op exception (S0C2) -- foreign key denied");

    /* ---- (3) supervisor storage-key protection: a key-8 MVCK of a fetch-
     ** protected frame -> S0C4.  Fetch-protect the frame's REAL page (SSK on the
     ** LRA-translated real address; storage keys are per-2K frame). ---- */
    g_fp_frame = (void *)(((unsigned)(void *)g_raw + 0x7FFu) & ~0x7FFu);
    *(volatile char *)g_fp_frame = 0x11;    /* touch -> resident for LRA        */
    g_fp_real = NULL;
    if (__super(PSWKEY0, &savekey) == 0) {
        unsigned keyback = 0xFFu;
        g_fp_real = lra_real(g_fp_frame);
        if (g_fp_real) {
            ssk_set(g_fp_real, 0x08u);      /* real frame key 0 + FP           */
            /* ISK 3,4 (X'0934') read the key back to confirm SSK took. */
            __asm__ __volatile__("  LR 4,%1\n  DC X'0934'\n  LR %0,3\n"
                : "=r"(keyback) : "r"(g_fp_real) : "3", "4");
        }
        __prob(savekey, NULL);              /* ESTAE below is set in prob state */
        wtof("TSTMVCK: FP setup virt=%08X real=%08X keyback=%02X",
             (unsigned)g_fp_frame, (unsigned)g_fp_real, keyback & 0xFFu);
    }
    if (g_fp_real) {
        rc = (unsigned)___try(t_read_fp_super);     /* supervisor MVCK -> S0C4  */
        /* force key 0, restore the real frame key, drop to problem state. */
        __super(PSWKEY0, &savekey);
        ssk_set(g_fp_real, 0x80u);
        __prob(0x80u, NULL);
        CHECK(rc != 0u,
              "supervisor MVCK of a fetch-protected frame FAULTS (not a clobber)");
        CHECK_EQ((long)((rc >> 12) & 0xFFFu), (long)0x0C4,
                 "the fault is a PROTECTION exception (S0C4) -- key enforced live");
    } else {
        wtof("TSTMVCK: LRA not resident -- S0C4 case skipped");
    }

    wtof("TSTMVCK: MVCK PROBE DONE (rc=%06X)", rc);

    return mbt_test_summary("TSTMVCK");
}
