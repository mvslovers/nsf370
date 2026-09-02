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

/* THIS SERVER SERVICES THE PROBE VERBS (issue #67, M5-2d1c).  Set by the probe
 * STC (src/nsfv.c) and by nothing else; the production STC (src/nsfsx.c)
 * deliberately leaves it clear.
 *
 * WHY THE BIT EXISTS.  A probe verb reaching the PRODUCTION STC claims a CSA
 * slot and an in-flight count, is set HELD by the dispatch arm because its
 * xfunc is not RQE (src/nsfsx.c, NSFREQX_ACT_DISPATCH), and is never
 * re-examined -- so the client parks on its reply ECB forever and the slot is
 * gone for the life of the STC.  That is issue #67, and it is not theoretical:
 * M5-2d1's second live round walked into it, had to cancel the job, and left
 * an anchor plus the router retained to IPL.  The routine now refuses ahead of
 * the claim, so the refusal costs no slot and no in-flight count BY POSITION
 * rather than by argument -- the form M5-2c2 established for the retired
 * FNORPH.
 *
 * PERMIT ONE VERB, DO NOT REFUSE TWO BY NAME.  The staging dispatch at CLAIMOK
 * is a fall-through chain ending in ECHO, so ECHO is not the only way in: ANY
 * unrecognised REQFUNC -- one wrong word in a client -- falls through, stages
 * as ECHO, and hangs identically.  Naming ECHO and XFER would leave that open,
 * and it is the cheapest instance of the fault.  So the production STC permits
 * exactly FNRQE and refuses everything else.  QUERY / UNSTAGE / SLOT are
 * unaffected either way: they branch out of the chain ABOVE this point and
 * take no slot.
 *
 * POLARITY IS FAIL-CLOSED, AND THAT IS THE REASON FOR IT.  The bit says
 * "permitted", never "forbidden", so the zero an unset or zeroed anchor
 * carries means REFUSE.  A server that has not said it services probe verbs is
 * not assumed to.
 *
 * WHY flags AND NOT rsvd0.  rsvd0 is the LAST slack word in the header and its
 * own comment names the use it was reserved for (the lost-race counter);
 * spending it here would put the next addition back to a full Stage-0 round.
 * ANCFLAG is already loaded and TM'd one instruction above the insertion
 * point, so the marginal cost of a second TM is one instruction.
 *
 * NSFV_ANCHOR_VER STAYS 3, AND NEITHER SKEW DIRECTION IS SILENT.  The version
 * guards the router<->STC layout contract and no field moved.  A stale router
 * against a new STC does not test the bit, so probe verbs are serviced as they
 * are today -- correct at NSFV, and no worse than today at NSFS.  A new router
 * against a stale STC reads the bit CLEAR at NSFV and refuses every probe verb
 * there, which takes the whole Stage-0 set red immediately and loudly.  A
 * version bump would be the honest answer to a SILENT skew; there is none.
 *
 * LIFETIME.  M5-2c3 retires the probe verbs after (e).  When it does, this bit,
 * the routine's gate and the NSFV assignment all go with them -- they are not
 * left behind as unattributable dead code. */
#define NSFV_ANCHOR_PROBE   0x40000000U

/* Slot lifecycle (M5-2b3 / ADR-0042).  Was a single request area with one
 * writer per transition; it is now a per-slot state word claimed by CS, and
 * CLAIMED is the new state between FREE and PENDING -- the window in which a
 * client owns the slot but has not finished staging into it:
 *   FREE    -> CLAIMED : SVC routine, BY CS -- the only contended transition
 *   CLAIMED -> PENDING : SVC routine, after staging: publish LAST
 *   CLAIMED -> FREE    : SVC routine, bail before publishing (POST failed)
 *   PENDING -> DONE    : STC service
 *   PENDING -> HELD    : STC, client liveness UNKNOWN
 *   PENDING -> FREE    : STC, reaping a DEAD client -- BY CS (see below)
 *   DONE    -> FREE    : SVC routine, releasing its own slot (plain ST)
 * The truth table is host-pinned by nsfreqx_slot_legal (TSTREQX); these
 * values are mirrored there as NSFREQX_ST_* so the pure half builds on the
 * host, where there is no CSA.
 *
 * THREE WRITERS, THREE RULES (ADR-0042 2).  Claim by CS: two clients may
 * reach the same word.  Release by plain ST: the owner races with nobody.
 * REAP by CS: the STC is a THIRD OBSERVER, and between reading a slot as
 * reapable and reclaiming it the owner can complete, release, and a new
 * client can claim -- a blind store there hands one slot to two owners.
 * "ABA-free by construction" covers the claim, not the reaper.            */
#define NSFV_REQ_FREE     0U
#define NSFV_REQ_PENDING  1U
#define NSFV_REQ_DONE     2U

/* HELD (Stage-0c, ADR-0040 6): the STC could NOT establish that the client is
 * alive (UNKNOWN), so it neither posted into the request nor reclaimed it.  The
 * slot stays BUSY to the SVC routine (which rejects any non-FREE state) but is
 * no longer a work item to the STC -- without a distinct state the executive's
 * "drain while PENDING" loop would spin on a request it refuses to service. */
#define NSFV_REQ_HELD     3U

/* CLAIMED (M5-2b3): the claim CS succeeded, staging is in progress, nothing is
 * published yet.  Busy to another client, and NOT a work item to the STC.
 *
 * IT IS NEVER REAPED, AND NOT BECAUSE IT CANNOT BE CLASSIFIED.  The identity
 * (req_ascb / req_asid) is recorded at the CLAIM -- immediately after the CS,
 * before any staging (asm/nsfvsvc.asm, CLAIMOK) -- so a CLAIMED slot has a
 * real ASCB and nsfreqx_classify will answer LIVE or DEAD for it.  Safety
 * rests on nsfreqx_reap_ok excluding CLAIMED EXPLICITLY, and on nothing else.
 *
 * Consequence, stated rather than fixed: a client whose address space ends
 * between the claim and the publish leaks that slot until the STC stops.  It
 * is classifiable, so that leak is closable -- but not by widening the
 * predicate: the two-move reap proves exclusivity with CS(observed ->
 * CLAIMED), and when the observed state already IS CLAIMED that compare
 * succeeds trivially and cannot tell "I took it" from "the live owner still
 * has it".  Closing it needs a distinct fourth state (CS(CLAIMED -> REAPING)).
 * That belongs with fault recovery, which ADR-0039/0041 still name as open. */
#define NSFV_REQ_CLAIMED  4U

/* Router return codes (-> R15 to the SVC issuer). */
#define NSFV_RC_OK        0
#define NSFV_RC_INVALID   4    /* bad R1 magic (not our caller)              */
#define NSFV_RC_CORRUPT   8    /* anchor gone / server quiescing             */
#define NSFV_RC_NOREQ     12   /* named slot busy (probe paths only, b3)     */
#define NSFV_RC_NOBUF     16   /* POOL FULL -- every slot taken (ADR-0042 7) */

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
 *   ORPHAN  RETIRED in M5-2c2 stage b.  It staged the identity the CLIENT
 *           supplied (pascb/pasid) instead of the FLIH's -- a request-supplied
 *           identity trusted verbatim from an unauthorised caller, which is
 *           precisely what the guard must never do for a real client.  That is
 *           the identity half of obligation #4, and retiring the verb discharges
 *           it in substance; the rest of the probe scaffolding (ECHO / XFER /
 *           UNSTAGE / SLOT) is c3, so #4 is NOT met overall.
 *           The code is kept and REJECTED BY NAME ahead of the slot claim
 *           (asm/nsfvsvc.asm, the pre-claim chain -> BADFUNC), so it costs no
 *           slot and no in-flight count and can never be silently reused.
 *   QUERY   read req_state / inflight / reaped / served back into the request
 *           block.  Changes nothing, works while the slot is busy: an
 *           unauthorized client cannot read the anchor in CSA itself.
 *   UNSTAGE release a slot the STC deliberately did not release (the HELD case,
 *           and a LIVE orphan), so the probe leaves no in-flight count behind
 *           and the STC still stops clean. */
/* FUNCTION CODE 3 IS PERMANENTLY RESERVED AND MUST NOT BE REUSED.
 *
 * It was NSFV_REQ_ORPHAN.  The verb was retired in M5-2c2 stage b and the C
 * constant removed in stage c, once the TSTDEATH decision it was waiting on had
 * been taken (the rule that kept it alive: a mechanical change must not settle
 * an open design question as a side effect).
 *
 * REMOVING THE NAME DID NOT REMOVE THE CODE.  A stale client built before the
 * retirement -- or a hostile one -- can still put 3 in req.func, so the SVC
 * routine still knows the code and REJECTS it by name, ahead of the slot claim
 * (asm/nsfvsvc.asm: FNORPH EQU 3 -> BADFUNC -> NSFV_RC_INVALID, claiming no slot
 * and taking no in-flight count).  That asm is NOT dead code and must not be
 * "cleaned up" as such: it is the only thing standing between a retired verb
 * and the ECHO default it would otherwise fall through to. */
#define NSFV_REQ_QUERY    4U
#define NSFV_REQ_UNSTAGE  5U

/* PROBE-ONLY, added by M5-2b3 (ADR-0042).  SLOT compare-and-swaps ONE named
 * slot's state from `sexpect` to `snew`, so a test can pre-claim slots and
 * then observe that the scan skips exactly those, or fill the pool and observe
 * ENOBUFS.  An unauthorised client cannot store into CSA itself, and the
 * routine (key 0) can.
 *
 * IT IS A CS, NOT A BLIND STORE, and the reason is the same one that governs
 * the reaper: a blind "set slot i to CLAIMED" would stomp a LIVE claim if the
 * test miscounted, and that failure would present as a pool bug.  A failed
 * compare returns NSFV_RC_NOREQ, so the test asserts the pre-claim TOOK rather
 * than inferring it from the absence of a complaint (CLAUDE.md 8.5).
 *
 * SCAFFOLDING, DUE OUT IN M5-2c -- which is a SECURITY item, not hygiene. */
#define NSFV_REQ_SLOT     7U

/* CSA staging / copy chunk = BUFLARGE (the large PBUF, ADR-0009), so M5-2's
 * marshalling copies straight into/out of 2048-byte PBUFs (ufsd used 4K -- not
 * PBUF-aligned; ADR-0039).  A ubuf > 2048 is moved in 2048-byte chunks, one
 * SVC<->STC round trip per chunk. */
#define NSFV_XFER_CHUNK   2048U

/* The M5-2a request slot: one frozen NSFRQE (spec 10.4, 64 bytes on target). */
#define NSFV_RQE_SLOT     64U

/* ---- M5-2b3: the pool (ADR-0042) ---------------------------------------- */

/* One slot per possible socket.  NSFSOC_MAX_DEFAULT is 64 and nsfeza.c
 * static-asserts NSFEZA_MAXSOC against it, so 64 slots means the transport can
 * never be the thing that runs out first. */
#define NSFV_NSLOTS       64U

/* Anchor layout version, checked by the SVC routine against its own EQU.
 *
 * NSFVSVC is a SEPARATE load module from NSFS, __loadhi'd into CSA, and this
 * project has a documented way for the two to diverge: `make deploy` fails
 * mid-chain while an STC holds NSF.LINKLIB and the run afterwards silently
 * uses the PREVIOUSLY deployed module (CLAUDE.md 5).  A stale router against a
 * new STC is not a wrong answer -- it is a scan striding by the wrong slot
 * length, or walking off the end of the allocation into adjacent CSA, which is
 * the IPL-class overrun ADR-0039 3 names.
 *
 * BUMP THIS WITH EVERY LAYOUT MOVE, or the check is decorative.  1 was the
 * single-slot anchor through M5-2b2; 2 is the pool; 3 adds the M5-2b4 header
 * words (`collisions` + one reserved), which moves the slot array by 8. */
#define NSFV_ANCHOR_VER   3U

/* ============================================================
 * NSFV_REQ -- the client's request block (R1 -> here at the SVC).
 *
 * The M5-2 NSFRQE-by-pointer shape, staged on an empty token.  For XFER it also
 * carries the ubuf address + length IN THE CALLER'S ADDRESS SPACE (the routine
 * runs in that AS and MVCKs the ubuf<->staging; ADR-0039).  The routine reads
 * eye+func+token+ubuf+ulen IN and writes token+seq+rc OUT.
 *
 * Stage-0c appends the probe-only words (ADR-0040 8): rsvd_pascb/rsvd_pasid
 * are RESERVED since M5-2c2 stage b (they were the identity the retired ORPHAN
 * staged verbatim), and qstate/qinfl/qreap are what QUERY reports
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
    /* RESERVED since M5-2c2 stage b.  These were ORPHAN's request-supplied
    ** identity; the verb is retired and nothing reads them any more.  They are
    ** KEPT, not removed, and that is a deliberate no-layout-change decision:
    ** they sit MID-STRUCT with seven fields after them, two of which carry real
    ** requests (rqeimg, and slot -- written for EVERY request), and NOTHING
    ** VERSION-CHECKS THIS BLOCK.  The router validates a layout-INVARIANT
    ** eyecatcher; NSFV_ANCHOR_VER guards the anchor (router<->STC), not this
    ** client<->router contract.  Client modules link separately from
    ** NSF.LINKLIB and the runbook replaces only the latter, so a router-only
    ** deploy would give a new client an old router reading these offsets
    ** silently.  They can go when this block gains a version check, or in a
    ** round that relinks every client with the router. */
    void     *rsvd_pascb;   /* +1C reserved (was ORPHAN pascb)                */
    UINT      rsvd_pasid;   /* +20 reserved (was ORPHAN pasid)                */
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
    /* M5-2b3 (ADR-0042).  `slot` is IN for the probe verbs that name a slot
    ** (SLOT / QUERY / UNSTAGE) and OUT for every real request -- the index the
    ** scan actually claimed.  Reporting it is what makes the live reuse and
    ** skip checks observations rather than inferences: "the same slot came
    ** back" and "the scan stepped over the pre-claimed ones" are both
    ** statements about this number.  `sexpect` / `snew` are the SLOT verb's
    ** compare-and-swap operands.
    **
    ** NSFV_REQ is the TRANSPORT block, not the frozen contract, so growing it
    ** is free; NSFRQE stays 64 B and untouched. */
    UINT      slot;         /* +34 in: probe slot index; out: slot claimed    */
    UINT      sexpect;      /* +38 SLOT probe: expected state (CS comparand)  */
    UINT      snew;         /* +3C SLOT probe: state to set                   */
} NSFV_REQ;                 /* +40 = 64 bytes                                */
NSF_SIZE_ASSERT(NSFV_REQ, 64);
NSFV_OFF_ASSERT(NSFV_REQ, func,   4);
NSFV_OFF_ASSERT(NSFV_REQ, token,  8);
NSFV_OFF_ASSERT(NSFV_REQ, rc,    12);
NSFV_OFF_ASSERT(NSFV_REQ, seq,   16);
NSFV_OFF_ASSERT(NSFV_REQ, ubuf,  20);
NSFV_OFF_ASSERT(NSFV_REQ, ulen,  24);
NSFV_OFF_ASSERT(NSFV_REQ, rsvd_pascb, 28);
NSFV_OFF_ASSERT(NSFV_REQ, rsvd_pasid, 32);
NSFV_OFF_ASSERT(NSFV_REQ, qstate, 36);
NSFV_OFF_ASSERT(NSFV_REQ, qinfl, 40);
NSFV_OFF_ASSERT(NSFV_REQ, qreap, 44);
NSFV_OFF_ASSERT(NSFV_REQ, rqeimg, 48);
NSFV_OFF_ASSERT(NSFV_REQ, slot,   52);
NSFV_OFF_ASSERT(NSFV_REQ, sexpect, 56);
NSFV_OFF_ASSERT(NSFV_REQ, snew,   60);

/* THE R8 PROBE'S COVERAGE ARGUMENT, AS AN ASSERT (M5-2d1b).
 *
 * asm/nsfvsvc.asm validates the caller's request block with exactly TWO TPROT
 * probes -- byte 0 and byte 63 -- and that covers all 20 key-0 stores into the
 * block ONLY because a 64-byte object straddles at most one page boundary and
 * so occupies at most the two pages probed.
 *
 * Grow NSFV_REQ past a page and the probes silently stop covering the stores,
 * with NOTHING about the check looking wrong -- the assembler is happy, the
 * link is clean, and the hole is back. So the dependency is a compile-time
 * assert rather than a comment, and it is named for what it PROTECTS rather
 * than for what it measures, so a failure says why it exists.
 *
 * (The 64-byte size itself is pinned by NSF_SIZE_ASSERT below, and snew@60
 * above fixes the last field; together they prove every store is inside the
 * probed span. This assert covers the remaining step -- span to page count.) */
#ifdef __MVS__
typedef char nsfv_r8_probe_covers_block[(sizeof(NSFV_REQ) <= 4096) ? 1 : -1];
#endif

/* ============================================================
 * NSFV_SLOT -- ONE request slot.  64 of them make the pool (M5-2b3, ADR-0042).
 *
 * Everything a single client needs for a single request in flight, so that two
 * clients share nothing but the claim discipline.  Through M5-2a all of this
 * lived once in the anchor and a second caller was turned away at the door
 * (RCNOREQ); the fields are the same, the ownership is not.
 *
 * req_state IS THE CLAIM WORD, at offset 0.  CS requires a fullword-aligned
 * operand: the slot array starts at a multiple of 8 (the header is 56 bytes
 * since M5-2b4, 48 before it) and the slot is 2144 bytes, also a multiple of
 * 8, so every slot -- and hence every claim word -- is doubleword aligned.
 * That is load-bearing, not cosmetic, and it is why the header grows by TWO
 * words rather than one.
 *
 * 2144 BYTES, DELIBERATELY NOT PADDED TO A POWER OF TWO.  Padding to 2560 or
 * 4096 would cost 26 KB or 120 KB of CSA to enable an index multiply the scan
 * does not perform: it walks a POINTER (LA Rslot,SLOTLEN(,Rslot)) bounded by a
 * count.  2144 fits an LA displacement (max 4095) and every field offset below
 * fits base-displacement addressing off a slot-base register.
 *
 * stage[] STAYS LAST, as ADR-0039 requires -- but the consequence has changed
 * and is worth stating rather than inheriting: an over-long move now runs into
 * the NEXT SLOT'S CLAIM WORD instead of into adjacent CSA.  That is a
 * different shape of hazard, not a smaller one; corrupting a live claim word
 * hands one slot to two owners.  The min(ulen, 2048) clamp in the SVC routine
 * is the only thing preventing it and is UNCHANGED from M5-2b1.  (Putting
 * stage[] first, so an overrun would land on rqe_guard, was considered and
 * rejected in ADR-0042: the guard is checked BEFORE dispatch against a value
 * stamped at allocation, so a write-in overrun would be caught only on the
 * NEXT request through that slot, if ever.)
 * ============================================================ */
typedef struct nsfv_slot {
    UINT      req_state;          /* +00 THE CS CLAIM WORD (NSFV_REQ_*)       */
    UINT      req_token;          /* +04 in: client token; out: echo (+1)     */
    ECB       reply_ecb;          /* +08 this client's WAIT target (CSA, key 0)*/
    void     *req_ascb;           /* +0C client ASCB (__xmpost target)        */
    UINT      req_asid;           /* +10 client ASID (ASCBASID, ASCB+X'24')   */
    UINT      xfunc;              /* +14 transform: NSFV_REQ_ECHO/_XFER/_RQE  */
    UINT      xlen;               /* +18 bytes in stage[] this chunk          */
    char      rqe[NSFV_RQE_SLOT]; /* +1C the 64-byte NSFRQE image (ADR-0041)  */
    char      rqe_guard[4];       /* +5C NSFREQX_GUARD, per slot now          */
    char      stage[NSFV_XFER_CHUNK]; /* +60 this client's CSA staging        */
} NSFV_SLOT;                      /* +860 = 2144 bytes                        */
NSF_SIZE_ASSERT(NSFV_SLOT, 2144);
NSFV_OFF_ASSERT(NSFV_SLOT, req_token,  4);
NSFV_OFF_ASSERT(NSFV_SLOT, reply_ecb,  8);
NSFV_OFF_ASSERT(NSFV_SLOT, req_ascb,  12);
NSFV_OFF_ASSERT(NSFV_SLOT, req_asid,  16);
NSFV_OFF_ASSERT(NSFV_SLOT, xfunc,     20);
NSFV_OFF_ASSERT(NSFV_SLOT, xlen,      24);
NSFV_OFF_ASSERT(NSFV_SLOT, rqe,       28);
NSFV_OFF_ASSERT(NSFV_SLOT, rqe_guard, 92);
NSFV_OFF_ASSERT(NSFV_SLOT, stage,     96);

/* ============================================================
 * NSFV_ANCHOR -- the CSA (SP=241, key 0) rendezvous block: a fixed global
 * header followed by the slot array (M5-2b3, ADR-0042).
 *
 * The split is what keeps the assembler manageable.  Without it every
 * per-slot field would need 64 EQUs or an index multiply; with it there is one
 * set of ANC* EQUs off the anchor base and one set of SL* EQUs off a slot-base
 * register, and the scan is a pointer walk between them.
 *
 * csasave[18] IS GONE.  Dead since M5-2b2 (the POST save area moved to the
 * SVRB's RBEXSAVE), kept only because removing it would have shifted every
 * later field for no benefit.  This is the layout move it was waiting for, and
 * with it goes b2's five-word self-check -- one-time evidence, collected, and
 * recorded in ADR-0038 (issue #61).
 *
 * nslots IS WRITTEN BY THE ALLOCATOR AND READ BY THE SCAN.  The SVC routine
 * bounds its walk by THIS field, never by its own EQU: the party that knows
 * how much storage exists is the party that allocated it, and a stale NSFVSVC
 * in CSA against a newer NSFS is a real, documented divergence (see
 * NSFV_ANCHOR_VER).
 *
 * Target layout, 4-byte pointers: 56-byte header + 64 x 2144 = 137,272 bytes,
 * one contiguous SP=241 GETMAIN.  Measured on MVSCE (b0): CSA total 2064 KB,
 * largest contiguous SP=241 GETMAIN >= 1 MB -- so the pool is 6.5 % of CSA.
 *
 * ============ ASSEMBLER MIRROR (asm/nsfvsvc.asm carries these as EQUs) ======
 *  global header
 *   ANCEYE     EQU  0    CL8  "NSFVANCR"
 *   ANCVER     EQU  8    F    version -- checked against ANCVERNO
 *   ANCFLAG    EQU 12    F    ANCHOR_ACTIVE = X'80000000'
 *                              ANCHOR_PROBE  = X'40000000' (#67)
 *   ANCSECB    EQU 16    F    server_ecb  (STC WAIT target, fallback)
 *   ANCSASCB   EQU 20    A    server_ascb (STC ASCB, POST target)
 *   ANCINFL    EQU 24    F    inflight
 *   ANCSERVD   EQU 28    F    served
 *   ANCREAPD   EQU 32    F    reaped
 *   ANCSEPTR   EQU 36    A    A(STC private key-8 wake ECB)
 *   ANCNSLOT   EQU 40    F    nslots -- THE SCAN'S BOUND
 *   ANCEXH     EQU 44    F    exhausted (ENOBUFS count, diagnostic)
 *   ANCCOLL    EQU 48    F    collisions (contended claims, M5-2b4)
 *   ANCRSV0    EQU 52    F    reserved -- header slack, deliberately unused
 *   ANCSLOTS   EQU 56         the slot array base
 *  per slot, off a slot-base register
 *   SLSTATE    EQU  0    F    req_state -- THE CS CLAIM WORD
 *   SLTOKEN    EQU  4    F    req_token
 *   SLRECB     EQU  8    F    reply_ecb
 *   SLRASCB    EQU 12    A    req_ascb
 *   SLASID     EQU 16    F    req_asid
 *   SLXFUNC    EQU 20    F    xfunc
 *   SLXLEN     EQU 24    F    xlen
 *   SLRQE      EQU 28    64   rqe
 *   SLRQEG     EQU 92    CL4  rqe_guard
 *   SLSTAGE    EQU 96    2048 stage
 *   SLOTLEN    EQU 2144       one slot, the scan's stride
 *  and the routine's own constant, which is NOT an offset:
 *   ANCVERNO   EQU 3          == NSFV_ANCHOR_VER; the routine rejects an
 *                                anchor whose ANCVER differs
 *   ANCPROBE   EQU X'40'      == NSFV_ANCHOR_PROBE's high byte, TM'd against
 *                                ANCFLAG(R2): does this server service the
 *                                probe verbs?  (issue #67)
 * ============================================================================ */
typedef struct nsfv_anchor {
    char      eye[8];             /* +00 "NSFVANCR"                           */
    UINT      version;            /* +08 NSFV_ANCHOR_VER -- routine checks it  */
    UINT      flags;              /* +0C NSFV_ANCHOR_ACTIVE, and since
                                  **     M5-2d1c NSFV_ANCHOR_PROBE -- the
                                  **     probe-verb capability bit the SVC
                                  **     routine gates on (issue #67).     */
    ECB       server_ecb;         /* +10 STC WAIT target (CSA, key 0) -- the
                                  **     fallback the Stage-0 probe STC uses  */
    void     *server_ascb;        /* +14 STC ASCB (__ascb(0) at startup)      */
    UINT      inflight;           /* +18 clients executing inside the routine */
    UINT      served;             /* +1C requests serviced (diagnostic)       */
    UINT      reaped;             /* +20 dead requests reclaimed (diagnostic) */
    void     *server_ecb_ptr;     /* +24 A(STC private key-8 wake ECB); ZERO
                                  **     means "not published" and the routine
                                  **     falls back to server_ecb, which keeps
                                  **     the four Stage-0 gates a regression
                                  **     rather than a rewrite (ADR-0041)     */
    UINT      nslots;             /* +28 slots actually allocated -- the bound
                                  **     the SVC routine's scan uses          */
    UINT      exhausted;          /* +2C times the pool was full (ENOBUFS).  A
                                  **     pool that is regularly full is a
                                  **     sizing fact and should not have to be
                                  **     inferred from client-side errnos.    */
    UINT      collisions;         /* +30 CONTENDED CLAIMS (M5-2b4, ADR-0042
                                  **     annotation): one per CS that FAILED
                                  **     during a claim scan.  See "COLLISIONS"
                                  **     below for what that does and does not
                                  **     distinguish, and why the increment is
                                  **     deliberately not interlocked.        */
    UINT      rsvd0;              /* +34 RESERVED -- header slack, on purpose.
                                  **     b3 left the header with none, which
                                  **     is the entire reason adding one
                                  **     diagnostic word in b4 costs a full
                                  **     Stage-0 round (every slot moves, the
                                  **     version bumps, every gate re-runs).
                                  **     M5-2c gets to spend this one instead. */
    NSFV_SLOT slots[NSFV_NSLOTS]; /* +38 the pool                             */
} NSFV_ANCHOR;                    /* 56 + 64*2144 = 137272 bytes              */
NSF_SIZE_ASSERT(NSFV_ANCHOR, 137272);
NSFV_OFF_ASSERT(NSFV_ANCHOR, version,        8);
NSFV_OFF_ASSERT(NSFV_ANCHOR, flags,         12);
NSFV_OFF_ASSERT(NSFV_ANCHOR, server_ecb,    16);
NSFV_OFF_ASSERT(NSFV_ANCHOR, server_ascb,   20);
NSFV_OFF_ASSERT(NSFV_ANCHOR, inflight,      24);
NSFV_OFF_ASSERT(NSFV_ANCHOR, served,        28);
NSFV_OFF_ASSERT(NSFV_ANCHOR, reaped,        32);
NSFV_OFF_ASSERT(NSFV_ANCHOR, server_ecb_ptr, 36);
NSFV_OFF_ASSERT(NSFV_ANCHOR, nslots,        40);
NSFV_OFF_ASSERT(NSFV_ANCHOR, exhausted,     44);
NSFV_OFF_ASSERT(NSFV_ANCHOR, collisions,    48);
NSFV_OFF_ASSERT(NSFV_ANCHOR, rsvd0,         52);
NSFV_OFF_ASSERT(NSFV_ANCHOR, slots,         56);

/* ============================================================
 * COLLISIONS -- what the counter counts, and what it cannot tell you.
 *
 * M5-2b3 built the pool and could not prove contention: a FAILED `CS` on a
 * slot word is invisible from outside the routine.  A client that scans, finds
 * slot K taken and moves on to K+1 is externally indistinguishable from one
 * that found K free, lost the compare and moved on -- and both are
 * indistinguishable from a client that simply started at K+1.  No arrangement
 * of clients and slots recovers that from the outside, so the only honest
 * answer is instrumentation.
 *
 * `collisions` is incremented ONCE PER FAILED CS in the claim scan.  What it
 * therefore means is precisely:
 *
 *     A CLAIM ATTEMPT FOUND A SLOT THAT WAS NOT FREE AT THE INSTANT OF THE
 *     COMPARE.
 *
 * That is the statement the two-client gate needs -- with one client the scan
 * finds slot 0 free every time and the counter never moves (TSTRQXC's no-PARM
 * run asserts exactly that, as the negative control), while two address spaces
 * hammering the pool step over each other's claims constantly.
 *
 * IT DOES NOT SEPARATE "the slot was already busy" FROM "I lost a simultaneous
 * race", and it cannot: `CS` reports only the value it actually found, and the
 * routine performs no load before the compare that a second value could be
 * compared against.  Adding one would sharpen the counter into "a slot
 * observed FREE was gone by the time I compared" -- the true lost-race count
 * -- at the price of a second counter and a second word.  Recorded here as the
 * refinement M5-2c may spend `rsvd0` on, NOT done here; the gate that needs
 * proving is that two address spaces really share the pool, and that is what
 * this counter answers.
 *
 * (Worth knowing when reading a number from this counter: the MVSCE stand runs
 * Hercules with NUMCPU 2 and MVS dispatches on both, so a genuinely
 * simultaneous compare is physically possible here and not merely defended
 * against on paper.)
 *
 * THE INCREMENT IS DELIBERATELY NOT INTERLOCKED -- a plain L/LA/ST, not the
 * CS loop `exhausted` next door uses.  Two address spaces can lose an update,
 * so the counter UNDER-reports and never over-reports: "collisions >= 1" stays
 * sound, "collisions == N" is not a number to build on.  Aligned fullword
 * stores do not tear, so a reader never sees a half-written value.  The reason
 * to accept that is the path: this sits inside the claim scan, which is the
 * one hot loop in the routine, and paying an interlocked update there to make
 * a DIAGNOSTIC exact would be the wrong trade.  `exhausted` can afford its CS
 * loop because a full pool is a rare event.  DO NOT "fix" this into a CS loop.
 * ============================================================ */

#endif /* NSFVSVC_H */
