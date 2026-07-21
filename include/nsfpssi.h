/*
 * nsfpssi.h -- M5 Stage-0a SSI cross-AS probe: shared CSA/SSOB layout.
 *
 * ADR-0036.  This is the ONLY thing shared between the probe STC (src/nsfp.c)
 * and the SSI router (src/nsfpssir.c): they share DATA (the CSA anchor + the
 * SSOB extension) and CONSTANTS, never a function -- so the whole probe has
 * exactly one cross-module external, the router entry point NSFPSSIR (aliased
 * below).  No NSFRQE, no socket, no protocol: the probe round-trips a token
 * only, to prove the cross-AS transport mechanics before M5-2 builds on them.
 *
 * MVS-ONLY.  ASCB/SSI/CSA do not exist on the host, so this pulls MVS-only
 * libc370 seams and is never part of a host build.  The probe's "host"
 * coverage is the NSF_SIZE_ASSERTs below firing at cc370 CROSS-COMPILE -- the
 * transport itself is MVS-only by nature (there is no honest host stand-in).
 *
 * Every layout/state/key choice is modelled VERBATIM on the UFSD AP-1c cross-AS
 * design (ufsd/docs/cross-as-reference.md, ufsd/src/ufsd#ssi.c) -- the five
 * successive abends UFSD walked are inherited, not re-derived.  See ADR-0036.
 */
#ifndef NSFPSSI_H
#define NSFPSSI_H

#include "nsf.h"        /* UINT + NSF_SIZE_ASSERT                              */
#include <clibecb.h>    /* ECB (unsigned int)                                 */

/* --- Subsystem identity -------------------------------------------------- */
#define NSFP_SUBSYS       "NSFP"     /* probe subsystem name (NOT NSFS)       */
#define NSFP_ROUTER_MOD   "NSFPSSIR" /* router load module (__loadhi by name) */
#define NSFP_SSOBFUNC     200U       /* SSOB function code routed to router    */
#define NSFP_SSVT_ROUTER  1U         /* SSVT index the router occupies         */

/* --- Anchor state -------------------------------------------------------- */
#define NSFP_ANCHOR_ACTIVE  0x80000000U

/* Single request-slot lifecycle.  Each transition has ONE writer, so the two
 * address spaces never both write the same field:
 *   FREE -> PENDING : router entry (client side), key 0
 *   PENDING -> DONE : STC service  (server side), key 0
 *   DONE -> FREE    : router reply  (client side), key 0                     */
#define NSFP_REQ_FREE     0U
#define NSFP_REQ_PENDING  1U
#define NSFP_REQ_DONE     2U

/* --- SSOB extension eye + router return codes (-> SSOB.SSOBRETN) ---------- */
#define NSFPSSOB_EYE      "NSFP"
#define NSFP_RC_OK        0
#define NSFP_RC_INVALID   4    /* bad SSOB / extension eye                    */
#define NSFP_RC_CORRUPT   8    /* anchor gone / server quiescing              */
#define NSFP_RC_NOREQ     12   /* request slot busy                           */

/* Router timed-wait liveness (mirrors UFSD): 5 s intervals, a sentinel post
 * code distinct from the STC's normal reply (post code 0). */
#define NSFP_WAIT_INTERVAL  500U
#define NSFP_TIMEOUT_CODE   0x0FFFFU

/* The one probe request: the STC increments the token and bumps a served
 * counter, so the client verifies both a byte-exact transform round-trip
 * (token -> token+1) and a monotonic server-side count. */
#define NSFP_REQ_ECHO     1U

/* ============================================================
 * NSFP_ANCHOR -- the CSA (SP=241, key 0) rendezvous block.
 *
 * ONE fixed request slot: the probe client is single-task and sequential, so
 * no free pool or request queue is needed to prove the transport (that
 * machinery is orthogonal to the five state/key abends this probe targets).
 * Target layout, 4-byte pointers -- 48 bytes.
 * ============================================================ */
typedef struct nsfp_anchor {
    char      eye[8];       /* +00 "NSFPANCR"                                 */
    UINT      version;      /* +08 anchor version                            */
    UINT      flags;        /* +0C NSFP_ANCHOR_ACTIVE                        */
    ECB       server_ecb;   /* +10 STC WAIT target (CSA, key 0)              */
    void     *server_ascb;  /* +14 STC ASCB (__ascb(0) at startup)          */
    UINT      inflight;     /* +18 clients executing inside the router       */
    UINT      req_state;    /* +1C NSFP_REQ_FREE / _PENDING / _DONE          */
    UINT      req_token;    /* +20 in: client token; out: STC's echo (+1)    */
    ECB      *req_ecb;      /* +24 -> router's key-8 stack-local reply ECB   */
    void     *req_ascb;     /* +28 client ASCB (__xmpost target)            */
    UINT      served;       /* +2C requests serviced (monotonic diagnostic)  */
} NSFP_ANCHOR;              /* +30 = 48 bytes                                */
NSF_SIZE_ASSERT(NSFP_ANCHOR, 48);

/* ============================================================
 * NSFPSSOB -- the SSOB extension (SSOB.SSOBINDV -> here).
 *
 * The first 16 bytes mirror UFSD's UFSSSOB (eye@0, func@4, token@8, rc@12).
 * UFSD abend #2 (S0C4) was a POINTER landing where the SSI path reads the
 * request extension -- so this extension carries NO pointers at all; the
 * reply ECB is the router's own stack local, never in the extension.
 * ============================================================ */
typedef struct nsfpssob {
    char      eye[4];       /* +00 "NSFP"                                     */
    UINT      func;         /* +04 request function (NSFP_REQ_ECHO)          */
    UINT      token;        /* +08 in: client token; out: STC echo (+1)      */
    int       rc;           /* +0C router return code (mirrors SSOBRETN)     */
    UINT      seq;          /* +10 out: server's served-counter snapshot     */
} NSFPSSOB;                 /* +14 = 20 bytes                                */
NSF_SIZE_ASSERT(NSFPSSOB, 20);

/* The router entry point -- the ONE cross-module external of the probe.
 * MVS invokes it via IEFSSREQ with R1 = SSOB (captured by inline asm inside).
 * project.toml: entry=NSFPSSIR, startup=false; the alias IS that name. */
void nsfpssir(void) asm("NSFPSSIR");

#endif /* NSFPSSI_H */
