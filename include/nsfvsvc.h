/*
 * nsfvsvc.h -- M5 Stage-0a' SVC cross-AS probe: shared CSA layout + constants.
 *
 * ADR-0038 (supersedes ADR-0036's transport).  This is the data/constants
 * contract shared between the probe STC (src/nsfv.c) and the probe client
 * (test/mvs/tstsvc.c).  The SVC routine (asm/nsfvsvc.asm) mirrors the anchor
 * offsets + constants below as EQUs -- an assembler cannot include this C
 * header, so the "ASSEMBLER MIRROR" block is the single source both sides copy;
 * the NSF_SIZE_ASSERT here guards the total anchor size at cross-compile.
 *
 * Stage-0a' reuses Stage-0a's cross-AS core (the CSA anchor, __xmpost, the
 * in-flight counter, the ESTAE/drain) and swaps ONLY the dispatch layer: an
 * application reaches the stack via a dynamically installed private SVC instead
 * of IEFSSREQ.  The decisive difference: the SVC serves an UNAUTHORIZED,
 * problem-state client (no APF, no self-authorization) -- the thing the SSI
 * path could not do (ADR-0036's open M5-2 question).  No NSFRQE, no socket, no
 * protocol: the probe round-trips a 32-bit token, entirely in registers.
 *
 * Stage-0c (ADR-0040) adds the CLIENT-DEATH GUARD to the same contract: the
 * anchor records the caller's ASID next to its ASCB, and the STC classifies that
 * pair against the ASVT immediately BEFORE the reply POST -- DEAD is reaped and
 * never posted into, UNKNOWN is neither posted into nor reaped (the safe-side
 * asymmetry).  It also adds three PROBE-ONLY function codes (ORPHAN / QUERY /
 * UNSTAGE) that exist to make client death reproducible in a batch job; they are
 * not part of the M5-2 transport.
 *
 * MVS-ONLY.  There is no SVC table / ASCB / CSA on the host, so the transport
 * is not host-simulable; the probe's "host" coverage is the NSF_SIZE_ASSERT and
 * the NSFV_OFF_ASSERTs below firing at cc370 cross-compile (ADR-0038/0040).
 */
#ifndef NSFVSVC_H
#define NSFVSVC_H

#include "nsf.h"        /* UINT + NSF_SIZE_ASSERT                              */
#include <clibecb.h>    /* ECB (unsigned int)                                 */
#include <stddef.h>     /* offsetof (NSFV_OFF_ASSERT)                         */

/* Per-FIELD offset assert, target-only (like NSF_SIZE_ASSERT: host pointers are
 * 8 bytes, so host offsets differ by design).  A total-size assert cannot catch
 * a field that MOVED, and every offset below is mirrored by hand as an EQU in
 * asm/nsfvsvc.asm -- a wrong ANCSTAGE is an IPL-class CSA overrun (ADR-0039 3).
 * These pin the C side at cross-compile; the mirror block is what the assembler
 * copies from. */
#ifdef __MVS__
#define NSFV_OFF_ASSERT(type, field, off) \
    typedef char nsfv_off_##type##_##field[(offsetof(type, field) == (off)) ? 1 : -1]
#else
#define NSFV_OFF_ASSERT(type, field, off) \
    typedef char nsfv_off_##type##_##field[1]
#endif

/* --- Probe identity ------------------------------------------------------ */
#define NSFV_ROUTER_MOD   "NSFVSVC"  /* CSA SVC-routine load module (__loadhi) */

/* The stolen installation SVC number (200-255; IBM assigns user SVCs from 255
 * down).  Configurable; the STC saves the original 8-byte SVCTABLE entry and
 * restores it at stop AND on abend, so a wrongly-chosen busy slot is restored
 * rather than lost.  239 is the highest genuinely-FREE slot on the MVSCE target
 * (240-255 are in use there; the STC's scan confirmed 40 free slots in 200-255,
 * highest free 239, unused-SVC marker EP 0000CCC8).  The STC refuses to steal a
 * busy slot (NSFV029E) and logs the free landscape, so re-pick from there. */
#ifndef NSFV_SVCNUM
#define NSFV_SVCNUM       239U
#endif

/* The request-block eye-catcher.  The client passes R1 = &NSFV_REQ (the M5-2
 * NSFRQE-by-pointer shape; ADR-0038 §6); the routine checks this eye so a stray
 * SVC caller is rejected cleanly.  EBCDIC-transparent: the client copies the
 * "NSFV" chars, the routine compares =CL4'NSFV' -- matches on the target. */
#define NSFV_REQ_EYE      "NSFV"

/* Offset (from the CSA routine's entry point) of the anchor-pointer word the
 * STC patches once at start; the routine loads it via R6 (its own entry).  The
 * routine module lays out: B GO (+00,4) ; NSFVANCH DC A(0) (+04) -- so +04. */
#define NSFV_ANCH_OFF     4U

/* --- Anchor state -------------------------------------------------------- */
#define NSFV_ANCHOR_ACTIVE  0x80000000U

/* Single request-slot lifecycle.  Each transition has ONE writer, so the two
 * address spaces never both write the same field:
 *   FREE -> PENDING : SVC routine (client side), key 0
 *   PENDING -> DONE : STC service (server side), key 0
 *   DONE -> FREE    : SVC routine (client side), key 0                       */
#define NSFV_REQ_FREE     0U
#define NSFV_REQ_PENDING  1U
#define NSFV_REQ_DONE     2U

/* HELD (Stage-0c, ADR-0040 6): the STC could NOT establish that the client is
 * alive (UNKNOWN), so it neither posted into the request nor reclaimed it.  The
 * slot stays BUSY to the SVC routine (which rejects any non-FREE state) but is
 * no longer a work item to the STC -- without a distinct state the executive's
 * "drain while PENDING" loop would spin on a request it refuses to service. */
#define NSFV_REQ_HELD     3U

/* Router return codes (-> R15 to the SVC issuer). */
#define NSFV_RC_OK        0
#define NSFV_RC_INVALID   4    /* bad R1 magic (not our caller)              */
#define NSFV_RC_CORRUPT   8    /* anchor gone / server quiescing             */
#define NSFV_RC_NOREQ     12   /* request slot busy (concurrent client)      */

/* Request functions.  ECHO (Stage-0a') increments the token; XFER (Stage-0b)
 * moves a ubuf app->stack->app through a CSA staging buffer with a byte-wise
 * +1 transform (ADR-0039). */
/* M5-2a (ADR-0041): carry a frozen NSFRQE across.  The client stages its
 * NSFRQE into anchor->rqe, the STC dispatches an STC-PRIVATE copy of it, and
 * the result fields come back in the same slot.  Unlike ECHO/XFER this verb
 * reaches the real executive -- it is the first non-probe request. */
#define NSFV_REQ_RQE      6U

#define NSFV_REQ_ECHO     1U
#define NSFV_REQ_XFER     2U

/* PROBE-ONLY function codes (Stage-0c, ADR-0040 8).  They exist so a batch job
 * can reproduce client death deterministically, with no operator timing, and
 * they are NOT part of the transport M5-2 inherits:
 *   ORPHAN  stage the identity the CLIENT supplies (pascb/pasid) instead of the
 *           FLIH's, POST the STC, and return WITHOUT waiting -- so the in-flight
 *           decrement is skipped by construction, which is what a dead client
 *           leaves behind, while the caller survives to observe the outcome.
 *   QUERY   read req_state / inflight / reaped / served back into the request
 *           block.  Changes nothing, works while the slot is busy: an
 *           unauthorized client cannot read the anchor in CSA itself.
 *   UNSTAGE release a slot the STC deliberately did not release (the HELD case,
 *           and a LIVE orphan), so the probe leaves no in-flight count behind
 *           and the STC still stops clean. */
#define NSFV_REQ_ORPHAN   3U
#define NSFV_REQ_QUERY    4U
#define NSFV_REQ_UNSTAGE  5U

/* CSA staging / copy chunk = BUFLARGE (the large PBUF, ADR-0009), so M5-2's
 * marshalling copies straight into/out of 2048-byte PBUFs (ufsd used 4K -- not
 * PBUF-aligned; ADR-0039).  A ubuf > 2048 is moved in 2048-byte chunks, one
 * SVC<->STC round trip per chunk. */
#define NSFV_XFER_CHUNK   2048U

/* The M5-2a request slot: one frozen NSFRQE (spec 10.4, 64 bytes on target). */
#define NSFV_RQE_SLOT     64U

/* ============================================================
 * NSFV_REQ -- the client's request block (R1 -> here at the SVC).
 *
 * The M5-2 NSFRQE-by-pointer shape, staged on an empty token.  For XFER it also
 * carries the ubuf address + length IN THE CALLER'S ADDRESS SPACE (the routine
 * runs in that AS and MVCKs the ubuf<->staging; ADR-0039).  The routine reads
 * eye+func+token+ubuf+ulen IN and writes token+seq+rc OUT.
 *
 * Stage-0c appends the probe-only words (ADR-0040 8): pascb/pasid are the
 * identity ORPHAN stages verbatim, and qstate/qinfl/qreap are what QUERY reports
 * back.  They ride the SAME block rather than a second one so the client keeps
 * one shape and one issuer.  48 bytes.
 * ============================================================ */
typedef struct nsfv_req {
    char      eye[4];       /* +00 "NSFV"                                     */
    UINT      func;         /* +04 request function (ECHO / XFER / probe)     */
    UINT      token;        /* +08 in: client token; out: STC echo (+1)      */
    int       rc;           /* +0C out: router return code (also -> R15)     */
    UINT      seq;          /* +10 out: server's served-counter snapshot     */
    void     *ubuf;         /* +14 XFER: caller-AS buffer address            */
    UINT      ulen;         /* +18 XFER: caller buffer length (bytes to move) */
    void     *pascb;        /* +1C ORPHAN in: client ASCB to stage (verbatim) */
    UINT      pasid;        /* +20 ORPHAN in: client ASID to stage (verbatim) */
    UINT      qstate;       /* +24 QUERY out: anchor req_state                */
    UINT      qinfl;        /* +28 QUERY out: anchor inflight                 */
    UINT      qreap;        /* +2C QUERY out: anchor reaped (dead requests)   */
    void     *rqeimg;       /* +30 RQE: caller-AS address of the 64-byte NSFRQE
                            **     image (M5-2a, ADR-0041).  A SEPARATE field
                            **     from ubuf because a real socket op needs
                            **     BOTH at once -- ubuf carries the user data,
                            **     rqeimg the request block.  NSFV_REQ is the
                            **     transport block, not the frozen contract, so
                            **     growing it is free; NSFRQE stays 64 B.       */
} NSFV_REQ;                 /* +34 = 52 bytes                                */
NSF_SIZE_ASSERT(NSFV_REQ, 52);
NSFV_OFF_ASSERT(NSFV_REQ, func,   4);
NSFV_OFF_ASSERT(NSFV_REQ, token,  8);
NSFV_OFF_ASSERT(NSFV_REQ, rc,    12);
NSFV_OFF_ASSERT(NSFV_REQ, seq,   16);
NSFV_OFF_ASSERT(NSFV_REQ, ubuf,  20);
NSFV_OFF_ASSERT(NSFV_REQ, ulen,  24);
NSFV_OFF_ASSERT(NSFV_REQ, pascb, 28);
NSFV_OFF_ASSERT(NSFV_REQ, pasid, 32);
NSFV_OFF_ASSERT(NSFV_REQ, qstate, 36);
NSFV_OFF_ASSERT(NSFV_REQ, qinfl, 40);
NSFV_OFF_ASSERT(NSFV_REQ, qreap, 44);
NSFV_OFF_ASSERT(NSFV_REQ, rqeimg, 48);

/* ============================================================
 * NSFV_ANCHOR -- the CSA (SP=241, key 0) rendezvous block.
 *
 * ONE fixed request slot (single-task sequential probe client).  Stage-0a's
 * NSFP_ANCHOR (48 B) with two differences the SVC path forces:
 *   - the reply target is an EMBEDDED CSA ECB (reply_ecb), not a pointer to a
 *     router stack-local: the SVC routine runs supervisor / key 0 throughout,
 *     so it WAITs on a key-0 CSA ECB legally (empirical unknown #1, ADR-0038;
 *     Stage-0a's key-8-stack-ECB rule does not transfer);
 *   - an 18-word scratch save area (csasave) the RENT SVC routine uses to
 *     preserve its registers across the branch POST (STM/LM, the exact @@xmpost
 *     pattern); the routine has no stack, and only R9 survives the cross-AS
 *     POST natively.  Single-client-sequential, so the shared scratch is safe
 *     here; a concurrent-client M5-2 needs per-invocation scratch (the SVRB /
 *     GETMAIN) -- ADR-0038.
 * Stage-0c (ADR-0040) adds req_asid -- the caller's ASID, captured at SVC entry
 * from ASCBASID (ASCB+X'24') and NOT from the request, so a client cannot forge
 * it -- and reaped, the count of requests the guard reclaimed from dead clients.
 * stage[] stays the LAST field: ADR-0039's clamp argument (an over-long ulen
 * would run MVCK off the end of stage[] into adjacent CSA) depends on it.
 * Target layout, 4-byte pointers -- 120 bytes.
 *
 * ============ ASSEMBLER MIRROR (asm/nsfvsvc.asm carries these as EQUs) ======
 *   ANCEYE     EQU  0    CL8  "NSFVANCR"
 *   ANCVER     EQU  8    F
 *   ANCFLAG    EQU 12    F    ANCHOR_ACTIVE = X'80000000'
 *   ANCSECB    EQU 16    F    server_ecb  (STC WAIT target)
 *   ANCSASCB   EQU 20    A    server_ascb (STC ASCB, POST target)
 *   ANCINFL    EQU 24    F    inflight
 *   ANCSTATE   EQU 28    F    req_state
 *   ANCTOKEN   EQU 32    F    req_token
 *   ANCRECB    EQU 36    F    reply_ecb   (client WAIT target, CSA)
 *   ANCRASCB   EQU 40    A    req_ascb    (client ASCB, POST target)
 *   ANCSERVED  EQU 44    F    served
 *   ANCSAVE    EQU 48    18F  SVC-routine POST register save area
 *   ANCXFUNC   EQU 120   F    transform the STC applies (ECHO / XFER)
 *   ANCXLEN    EQU 124   F    bytes in the staging buffer this chunk
 *   ANCXASID   EQU 128   F    req_asid  (client ASID, ASCBASID at ASCB+X'24')
 *   ANCREAPD   EQU 132   F    reaped    (dead requests reclaimed, diagnostic)
 *   ANCSTAGE   EQU 136   2048 CSA staging buffer (the ubuf CSA bounce)
 * ============================================================================
 * The staging buffer is CSA-shared (the STC must reach it -- it cannot live in
 * the SVRB), single-client-sequential like csasave; M5-2 concurrency needs
 * per-client staging (ADR-0039).  Stage-0b adds no other shared scratch.
 * ============================================================ */
typedef struct nsfv_anchor {
    char      eye[8];             /* +00 "NSFVANCR"                           */
    UINT      version;            /* +08 anchor version                      */
    UINT      flags;              /* +0C NSFV_ANCHOR_ACTIVE                  */
    ECB       server_ecb;         /* +10 STC WAIT target (CSA, key 0)        */
    void     *server_ascb;        /* +14 STC ASCB (__ascb(0) at startup)    */
    UINT      inflight;           /* +18 clients executing inside the routine */
    UINT      req_state;          /* +1C NSFV_REQ_FREE / _PENDING / _DONE    */
    UINT      req_token;          /* +20 in: client token; out: echo (+1)    */
    ECB       reply_ecb;          /* +24 client WAIT target (CSA, key 0)     */
    void     *req_ascb;           /* +28 client ASCB (__xmpost target)      */
    UINT      served;             /* +2C requests serviced (diagnostic)      */
    UINT      csasave[18];        /* +30 SVC-routine POST save area (72 B)    */
    UINT      xfunc;              /* +78 transform: NSFV_REQ_ECHO / _XFER     */
    UINT      xlen;               /* +7C bytes in stage[] this chunk          */
    UINT      req_asid;           /* +80 client ASID (ASCBASID, ASCB+X'24')   */
    UINT      reaped;             /* +84 dead requests reclaimed (diagnostic) */
    char      stage[NSFV_XFER_CHUNK]; /* +88 CSA staging (the ubuf bounce)   */
    /* M5-2a request slot (ADR-0041 3): a 64-byte NSFRQE image, APPENDED after
    ** stage[] so `stage` stays at +136 and every ANC* EQU in nsfvsvc.asm is
    ** untouched -- the Stage-0c lesson (an asm change under a MOVED layout is
    ** validated by re-running every stage's live gate) applies here with almost
    ** no surface.  Declared as bytes, not as an NSFRQE: this header describes a
    ** TARGET layout, and on the host NSFRQE inflates to 80 bytes on 8-byte
    ** pointers.  The 64 is guaranteed where it matters by
    ** NSF_SIZE_ASSERT(NSFRQE, 64) in nsfreq.h, which fires at cc370
    ** cross-compile.  Single slot by construction -- the 64-slot (= MAXSOC)
    ** pool is M5-2b. */
    char      rqe[NSFV_RQE_SLOT];     /* +888 the M5-2a NSFRQE request slot   */
    /* The STC's wake ECB address, in the STC's OWN key-8 private storage
    ** (ADR-0041 addendum).  The executive WAITs through ecb_waitlist from
    ** PROBLEM state key 8, and a key-0 CSA ECB in that ECBLIST is a documented
    ** abend (S047 / X'201', ufsd/docs/cross-as-reference.md), so the SVC
    ** routine posts THIS address instead of &server_ecb.  Cross-AS POST takes
    ** an ASCB and interprets the ECB address in that address space, so private
    ** storage is fine -- ufsd runs the mirror image (STC -> client key-8 stack
    ** ECB) as its final working design.
    **
    ** ZERO means "not published": the routine then falls back to server_ecb,
    ** which is what the Stage-0 probe STC wants (it supervisor-WAITs on the
    ** key-0 CSA ECB), so the four Stage-0 gates stay a regression rather than
    ** becoming a rewrite.  Appended, never inserted (ADR-0041 3). */
    void     *server_ecb_ptr;         /* +8C8 A(STC private key-8 wake ECB)   */
} NSFV_ANCHOR;                    /* +8CC = 2252 bytes                        */
NSF_SIZE_ASSERT(NSFV_ANCHOR, 2252);
NSFV_OFF_ASSERT(NSFV_ANCHOR, version,     8);
NSFV_OFF_ASSERT(NSFV_ANCHOR, flags,      12);
NSFV_OFF_ASSERT(NSFV_ANCHOR, server_ecb, 16);
NSFV_OFF_ASSERT(NSFV_ANCHOR, server_ascb, 20);
NSFV_OFF_ASSERT(NSFV_ANCHOR, inflight,   24);
NSFV_OFF_ASSERT(NSFV_ANCHOR, req_state,  28);
NSFV_OFF_ASSERT(NSFV_ANCHOR, req_token,  32);
NSFV_OFF_ASSERT(NSFV_ANCHOR, reply_ecb,  36);
NSFV_OFF_ASSERT(NSFV_ANCHOR, req_ascb,   40);
NSFV_OFF_ASSERT(NSFV_ANCHOR, served,     44);
NSFV_OFF_ASSERT(NSFV_ANCHOR, csasave,    48);
NSFV_OFF_ASSERT(NSFV_ANCHOR, xfunc,     120);
NSFV_OFF_ASSERT(NSFV_ANCHOR, xlen,      124);
NSFV_OFF_ASSERT(NSFV_ANCHOR, req_asid,  128);
NSFV_OFF_ASSERT(NSFV_ANCHOR, reaped,    132);
NSFV_OFF_ASSERT(NSFV_ANCHOR, stage,     136);
NSFV_OFF_ASSERT(NSFV_ANCHOR, rqe,      2184);
NSFV_OFF_ASSERT(NSFV_ANCHOR, server_ecb_ptr, 2248);

#endif /* NSFVSVC_H */
