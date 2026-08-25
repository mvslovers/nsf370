/*
 * tstmvcd.c -- M5-2b Step 0: the DESTINATION-KEY probe.
 *
 * ADR-0039 instructed that the write-out key window "opens with an empirical
 * probe"; ADR-0041's second correction added that there are now TWO key-0
 * destinations (`ubuf` and `rqeimg`), not one.  This probe ANSWERS QUESTIONS
 * AND FIXES NOTHING: no production code, no anchor change, no SVC steal.  It is
 * the Stage-0-style isolated measuring step that `test/mvs/tstmvck.c` was for
 * Stage-0b, and it is modelled on that file throughout.
 *
 * MVS-only (host = false): SSE opcodes, PSW keys, storage keys, SSK/ISK/LRA and
 * the CSA control blocks have no host analog.
 *
 * Three questions, in order:
 *
 *   1. Does a DESTINATION-KEYED MOVE exist on this target?  MVCK (Stage-0b)
 *      keys only its SOURCE, which is why the write-out is still a key-0 store
 *      the hardware cannot check.  MVCDK (Move with Destination Key) would be
 *      the direct answer, and MVCSK comes free in the same probe -- the pair
 *      answers "is this whole SSE family absent" rather than "is this one
 *      opcode absent".
 *
 *      RECORDED PREDICTION (written before the run, so the run can falsify it):
 *      MVCDK and MVCSK are 370-XA/390, NOT base S/370, and are NOT there --
 *      the same shape of answer ADR-0039 got for MVCP/MVCS.  If the run
 *      disagrees, the run wins.
 *
 *   2. Does an SPKA window actually close the hole -- and can it still read the
 *      key-0 CSA source?  Availability is not protection: the load-bearing
 *      check is (2), a key-8 store into key-0 storage, which MUST fault.
 *
 *   3. What is the CSA budget on MVSCE?  Sizing input for M5-2b3, collected
 *      here because this step is already a measuring step.
 *
 * A MECHANISM probe: it SELF-AUTHORISES (SVC 244), exactly like tstmvck.c,
 * because SPKA/SSK/ISK/LRA are privileged and the window being probed lives in
 * the SVC routine's supervisor context.  It is NOT a client, so the
 * UNAUTHORISED-CLIENT RED LINE (ADR-0038) does not apply to it -- that red line
 * governs the client (TSTUBUF/TSTDEATH/TSTRQXM), which never self-authorises.
 * This probe needs NO probe STC: there is no anchor, no SVC and no rendezvous.
 *
 * libc370 try() runs each faulting sequence under ESTAE and returns the abend
 * code (0x00sssuuu) with NO dump -- so a fault IS the evidence, cleanly caught.
 */
#include <clibos.h>         /* __super/__prob/clib_apf_setup/getmain/freemain  */
#include <clibtry.h>        /* ___try() -- run under ESTAE, return abend code  */
#include <clibwto.h>        /* wtof                                            */
#include <cvt.h>            /* CVT, CVTPTR -- the CSA budget walk              */
#include <mbtcheck.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Encoding derivation -- PRIMARY SOURCE, not the briefed hypothesis.
 *
 * The brief hypothesised MVCSK = B20E / MVCDK = B20F with the byte count in
 * R1 and the key in R0.  The machine says otherwise.  Hercules is the machine
 * here (MVSCE runs under it, `local.cnf`: ARCHMODE S/370), and its opcode
 * table + instruction bodies are the reference this probe follows:
 *
 *   opcode.c   E50E:  GENx___x390x900 ( "MVCSK", SSE, ... )
 *   opcode.c   E50F:  GENx___x390x900 ( "MVCDK", SSE, ... )
 *
 * So the opcodes are E50E / E50F (not B20E / B20F), and the GENx___x390x900
 * gating means the S/370 slot of each is `operation_exception` -- which is the
 * prediction above, reached from primary source rather than from memory.
 *
 *   control.c  move_with_destination_key():
 *       l = regs->GR_L( 0 ) & 0xFF;   -- "operand length-1 from register 0"
 *       k = regs->GR_L( 1 ) & 0xF0;   -- "destination key from register 1"
 *
 * So R0 bits 24-31 = LENGTH MINUS ONE and R1 bits 24-27 = the key -- the
 * opposite register roles to the hypothesis, and a length that is biased by
 * one.  MVCSK (move_with_source_key) uses the identical register convention.
 *
 * SSE format is opcode(2 bytes) | B1 D1 (halfword) | B2 D2 (halfword), so with
 * B1 = 4 (destination base), D1 = 0, B2 = 5 (source base), D2 = 0:
 *
 *       MVCDK 0(4),0(5)   ->   E5 0F 40 00 50 00
 *       MVCSK 0(4),0(5)   ->   E5 0E 40 00 50 00
 *
 * These are emitted as RAW BYTES, and not because the mnemonic misbehaves:
 * as370 does not know either mnemonic at all, and cannot express them -- its
 * instruction formats are RR/RX/RS/SI/SS plus a three-entry S-format list
 * (IPK/SPKA/STCK).  There is no SSE format in as370.  The `as370 -a=` listing
 * is the gate on the bytes below, exactly as it was for MVCK's raw D9.
 * ------------------------------------------------------------------ */

/* MVCDK dst <- src, `len` bytes, DESTINATION accessed under `key`. */
static void
mvcdk_move(void *dst, const void *src, unsigned len, unsigned key)
{
    unsigned r0 = (len - 1u) & 0xFFu;       /* length MINUS ONE (bits 24-31)  */
    unsigned r1 = (key & 0xFu) << 4;        /* key in bits 24-27              */

    __asm__ __volatile__(
        "  LR    0,%0\n"
        "  LR    1,%1\n"
        "  LR    4,%2\n"
        "  LR    5,%3\n"
        "  DC    X'E50F4000',X'5000'\n"
        :
        : "r"(r0), "r"(r1), "r"(dst), "r"(src)
        : "0", "1", "4", "5", "memory", "cc");
}

/* MVCSK dst <- src, `len` bytes, SOURCE accessed under `key`.  Same register
 * convention as MVCDK; probed only to tell "this opcode is missing" from "this
 * whole SSE family is missing". */
static void
mvcsk_move(void *dst, const void *src, unsigned len, unsigned key)
{
    unsigned r0 = (len - 1u) & 0xFFu;
    unsigned r1 = (key & 0xFu) << 4;

    __asm__ __volatile__(
        "  LR    0,%0\n"
        "  LR    1,%1\n"
        "  LR    4,%2\n"
        "  LR    5,%3\n"
        "  DC    X'E50E4000',X'5000'\n"
        :
        : "r"(r0), "r"(r1), "r"(dst), "r"(src)
        : "0", "1", "4", "5", "memory", "cc");
}

/* SPKA 0(2) -- set the PSW key from bits 24-27 of the second-operand address.
 * PRIVILEGED: supervisor state only.  `keybyte` is the key in the HIGH nibble
 * (0x80 = key 8, 0x00 = key 0), the libc370 __super/__prob convention.  The
 * "memory" clobber is load-bearing: without it the compiler may move the
 * probed store across the key change, which would silently invalidate the
 * whole window. */
static void
spka_set(unsigned keybyte)
{
    __asm__ __volatile__(
        "  LR    2,%0\n"
        "  SPKA  0(2)\n"
        :
        : "r"(keybyte)
        : "2", "memory", "cc");
}

/* IPK -- the current PSW key into R2 bits 24-27 (bits 28-31 zeroed, 0-23
 * unchanged), so mask to a byte.  0x80 == key 8.  Reading it INSIDE the window
 * is what distinguishes "the key window is not protecting" from "SPKA never
 * took" -- without it, a non-faulting store is ambiguous. */
static unsigned
ipk_read(void)
{
    unsigned k;

    __asm__ __volatile__(
        "  IPK   0\n"
        "  LR    %0,2\n"
        : "=r"(k)
        :
        : "2", "memory", "cc");
    return k & 0xFFu;
}

/* SSK (privileged, raw RR X'08', R1=1 R2=2 -> X'0812'): set the storage key of
 * the REAL frame at `real` to `keybyte`.  Storage-key bits: 0-3 key, 4 fetch
 * protect, 5 reference, 6 change.  as370 has no SSK mnemonic (tstmvck.c). */
static void
ssk_set(void *real, unsigned keybyte)
{
    __asm__ __volatile__(
        "  LR    1,%0\n"
        "  LR    2,%1\n"
        "  DC    X'0812'\n"
        :
        : "r"(keybyte), "r"(real)
        : "1", "2", "cc");
}

/* ISK (privileged, raw RR X'09', R1=3 R2=4 -> X'0934'): read the storage key
 * of the REAL frame at `real`.  KEY = byte & 0xF0, FETCH-PROTECT = byte & 0x08
 * (so 0x80 is key 8 unprotected and 0x08 is key 0 fetch-protected -- the two
 * values tstmvck.c uses).  Pinned here so check (3) cannot be read inverted. */
static unsigned
isk_read(void *real)
{
    unsigned k;

    __asm__ __volatile__(
        "  LR    4,%1\n"
        "  DC    X'0934'\n"
        "  LR    %0,3\n"
        : "=r"(k)
        : "r"(real)
        : "3", "4", "cc");
    return k & 0xFFu;
}

/* LRA (privileged): virtual -> real, 0 when not translation-available.  Named
 * asm label -> noinline (one definition).  Verbatim from tstmvck.c. */
static void * __attribute__((noinline))
lra_real(void *virt)
{
    unsigned real;

    __asm__ __volatile__(
        "  LR    4,%1\n"
        "  SLR   3,3\n"
        "  LRA   3,0(0,4)\n"
        "  BC    8,MVCDLRA1\n"
        "  SLR   3,3\n"
        "MVCDLRA1 DS 0H\n"
        "  LR    %0,3\n"
        : "=r"(real)
        : "r"(virt)
        : "3", "4", "cc");
    return (void *)real;
}

/* --- shared state ---------------------------------------------------- */

#define PROBE_PAT   0x5A5A5A5Au             /* what a probed store writes   */
#define PROBE_SEED  0xC3D4E5F6u             /* what the CSA source holds    */

static volatile unsigned  g_win_key;        /* IPK reading INSIDE the window */
static volatile unsigned  g_fetched;        /* what check (3) read back      */
static unsigned           g_k8_dst[8];      /* our own key-8 destination     */
static unsigned           g_mv_src[8];      /* MVCDK/MVCSK source            */
static unsigned           g_mv_dst[8];      /* MVCDK/MVCSK destination       */
static char               g_raw[8192];      /* holds a 2K-aligned test frame */
static void              *g_k0_frame;       /* that frame, virtual           */
static void              *g_k0_real;        /* that frame, real (SSK'd key 0)*/
static unsigned           g_k0_origkey;     /* its key before we touched it  */
static void              *g_csa;            /* key-0 CSA block (SP=241)      */
static void              *g_csa_real;       /* its real address (for ISK)    */

/* Store `n` fullwords -- the probed access.  A store is a store as far as
 * key-controlled protection is concerned; using a plain store rather than a
 * library move keeps the faulting instruction unambiguous. */
static void
store_words(void *p, unsigned v, unsigned n)
{
    volatile unsigned *q = (volatile unsigned *)p;
    unsigned           i;

    for (i = 0u; i < n; i++) q[i] = v;
}

/* Force a deterministic state after a try() -- the retry does not restore the
 * PSW for us (jmp_buf saves no PSW), so every path lands here BEFORE any CHECK
 * or printf runs: super/key 0 first, then problem state key 8. */
static void
force_state(void)
{
    unsigned char sk;

    __super(PSWKEY0, &sk);
    __prob(PSWKEY8, NULL);
}

/* --- (1) does a destination-keyed move exist? ------------------------ */

/* Run in SUPERVISOR state deliberately: in problem state a real MVCDK/MVCSK
 * would first take a PRIVILEGED-OPERATION exception on the CR3 key mask, which
 * would be indistinguishable in shape from "absent".  In supervisor state the
 * only expected failure is the OPERATION exception, so S0C1 means "not
 * installed" and nothing else. */
static int
t_mvcdk(void)
{
    unsigned char sk;

    if (__super(PSWKEY0, &sk)) return 99;
    mvcdk_move(g_mv_dst, g_mv_src, 16u, 8u);
    __prob(sk, NULL);
    return 0;
}

static int
t_mvcsk(void)
{
    unsigned char sk;

    if (__super(PSWKEY0, &sk)) return 99;
    mvcsk_move(g_mv_dst, g_mv_src, 16u, 8u);
    __prob(sk, NULL);
    return 0;
}

/* --- (2) does an SPKA window close the hole? -------------------------- */

/* (2.1) baseline: key 8 storing into key-8 storage succeeds, and IPK inside
 * the window proves the key really changed. */
static int
t_store_key8(void)
{
    unsigned char sk;

    if (__super(PSWKEY0, &sk)) return 99;
    spka_set(0x80u);
    g_win_key = ipk_read();
    store_words(g_k8_dst, PROBE_PAT, 4u);
    spka_set(0x00u);
    __prob(sk, NULL);
    return 0;
}

/* (2.2a) THE check that matters: key 8 storing into key-0 CSA -- the exact
 * storage class the STC's anchor/staging live in (getmain SP=241 taken in key
 * 0, the production pattern).  MUST fault S0C4. */
static int
t_store_csa(void)
{
    unsigned char sk;

    if (__super(PSWKEY0, &sk)) return 99;
    spka_set(0x80u);
    g_win_key = ipk_read();
    store_words(g_csa, PROBE_PAT, 4u);      /* expected to fault here          */
    spka_set(0x00u);
    __prob(sk, NULL);
    return 0;
}

/* (2.2b) the same question asked of a SECOND, independent key-0 destination:
 * one of our own frames, SSK'd to key 0 and ISK-confirmed.  Two independent
 * readings are what make this a measurement rather than an impression -- and
 * they make a non-fault impossible to misattribute to the subpool's key. */
static int
t_store_k0frame(void)
{
    unsigned char sk;

    if (__super(PSWKEY0, &sk)) return 99;
    spka_set(0x80u);
    g_win_key = ipk_read();
    store_words(g_k0_frame, PROBE_PAT, 4u); /* expected to fault here          */
    spka_set(0x00u);
    __prob(sk, NULL);
    return 0;
}

/* (2.3) can the window still READ the key-0 CSA source?  Succeeds only if the
 * block is not fetch-protected -- which decides whether a fix of this shape is
 * an SPKA around the move or has to bounce through a key-8 landing area. */
static int
t_fetch_csa(void)
{
    unsigned char sk;

    if (__super(PSWKEY0, &sk)) return 99;
    spka_set(0x80u);
    g_win_key = ipk_read();
    g_fetched = *(volatile unsigned *)g_csa;
    spka_set(0x00u);
    __prob(sk, NULL);
    return 0;
}

/* --- helpers -------------------------------------------------------- */

/* Report a try() result three ways, never two.  ___try returns NEGATIVE when
 * the ESTAE CREATE itself failed, which is INCONCLUSIVE, not a fault. */
static int
inconclusive(int rc, const char *what)
{
    if (rc < 0) {
        wtof("TSTMVCD: %s INCONCLUSIVE -- ESTAE CREATE failed rc=%d", what, rc);
        return 1;
    }
    return 0;
}

static unsigned
abend_sys(int rc)
{
    return ((unsigned)rc >> 12) & 0xFFFu;
}

int
main(void)
{
    unsigned char sk;
    CVT          *cvt;
    unsigned      gda, pqe, first_regn, first_size, csa_total, hops;
    unsigned      largest, size, gdaoff;
    unsigned      csakey, k0key, i;
    int           rc, fp_protected;
    void         *p;

    wtof("TSTMVCD: DESTINATION-KEY PROBE START");
    printf("=== TSTMVCD -- M5-2b step 0: the destination-key probe ===\n");

    CHECK_EQ((long)clib_apf_setup("TSTMVCD"), 0L,
             "self-auth (SVC 244) -- SPKA/SSK/ISK/LRA are privileged");

    for (i = 0u; i < 8u; i++) { g_mv_src[i] = 0x11223344u + i; g_mv_dst[i] = 0u; }

    /* ================================================================
     * (1) does a DESTINATION-KEYED MOVE exist?
     * ================================================================ */
    rc = ___try(t_mvcdk);
    force_state();
    printf("  MVCDK (E50F): try rc=%08X\n", (unsigned)rc);
    if (!inconclusive(rc, "MVCDK")) {
        if (rc == 0) {
            wtof("TSTMVCD: MVCDK EXECUTED -- IT EXISTS.  STOP AND REPORT.");
            CHECK(0, "MVCDK is absent (prediction) -- IT EXECUTED: stop and report");
        } else {
            CHECK(1, "MVCDK does not execute (prediction held: not on base S/370)");
            CHECK_EQ((long)abend_sys(rc), (long)0x0C1,
                     "MVCDK fault is an OPERATION exception (S0C1) -- opcode absent");
        }
    }

    rc = ___try(t_mvcsk);
    force_state();
    printf("  MVCSK (E50E): try rc=%08X\n", (unsigned)rc);
    if (!inconclusive(rc, "MVCSK")) {
        if (rc == 0) {
            wtof("TSTMVCD: MVCSK EXECUTED -- IT EXISTS.  STOP AND REPORT.");
            CHECK(0, "MVCSK is absent (prediction) -- IT EXECUTED: stop and report");
        } else {
            CHECK(1, "MVCSK does not execute (the whole SSE pair is absent)");
            CHECK_EQ((long)abend_sys(rc), (long)0x0C1,
                     "MVCSK fault is an OPERATION exception (S0C1) -- opcode absent");
        }
    }
    CHECK_EQ((long)g_mv_dst[0], 0L,
             "neither move touched its destination (no partial execution)");

    /* ================================================================
     * (2) does an SPKA window close the hole?
     * Allocate the key-0 CSA block first: it is BOTH the key-0 store target
     * of (2.2a) and the key-0 fetch source of (2.3).  Kept small and freed in
     * exactly one place; every faulting sequence between here and the free
     * runs under try(), so no path leaks it.
     * ================================================================ */
    g_csa = NULL;
    g_csa_real = NULL;
    csakey = 0xFFu;
    if (__super(PSWKEY0, &sk) == 0) {
        g_csa = getmain(256u, 241u);        /* the production anchor pattern */
        if (g_csa) {
            store_words(g_csa, PROBE_SEED, 4u);
            g_csa_real = lra_real(g_csa);
            if (g_csa_real) csakey = isk_read(g_csa_real);
        }
        __prob(sk, NULL);
    }
    force_state();
    CHECK(g_csa != NULL, "GETMAIN SP=241 (key 0) for the key-0 probe target");
    printf("  CSA probe block: virt=%08X real=%08X storage key byte=%02X\n",
           (unsigned)g_csa, (unsigned)g_csa_real, csakey);
    wtof("TSTMVCD: CSA blk virt=%08X real=%08X key=%02X",
         (unsigned)g_csa, (unsigned)g_csa_real, csakey);
    CHECK_EQ((long)(csakey & 0xF0u), 0L,
             "the SP=241 block really is KEY 0 (ISK, key = byte & F0)");
    fp_protected = (csakey & 0x08u) ? 1 : 0;
    printf("  CSA fetch-protect bit (byte & 08) = %d -> %s\n", fp_protected,
           fp_protected ? "FETCH-PROTECTED" : "not fetch-protected");

    /* (2.1) baseline. */
    g_win_key = 0xFFu;
    for (i = 0u; i < 8u; i++) g_k8_dst[i] = 0u;
    rc = ___try(t_store_key8);
    force_state();
    if (!inconclusive(rc, "SPKA key-8 baseline")) {
        CHECK_EQ((long)rc, 0L, "key-8 store into KEY-8 storage succeeds (baseline)");
        CHECK_EQ((long)(g_win_key & 0xF0u), (long)0x80,
                 "SPKA took: IPK inside the window reads key 8");
        CHECK_EQ((long)g_k8_dst[0], (long)PROBE_PAT, "the key-8 store landed");
    }

    /* (2.2a) the load-bearing check, destination 1: key-0 CSA. */
    g_win_key = 0xFFu;
    rc = ___try(t_store_csa);
    force_state();
    printf("  key-8 store into key-0 CSA: try rc=%08X (window key=%02X)\n",
           (unsigned)rc, g_win_key);
    if (!inconclusive(rc, "key-8 store into key-0 CSA")) {
        CHECK_EQ((long)(g_win_key & 0xF0u), (long)0x80,
                 "the window was really key 8 for the key-0 CSA store");
        if (rc == 0) {
            wtof("TSTMVCD: KEY-8 STORE INTO KEY-0 CSA SUCCEEDED -- STOP AND REPORT");
            CHECK(0, "key-8 store into KEY-0 CSA must FAULT -- IT SUCCEEDED");
        } else {
            CHECK(1, "key-8 store into KEY-0 CSA faults (the window closes the hole)");
            CHECK_EQ((long)abend_sys(rc), (long)0x0C4,
                     "the fault is a PROTECTION exception (S0C4)");
        }
    }

    /* (2.2b) the same question, destination 2: one of our own frames SSK'd to
     * key 0.  SSK 0x00 only -- store protection needs no fetch-protect bit,
     * and setting one would conflate the two properties. */
    g_k0_frame = (void *)(((unsigned)(void *)g_raw + 0x7FFu) & ~0x7FFu);
    *(volatile char *)g_k0_frame = 0x11;    /* touch -> resident for LRA      */
    g_k0_real = NULL;
    g_k0_origkey = 0xFFu;
    k0key = 0xFFu;
    if (__super(PSWKEY0, &sk) == 0) {
        g_k0_real = lra_real(g_k0_frame);
        if (g_k0_real) {
            g_k0_origkey = isk_read(g_k0_real);
            ssk_set(g_k0_real, 0x00u);
            k0key = isk_read(g_k0_real);
        }
        __prob(sk, NULL);
    }
    force_state();
    printf("  own frame: virt=%08X real=%08X origkey=%02X nowkey=%02X\n",
           (unsigned)g_k0_frame, (unsigned)g_k0_real, g_k0_origkey, k0key);
    if (g_k0_real) {
        CHECK_EQ((long)(k0key & 0xF0u), 0L, "the own frame is now KEY 0 (ISK)");
        g_win_key = 0xFFu;
        rc = ___try(t_store_k0frame);
        /* restore state AND the frame key BEFORE any CHECK/printf runs. */
        __super(PSWKEY0, &sk);
        ssk_set(g_k0_real, g_k0_origkey);
        __prob(PSWKEY8, NULL);
        printf("  key-8 store into key-0 own frame: try rc=%08X (window key=%02X)\n",
               (unsigned)rc, g_win_key);
        if (!inconclusive(rc, "key-8 store into key-0 own frame")) {
            CHECK_EQ((long)(g_win_key & 0xF0u), (long)0x80,
                     "the window was really key 8 for the own-frame store");
            if (rc == 0) {
                wtof("TSTMVCD: KEY-8 STORE INTO A KEY-0 FRAME SUCCEEDED -- STOP");
                CHECK(0, "key-8 store into a KEY-0 frame must FAULT -- IT SUCCEEDED");
            } else {
                CHECK(1, "key-8 store into a KEY-0 frame faults (second reading)");
                CHECK_EQ((long)abend_sys(rc), (long)0x0C4,
                         "the second reading also faults S0C4 (protection)");
            }
        }
    } else {
        wtof("TSTMVCD: LRA not resident -- own-frame reading skipped");
        printf("  own-frame reading SKIPPED (LRA not resident)\n");
    }

    /* (2.3) can the window still read the key-0 CSA source? */
    g_win_key = 0xFFu;
    g_fetched = 0u;
    rc = ___try(t_fetch_csa);
    force_state();
    printf("  key-8 fetch from key-0 CSA: try rc=%08X value=%08X\n",
           (unsigned)rc, g_fetched);
    if (!inconclusive(rc, "key-8 fetch from key-0 CSA")) {
        CHECK_EQ((long)(g_win_key & 0xF0u), (long)0x80,
                 "the window was really key 8 for the key-0 CSA fetch");
        if (fp_protected) {
            CHECK(rc != 0, "CSA IS fetch-protected -> the key-8 fetch faults");
            CHECK_EQ((long)abend_sys(rc), (long)0x0C4,
                     "the fetch fault is a PROTECTION exception (S0C4)");
        } else {
            CHECK_EQ((long)rc, 0L,
                     "CSA is NOT fetch-protected -> the key-8 fetch succeeds");
            CHECK_EQ((long)g_fetched, (long)PROBE_SEED,
                     "the key-8 fetch read the key-0 CSA source byte-exact");
        }
        CHECK(((rc == 0) && !fp_protected) || ((rc != 0) && fp_protected),
              "behaviour and the ISK fetch-protect bit AGREE");
    }

    /* free the CSA probe block -- one place, all paths. */
    if (g_csa) {
        if (__super(PSWKEY0, &sk) == 0) {
            freemain(g_csa);
            __prob(sk, NULL);
        }
        force_state();
        g_csa = NULL;
    }

    /* ================================================================
     * (3) the CSA budget.
     *
     * CORRECTION to the brief: the GDA has NO CSA size field.  The MVS 3.8j
     * mapping (SYS1.AMODGEN(IHAGDA), read on the live system) is
     *     GDA+0 GVSMFLAG/GDAFLAGS+RESV, +4 VRDREG, +8 CSAPQEP, ...
     * so the CSA size is reached one hop further, through the PQE
     * (IHAPQE: +0 PQEFFBQE, +4 PQEBFBQE, +8 PQEFPQE, +12 PQEBPQE, +16 PQETCB,
     *  +20 PQESIZE, +24 PQEREGN).  The chain used here is therefore
     *     CVT -> CVTGDA (CVT+X'230') -> GDA+8 CSAPQEP -> PQE+20 PQESIZE
     *                                                 -> PQE+24 PQEREGN
     * and the PQE chain (PQE+8 PQEFPQE) is walked bounded, because more than
     * one PQE may describe the region.
     * ================================================================ */
    cvt   = CVTPTR;
    gdaoff = cvt ? (unsigned)((char *)&cvt->cvtgda - (char *)cvt) : 0u;
    gda   = cvt ? (unsigned)cvt->cvtgda : 0u;
    pqe   = gda ? (*(unsigned *)(gda + 8u) & 0x00FFFFFFu) : 0u;
    CHECK(cvt != NULL, "CVT reachable");
    CHECK_EQ((long)gdaoff, (long)0x230, "CVTGDA is at CVT+X'230' (cvt.h mapping)");
    CHECK(gda != 0u, "CVTGDA -> GDA");
    CHECK(pqe != 0u, "GDA+8 (CSAPQEP) -> CSA PQE");

    first_regn = 0u;
    first_size = 0u;
    csa_total  = 0u;
    hops       = 0u;
    if (pqe) {
        unsigned q = pqe;

        while (q && hops < 16u) {
            unsigned sz = *(unsigned *)(q + 20u);        /* PQESIZE          */
            unsigned rg = *(unsigned *)(q + 24u) & 0x00FFFFFFu; /* PQEREGN   */

            if (hops == 0u) { first_regn = rg; first_size = sz; }
            csa_total += sz;
            printf("  CSA PQE %u at %08X: PQEREGN=%08X PQESIZE=%u\n",
                   hops, q, rg, sz);
            q = *(unsigned *)(q + 8u) & 0x00FFFFFFu;     /* PQEFPQE          */
            hops++;
        }
    }
    printf("  TOTAL CSA (sum of PQESIZE over %u PQE(s)) = %u bytes (%u KB)\n",
           hops, csa_total, csa_total / 1024u);
    wtof("TSTMVCD: CSA total=%u KB regn=%08X pqes=%u",
         csa_total / 1024u, first_regn, hops);
    CHECK(csa_total > 0u, "total CSA read from the control blocks is non-zero");
    CHECK(first_regn != 0u, "CSA region address (PQEREGN) is non-zero");

    /* Largest GETMAIN SP=241 that actually succeeds: a coarse doubling probe,
     * ONE pass, capped at 1 MB, each block freed immediately.  A measurement,
     * not a stress test -- and powers of two UNDERSTATE the true largest by up
     * to 2x, which is the honest reading of the number, not a defect to refine.
     * NOTE for whoever reads the console log: libc370 getmain() WTOs on every
     * failed GETMAIN, so the one failure that ends this search is expected. */
    largest = 0u;
    for (size = 4096u; size <= 1048576u; size <<= 1) {
        p = NULL;
        if (__super(PSWKEY0, &sk) == 0) {
            p = getmain(size, 241u);
            if (p) freemain(p);
            __prob(sk, NULL);
        }
        force_state();
        if (!p) break;
        largest = size;
    }
    printf("  largest successful GETMAIN SP=241 (doubling, capped 1 MB) = %u bytes (%u KB)%s\n",
           largest, largest / 1024u, (largest >= 1048576u) ? " [search capped]" : "");
    wtof("TSTMVCD: largest SP=241 GETMAIN = %u KB", largest / 1024u);
    CHECK(largest > 0u, "at least one GETMAIN SP=241 succeeded and was freed");

    wtof("TSTMVCD: DESTINATION-KEY PROBE DONE");
    return mbt_test_summary("TSTMVCD");
}
