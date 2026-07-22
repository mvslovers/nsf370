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
 * MVS-ONLY.  There is no SVC table / ASCB / CSA on the host, so the transport
 * is not host-simulable; the probe's "host" coverage is the NSF_SIZE_ASSERT
 * below firing at cc370 cross-compile (ADR-0038).
 */
#ifndef NSFVSVC_H
#define NSFVSVC_H

#include "nsf.h"        /* UINT + NSF_SIZE_ASSERT                              */
#include <clibecb.h>    /* ECB (unsigned int)                                 */

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

/* Router return codes (-> R15 to the SVC issuer). */
#define NSFV_RC_OK        0
#define NSFV_RC_INVALID   4    /* bad R1 magic (not our caller)              */
#define NSFV_RC_CORRUPT   8    /* anchor gone / server quiescing             */
#define NSFV_RC_NOREQ     12   /* request slot busy (concurrent client)      */

/* The one probe request: the STC increments the token and bumps a served
 * counter, so the client verifies both a byte-exact transform (token -> token+1)
 * and a monotonic server-side count. */
#define NSFV_REQ_ECHO     1U

/* ============================================================
 * NSFV_REQ -- the client's request block (R1 -> here at the SVC).
 *
 * The M5-2 NSFRQE-by-pointer shape, staged on an empty token.  Mirrors
 * Stage-0a's NSFPSSOB extension; carries NO pointers (abend-immune, and the
 * probe needs none -- the token crosses via the CSA anchor, not this block).
 * The routine reads eye+token IN and writes token+seq+rc OUT (plain key-0 store
 * for the probe; M5-2 uses MVCP/MVCS -- ADR-0038 §6).  20 bytes.
 * ============================================================ */
typedef struct nsfv_req {
    char      eye[4];       /* +00 "NSFV"                                     */
    UINT      func;         /* +04 request function (NSFV_REQ_ECHO)          */
    UINT      token;        /* +08 in: client token; out: STC echo (+1)      */
    int       rc;           /* +0C out: router return code (also -> R15)     */
    UINT      seq;          /* +10 out: server's served-counter snapshot     */
} NSFV_REQ;                 /* +14 = 20 bytes                                */
NSF_SIZE_ASSERT(NSFV_REQ, 20);

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
 * ============================================================================
 * ============================================================ */
typedef struct nsfv_anchor {
    char      eye[8];       /* +00 "NSFVANCR"                                 */
    UINT      version;      /* +08 anchor version                            */
    UINT      flags;        /* +0C NSFV_ANCHOR_ACTIVE                        */
    ECB       server_ecb;   /* +10 STC WAIT target (CSA, key 0)              */
    void     *server_ascb;  /* +14 STC ASCB (__ascb(0) at startup)          */
    UINT      inflight;     /* +18 clients executing inside the SVC routine   */
    UINT      req_state;    /* +1C NSFV_REQ_FREE / _PENDING / _DONE          */
    UINT      req_token;    /* +20 in: client token; out: STC's echo (+1)    */
    ECB       reply_ecb;    /* +24 client WAIT target (CSA, key 0)           */
    void     *req_ascb;     /* +28 client ASCB (__xmpost target)            */
    UINT      served;       /* +2C requests serviced (monotonic diagnostic)  */
    UINT      csasave[18];  /* +30 SVC-routine POST register save area (72 B) */
} NSFV_ANCHOR;              /* +78 = 120 bytes                               */
NSF_SIZE_ASSERT(NSFV_ANCHOR, 120);

#endif /* NSFVSVC_H */
