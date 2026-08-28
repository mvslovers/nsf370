#ifndef NSFSWAP_H
#define NSFSWAP_H
/*
 * NSFSWAP -- SRM swappability control for the Phase-2 STC (issue #64).
 *
 * WHY THIS EXISTS. 64-0f measured the #64 stall as a COMPLETED MVS swap cycle
 * that left NSFS non-dispatchable for about twelve minutes (ASCBSTOR
 * 0FAF3C00 -> 0FC26C00, OUCBSWC 0 -> 1). A stack other address spaces depend
 * on cannot be unavailable for minutes, so NSFS asks SRM not to swap it.
 * That MITIGATES the symptom and EXPLAINS NOTHING -- see ADR-0044 and #64,
 * which stays open.
 *
 * PHASE 2 ONLY. These functions are compiled into the NSFS module and NOT
 * into NSF: SYSEVENT needs APF authorisation, supervisor state and key 0, and
 * the Phase-1 module has no ac=1, no clib_apf_setup and no __super. The
 * separation is STRUCTURAL (a source list), not a runtime test.
 *
 * EVERYTHING HERE IS READ FROM SYS1.AMODGEN ON THE TARGET, NOT FROM MEMORY.
 * The codes come from SYS1.AMODGEN(SYSEVENT); the field offsets from
 * (IRAOUCB) and (IHAASCB) through the DSECT gate that reproduces every
 * offset a prior IFOX00 CBOFF job proved (17/17 and 13/13) --
 * docs/measurements/64-3-0/cblayout.py.
 */

#include "nsf.h"

/* SYSEVENT event codes -- SYS1.AMODGEN(SYSEVENT), &EVENTO SETA values
 * immediately preceding each mnemonic's AIF (lines 90-93). */
#define NSFSWAP_DONTSWAP   41u   /* line 91: AIF ('&EVENT'EQ'DONTSWAP').EOK */
#define NSFSWAP_OKSWAP     42u   /* line 93: AIF ('&EVENT'EQ'OKSWAP').EOK   */

/* OUCB field offsets and bits -- SYS1.AMODGEN(IRAOUCB). */
#define NSFSWAP_OUCBQFL  0x10u   /* line  95  SWAPPABILITY TRANSITION FLAGS */
#define NSFSWAP_OUCBSFL  0x11u   /* line 102  SWAPOUT CONTINUATION FLAGS    */
#define NSFSWAP_OUCBAFL  0x13u   /* line 120  ALGORITHM STATUS FLAGS        */
#define NSFSWAP_OUCBASCB 0x28u   /* line 171  ASCB ADDRESS (identity)       */
#define NSFSWAP_OUCBNDS  0x84u   /* line 209  NUM OUTSTANDING DONTSWAPS     */
#define NSFSWAP_NSW      0x80u   /* line 103  OUCBNSW BIT0 NON-SWAPPABLE STATUS */
#define NSFSWAP_GOO      0x80u   /* line  96  OUCBGOO BIT0 TRANSITIONING OUT   */
#define NSFSWAP_ASW      0x01u   /* line 127  OUCBASW BIT7 AUTHORIZED FOR DONTSWAP */

/* ASCB field offsets -- SYS1.AMODGEN(IHAASCB). */
#define NSFSWAP_ASCBOUCB 0x90u   /* line 167  SRM USER CONTROL BLOCK (OUCB) */
#define NSFSWAP_ASCBFMCT 0x98u   /* line 171  ALLOCATED PAGE FRAME COUNT    */

/* One reading of SRM's swap view of THIS address space.  Taken through a
 * single code path with a single identity assertion, so a baseline and a
 * post-DONTSWAP reading are comparable -- see nsfswap_read. */
typedef struct nsfswap_view {
    UINT    oucb;       /* @0  OUCB address (0 = not reached)              */
    USHORT  nds;        /* @4  OUCBNDS  outstanding DONTSWAPs              */
    USHORT  fmct;       /* @6  ASCBFMCT allocated page frames              */
    UCHAR   sfl;        /* @8  OUCBSFL  (NSW = X'80')                      */
    UCHAR   afl;        /* @9  OUCBAFL  (ASW = X'01')                      */
    UCHAR   qfl;        /* @10 OUCBQFL  (GOO = X'80')                      */
    UCHAR   rsvd;       /* @11                                             */
} NSFSWAPVIEW;          /* 12 bytes */
NSF_SIZE_ASSERT(NSFSWAPVIEW, 12);

/* The raw seam: R0 = code, SVC 95, R15 back (asm/nsfsevt.asm).
 * CALLER MUST ALREADY BE supervisor state / key 0 (__super(PSWKEY0, ...)).
 * R15 IS UNSPECIFIED for these codes -- the ancestor recorded that the
 * documentation names no return code (mvsevent.asm:15-16) -- so it is
 * reported and NEVER branched on. The read-back is the proof. */
INT nsf_sysevent(UINT code) asm("NSFSEVT");

/* Read the three SRM fields plus ASCBFMCT.  Returns 0 on success, non-zero if
 * IDENTITY could not be established (no ASCB, no OUCB, wrong eyecatcher, or
 * OUCBASCB not pointing back at our own ASCB -- the 64-0c LSQA trap one
 * control block over). On failure *v is zeroed. Does NOT require key 0: SQA
 * is key 0 but not fetch-protected (Stage-0b, ADR-0039), so a key-8 fetch
 * succeeds. It is called inside the caller's existing key-0 window anyway,
 * so that "what runs in key 0" stays one short block. */
INT nsfswap_read(NSFSWAPVIEW *v) asm("NSFSWRD");

/* Stage A (64-3-1): the isolated probe. Baseline -> DONTSWAP -> read ->
 * OKSWAP -> read, every reading WTOed, and OUCBNDS shown returning to its
 * baseline VALUE. Operator-gated (F NSFS,SWAP) and never on the startup
 * path, so a wrong answer cannot break a normal start. Leaves the address
 * space exactly as it found it. */
void nsfswap_probe(void) asm("NSFSWPRB");

/* Stage B (64-3-1): the mitigation.  Ask SRM not to swap this address space
 * (DONTSWAP) / release it again (OKSWAP).  Both return 0 only when the
 * READ-BACK agrees -- OUCBNSW set for dontswap, clear for okswap -- never on
 * R15, which is unspecified for these codes.  *out (optional) receives the
 * post-call reading so the caller can report NDS without reading twice.
 *
 * These are deliberately NOT nsfswap_probe: the probe WTOs eleven lines and
 * is an operator diagnostic; init and shutdown want one line each, and the
 * failure line has to name the condition.  Reporting is the caller's.
 *
 * READ nsfswap_okswap's 0 CAREFULLY: it means "OUCBNSW is clear NOW", which
 * is also true when nothing was ever pinned -- it is NOT proof that a pin was
 * released.  That is the right semantic for the shutdown message (the operator
 * wants to know the address space is swappable, not how it got that way), and
 * it was observed live: the 64-3-1 revert build, which never issues DONTSWAP,
 * still reported NSF853I at shutdown.  Do not cite that message as evidence of
 * a release; the release evidence is nsfswap_probe's step 5, which compares
 * OUCBNDS against a baseline VALUE it read first. */
INT nsfswap_dontswap(NSFSWAPVIEW *out) asm("NSFSWDNT");
INT nsfswap_okswap(NSFSWAPVIEW *out)   asm("NSFSWOKS");

/* The operator verb handler registered with nsfopr_set_swap by the Phase-2
 * STC. Phase 1 registers nothing, so `F NSF,SWAP` stays NSF808E. */
void nsfswap_op(const char *arg) asm("NSFSWOP");

#endif /* NSFSWAP_H */
