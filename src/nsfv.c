/*
 * nsfv.c -- M5 Stage-0a' SVC cross-AS probe: the probe STC (NSFV).
 *
 * ADR-0038 (supersedes ADR-0036's SSI transport).  The target address space
 * for the private-SVC transport probe.  It owns the CSA rendezvous anchor,
 * __loadhi's the SVC routine (NSFVSVC) into CSA, publishes the anchor address
 * into the routine, then STEALS an unused installation SVC slot (200-255) to
 * point at the routine -- so an UNAUTHORIZED problem-state client can reach the
 * stack by issuing the SVC (the APF-free unauthorized->authorized transition
 * the SSI path lacked).  It runs an event loop that WAITs (supervisor / key 0)
 * on {server_ecb, console-CIB ECB}, services the one pending request (increment
 * the token, bump a served counter), and wakes the client with __xmpost.  No
 * NSFRQE, no socket, no protocol: this proves the transport mechanics on an
 * empty token before M5-2 rides the real request over them.
 *
 * The STOLEN SVC SLOT IS RESTORED at stop AND on abend (a dangling stolen slot
 * corrupts that SVC number system-wide -- stricter than Stage-0a's SSCT leak).
 * Restore merely redirects the slot, so the ESTAE may restore under RTM (unlike
 * Stage-0a's SSCT), which also makes a fresh S NSFV after an abend restartable.
 *
 * Modelled on Stage-0a's SSI probe STC (retired; ADR-0036) -- STC lifecycle,
 * in-flight drain, ESTAE -- and the
 * CBT/mvs38j-ip SVCTABLE steal (STCPSVC0/STCPSVC9: CVT->CVTABEND->SCVTSVCT).
 * The STC (not the client) self-authorises via clib_apf_setup (SVC 244) for
 * __loadhi / key-0 CSA / the SVCTABLE store -- NSF.LINKLIB need not be APF.
 * The RED LINE is that the CLIENT stays unauthorised; the STC does not.
 */
#ifndef NSFV_VERSION
#define NSFV_VERSION "0.1.0-probe"
#endif

#include "nsfvsvc.h"
#include "nsfreqx.h"   /* the shared, host-pinned classifier + slot rules */
#include <string.h>
#include <clibos.h>          /* __super/__prob/__ascb/__xmpost/getmain/loadhi   */
#include <clibwto.h>         /* wtof                                            */
#include <clibcib.h>         /* COM / CIB / __gtcom / __cibget / __cibdel       */
#include <clibstae.h>        /* __estae, ESTAE_CREATE/DELETE                    */
#include <clibsdwa.h>        /* SDWA, SDWACWT                                   */
#include <cvt.h>             /* CVT, CVTPTR                                     */
#include <ihascvt.h>         /* SCVT (scvtsvct), SVCTABLE, SVCENTRY             */
#include <ihaasvt.h>         /* ASVT (asvtmaxu, asvtenty) -- the liveness guard */

/* ============================================================
 * STC control block (main's stack).
 * ============================================================ */
typedef struct nsfv_stc {
    char          eye[8];        /* "**NSFV**"                                 */
    unsigned      flags;         /* NSFV_STC_ACTIVE                            */
    NSFV_ANCHOR  *anchor;        /* CSA anchor (SP=241)                        */
    void         *router_lpa;    /* NSFVSVC CSA load base (freemain)           */
    void         *router_epa;    /* NSFVSVC entry (SVC slot + anchor patch)    */
    unsigned      router_size;   /* NSFVSVC module size                        */
    SVCENTRY     *svc_slot;      /* the stolen SVCTABLE entry (restore target) */
    unsigned char svc_saved[8];  /* original 8-byte SVCTABLE entry             */
    int           svc_stolen;    /* 1 while the slot is stolen                 */
} NSFV_STC;

#define NSFV_STC_ACTIVE   0x80000000U
#define NSFV_SHUT_NORMAL  0
#define NSFV_SHUT_ABEND   1

static void nsfv_shutdown(NSFV_STC *stc, int mode);

/* ============================================================
 * CSA anchor allocation (SP=241, key 0).  Caller task must be APF authorised.
 * ============================================================ */
static NSFV_ANCHOR *
nsfv_anchor_alloc(void)
{
    NSFV_ANCHOR   *anchor;
    unsigned char  savekey;

    if (__super(PSWKEY0, &savekey)) return NULL;
    anchor = (NSFV_ANCHOR *)getmain((unsigned)sizeof(NSFV_ANCHOR), 241);
    if (anchor) {
        memset(anchor, 0, sizeof(NSFV_ANCHOR));
        memcpy(anchor->eye, "NSFVANCR", 8);
        anchor->version   = NSFV_ANCHOR_VER;
        anchor->flags     = NSFV_ANCHOR_ACTIVE;
        {   /* Publish the slot count the SVC routine's scan is bounded by,
            ** and stamp every slot's guard.  memset already left the states
            ** FREE, which is what the claim CS compares against. */
            unsigned i;
            anchor->nslots    = NSFV_NSLOTS;
            anchor->exhausted = 0;
            for (i = 0; i < NSFV_NSLOTS; i++)
                memcpy(anchor->slots[i].rqe_guard, NSFREQX_GUARD,
                       NSFREQX_GUARDLEN);
        }
    }
    __prob(savekey, NULL);
    return anchor;
}

static void
nsfv_anchor_free(NSFV_ANCHOR *anchor)
{
    unsigned char savekey;

    if (!anchor) return;
    if (__super(PSWKEY0, &savekey)) return;
    /* Invalidate the eye catcher BEFORE releasing: freed SP=241 storage is
    ** reused, not zeroed, so a client still parked in the routine must not
    ** accept the reused anchor as valid (its wake path revalidates the eye). */
    memset(anchor->eye, 0, sizeof(anchor->eye));
    freemain(anchor);
    __prob(savekey, NULL);
}

/* ============================================================
 * Load the SVC routine into CSA (__loadhi: supervisor + key 0).
 * ============================================================ */
static int
nsfv_router_load(NSFV_STC *stc)
{
    unsigned char savekey;
    void         *lpa;
    void         *epa;
    unsigned      size;
    int           rc;

    if (__super(PSWKEY0, &savekey)) {
        wtof("NSFV026E CANNOT ENTER SUPERVISOR FOR SVC LOAD");
        return -1;
    }
    rc = __loadhi(NSFV_ROUTER_MOD, &lpa, &epa, &size);
    if (rc) {
        __prob(savekey, NULL);
        wtof("NSFV027E CANNOT LOAD %s INTO CSA, RC=%d", NSFV_ROUTER_MOD, rc);
        return -1;
    }
    stc->router_lpa  = lpa;
    stc->router_epa  = epa;
    stc->router_size = size;

    /* Publish the anchor address into the routine's NSFVANCH word (offset
    ** NSFV_ANCH_OFF from the entry).  Written ONCE here, before the slot is
    ** stolen, then read-only -- so no invocation ever sees an unpatched value
    ** and reentrancy is preserved (ADR-0038 3). */
    *(void **)((unsigned char *)epa + NSFV_ANCH_OFF) = (void *)stc->anchor;

    __prob(savekey, NULL);
    return 0;
}

static void
nsfv_router_unload(NSFV_STC *stc)
{
    unsigned char savekey;

    if (!stc->router_lpa) return;
    if (__super(PSWKEY0, &savekey)) return;
    freemain(stc->router_lpa);
    stc->router_lpa  = NULL;
    stc->router_epa  = NULL;
    stc->router_size = 0;
    __prob(savekey, NULL);
}

/* ============================================================
 * Steal an unused SVCTABLE slot (CVT -> CVTABEND -> SCVTSVCT -> +svc#*8, the
 * STCPSVC0 walk) and point it at the CSA routine.  Saves the original 8-byte
 * entry for restore.  Safety: steal NSFV_SVCNUM only if it is genuinely FREE --
 * a free installation slot (200-255) points at the common "invalid SVC" routine,
 * whose entry point is the one SHARED BY THE MOST slots in 200-255 (unused slots
 * dominate).  If NSFV_SVCNUM is not free, refuse and log the free landscape so a
 * good number can be chosen.
 * ============================================================ */
static int
nsfv_svc_steal(NSFV_STC *stc)
{
    unsigned char  savekey;
    CVT           *cvt;
    SCVT          *scvt;
    SVCTABLE      *svct;
    SVCENTRY      *slot;
    unsigned char *b;
    void          *unused_ep;
    unsigned       i, j, best, free_cnt;
    int            hi_free;

    if (__super(PSWKEY0, &savekey)) {
        wtof("NSFV028E CANNOT ENTER SUPERVISOR FOR SVC STEAL");
        return -1;
    }

    cvt  = CVTPTR;
    scvt = (SCVT *)cvt->cvtabend;
    svct = (SVCTABLE *)scvt->scvtsvct;

    /* The unused-SVC marker: the entry point shared by the most slots in
    ** 200-255 (free installation SVCs all point at the invalid-SVC routine). */
    unused_ep = svct->svcentry[255].svcepa;
    best      = 0;
    for (i = 200; i <= 255; i++) {
        void    *ep = svct->svcentry[i].svcepa;
        unsigned c  = 0;
        for (j = 200; j <= 255; j++)
            if (svct->svcentry[j].svcepa == ep) c++;
        if (c > best) { best = c; unused_ep = ep; }
    }

    /* The free landscape (diagnostics: highest free slot + count). */
    hi_free  = -1;
    free_cnt = 0;
    for (i = 255; i >= 200; i--) {
        if (svct->svcentry[i].svcepa == unused_ep) {
            free_cnt++;
            if (hi_free < 0) hi_free = (int)i;
        }
        if (i == 200) break;
    }

    slot = &svct->svcentry[NSFV_SVCNUM];
    if (slot->svcepa != unused_ep) {
        __prob(savekey, NULL);
        wtof("NSFV029E SVC %u IN USE (EP %08X, UNUSED EP %08X) -- NOT STOLEN",
             (unsigned)NSFV_SVCNUM, (unsigned)slot->svcepa,
             (unsigned)unused_ep);
        wtof("NSFV029E %u FREE SLOTS IN 200-255, HIGHEST FREE = %d",
             free_cnt, hi_free);
        return -1;
    }

    /* Save the original entry, then install: EP = routine, attributes = Type 3
    ** (X'C0'), no APF (so an unauthorised caller may issue it), no locks. */
    memcpy(stc->svc_saved, slot, 8);
    slot->svcepa = stc->router_epa;
    b = (unsigned char *)slot + 4;
    b[0] = 0xC0;                        /* SVCTYPE3, svcapf=0, preemptive     */
    b[1] = 0;                           /* svcattribute                        */
    b[2] = 0;                           /* svclock                             */
    b[3] = 0;
    stc->svc_slot   = slot;
    stc->svc_stolen = 1;

    __prob(savekey, NULL);
    wtof("NSFV034I SVC %u STOLEN (OLD EP %08X, NEW EP %08X, %u FREE)",
         (unsigned)NSFV_SVCNUM, *(unsigned *)stc->svc_saved,
         (unsigned)stc->router_epa, free_cnt);
    return 0;
}

/* ============================================================
 * Restore the stolen slot to its original 8-byte entry.  Idempotent.  Merely
 * redirects the slot (does not free the CSA code), so it is safe under RTM.
 * ============================================================ */
static void
nsfv_svc_restore(NSFV_STC *stc)
{
    unsigned char savekey;

    if (!stc->svc_stolen || !stc->svc_slot) return;
    if (__super(PSWKEY0, &savekey)) {
        wtof("NSFV097E CANNOT ENTER SUPERVISOR -- SVC SLOT NOT RESTORED");
        return;
    }
    memcpy(stc->svc_slot, stc->svc_saved, 8);
    stc->svc_stolen = 0;
    __prob(savekey, NULL);
    wtof("NSFV095I SVC %u RESTORED", (unsigned)NSFV_SVCNUM);
}

/* ============================================================
 * Client liveness -- the Stage-0c guard (ADR-0040).
 *
 * Answers, for the identity the SVC routine recorded at entry (req_ascb +
 * req_asid, both taken from control blocks, neither forgeable by the client),
 * whether that address space is still there.  Called immediately before every
 * reply POST; a DEAD answer reaps the request instead of posting into it.
 *
 * ASID n lives at asvtenty[n-1] (ihaasvt.h).  An AVAILABLE entry (high bit)
 * belongs to no address space and carries the next available entry's address,
 * so the BIT -- never a zero test -- separates the two kinds.
 *
 * THE COMPARISON IS ON THE ASCB ADDRESS ALONE.  Nothing is read out of the
 * ASCB: the recorded pointer may be stale, an ASCB block returns to SQA at
 * memterm and is handed out again (ufsd measured a restart landing on its own
 * predecessor's block), so a field read out of it answers with a stranger's
 * data.  Comparing addresses cannot fault and needs no field offsets.
 *
 * UNKNOWN is NOT dead.  Every case where the lookup cannot be completed --
 * no ASCB, no CVT/ASVT, an ASID outside 1..asvtmaxu -- answers UNKNOWN, and
 * an UNKNOWN request is neither posted into nor reclaimed.  The asymmetry is
 * the whole point (ADR-0040 2): a dead client wrongly called live leaks a slot,
 * while a LIVE client wrongly called dead has its storage freed underneath it.
 * "I could not check" must never be answered with "go ahead and free it".
 *
 * Runs inside the caller's key-0 window (the CVT and the ASVT are fetch-
 * accessible; no storage is written).
 * ============================================================ */
#define NSFV_ASVT_AVAIL  0x80000000U   /* ASVTAVAI: ASID available, unassigned */

#define NSFV_CL_LIVE     0
#define NSFV_CL_DEAD     1
#define NSFV_CL_UNKNOWN  2

/* Is any slot published?  The executive's drain loop runs until none is, so
 * a request the guard declines (-> HELD) must not keep it spinning. */
static int
nsfv_any_pending(const NSFV_ANCHOR *anchor)
{
    unsigned i;

    for (i = 0; i < anchor->nslots; i++) {
        if (anchor->slots[i].req_state == NSFV_REQ_PENDING) return 1;
    }
    return 0;
}

/* How many slots are not FREE -- what the operator wants from a pool, where
 * the old single STATE= number no longer means anything. */
static unsigned
nsfv_busy_slots(const NSFV_ANCHOR *anchor)
{
    unsigned i, busy = 0;

    for (i = 0; i < anchor->nslots; i++) {
        if (anchor->slots[i].req_state != NSFV_REQ_FREE) busy++;
    }
    return busy;
}

static int
nsfv_client_state(const NSFV_SLOT *slot)
{
    CVT  *cvt;
    ASVT *asvt;

    /* M5-2b3: the ARITHMETIC is nsfreqx_classify, host-pinned by TSTREQX --
    ** the same function the production STC uses.  This file used to carry a
    ** hand-written second copy of the same truth table, which is exactly what
    ** the extraction removes: two copies of a classifier drift, and the row
    ** that drifts is the one no live run happens to hit. */
    cvt = *(CVT **)16;
    if (!cvt) return NSFREQX_CL_UNKNOWN;
    asvt = (ASVT *)cvt->cvtasvt;
    if (!asvt) return NSFREQX_CL_UNKNOWN;

    return nsfreqx_classify((UINT)slot->req_ascb, slot->req_asid,
                            (UINT)asvt->asvtmaxu,
                            (const UINT *)asvt->asvtenty);
}

/* ============================================================
 * Reap a request whose client the guard proved DEAD (ADR-0040 1).
 *
 * Everything the dead client would have released on its way out: the CSA
 * staging buffer and its descriptors, the reply ECB, the recorded identity,
 * the in-flight count, and last of all the slot itself.
 *
 * ORDER MATTERS.  The slot is published FREE only after the storage it owns
 * has been cleared and the count given back, because the SVC routine's
 * slot-take tests req_state: a new client must never see FREE while this
 * request's staging is still claimed.  The in-flight decrement is guarded
 * against underflow and is safe as a test-then-decrement precisely because
 * the slot is still busy at that moment -- no other client can be inside the
 * routine incrementing it, and the dead one will never decrement.
 * ============================================================ */
static int
nsfv_reap(NSFV_ANCHOR *anchor, NSFV_SLOT *slot, UINT observed, int verdict)
{
    UINT want = observed;

    /* Same single encoding the production STC uses -- the probe has no storage
     * trust check of its own, so storage_ok is 1 and liveness decides. */
    if (!nsfreqx_reap_ok(observed, verdict, 1)) {
        return 0;
    }

    /* M5-2b3: TAKE OWNERSHIP BY CS BEFORE CLEARING (ADR-0042 2).  With one
    ** slot the reaper raced with nobody; with a pool it is a THIRD OBSERVER,
    ** and between observing this slot as reapable and reclaiming it the owner
    ** can complete, release, and a new client can claim.  Clearing storage we
    ** do not yet own is that race in its most damaging form.  CLAIMED is a
    ** state nobody else can claim, so the clear below is unraced and the final
    ** store to FREE is ours alone to make. */
    if (__cas((unsigned *)&slot->req_state, (unsigned *)&want,
              NSFV_REQ_CLAIMED) != 0) {
        return 0;                           /* raced -- not ours to reclaim   */
    }

    memset(slot->stage, 0, sizeof(slot->stage));
    slot->xlen      = 0;
    slot->xfunc     = 0;
    slot->req_token = 0;
    slot->reply_ecb = 0;
    slot->req_ascb  = NULL;
    slot->req_asid  = 0;

    if (anchor->inflight != 0U) __udec(&anchor->inflight);

    anchor->reaped++;
    slot->req_state = NSFV_REQ_FREE;        /* published LAST */
    return 1;
}

/* ============================================================
 * Service the one pending request (cross-AS, supervisor state).  Dispatch on
 * the transform the SVC routine staged: ECHO increments the token; XFER applies
 * a byte-wise +1 to the CSA staging buffer's xlen bytes (ADR-0039 -- a trivial,
 * position-sensitive transform so a short/wrong/offset copy is visible).  The
 * SVC routine owns the slot's FREE<->PENDING/DONE lifecycle; this only moves
 * PENDING -> DONE.
 * ============================================================ */
static void
nsfv_service(NSFV_ANCHOR *anchor, NSFV_SLOT *slot)
{
    unsigned char savekey;
    int           cl    = -1;              /* the guard's answer, for the WTO  */
    unsigned      lascb = 0;
    unsigned      lasid = 0;
    unsigned      linfl = 0;
    unsigned      lreap = 0;

    if (__super(PSWKEY0, &savekey)) return;

    if (slot->req_state == NSFV_REQ_PENDING) {
        void  *ca = slot->req_ascb;

        /* ADR-0040: establish the client is alive BEFORE anything else.  The
        ** reply POST is the hazard -- __xmpost dereferences the recorded ASCB,
        ** and an ASCB block of an ended address space is reused SQA -- so the
        ** check gates the post, and reclamation falls out of the same answer.
        ** Nothing else in the STC may POST a client without passing here. */
        cl    = nsfv_client_state(slot);
        lascb = (unsigned)ca;
        lasid = slot->req_asid;

        if (cl == NSFREQX_CL_DEAD) {
            nsfv_reap(anchor, slot, NSFV_REQ_PENDING, cl);
            linfl = anchor->inflight;
            lreap = anchor->reaped;
        } else if (cl == NSFREQX_CL_UNKNOWN) {
            /* Neither post nor reap.  HELD keeps the slot busy to the SVC
            ** routine while taking it off the executive's work list -- the
            ** drain loop below spins while req_state is PENDING. */
            slot->req_state = NSFV_REQ_HELD;
        } else {
            /* LIVE: the Stage-0a'/0b path, unchanged.  ca is non-NULL here by
            ** construction -- a request with no recorded ASCB answers UNKNOWN
            ** above, so the post never runs on a null target. */
            if (slot->xfunc == NSFV_REQ_XFER) {
                UINT n = slot->xlen;
                UINT k;
                if (n > NSFV_XFER_CHUNK) n = NSFV_XFER_CHUNK;  /* clamp        */
                for (k = 0u; k < n; k++)
                    slot->stage[k] = (char)(slot->stage[k] + 1);
            } else {
                slot->req_token = slot->req_token + 1u;        /* ECHO         */
            }
            anchor->served++;
            slot->req_state = NSFV_REQ_DONE;    /* PENDING -> DONE             */
            __xmpost(ca, &slot->reply_ecb, 0);  /* wake the parked client      */
        }
    }

    __prob(savekey, NULL);

    /* WTO outside the key-0 window (every other message in this file does the
    ** same).  A held request is reported once: the STC only re-evaluates it on
    ** the next wake, and the slot stays busy until the client releases it. */
    if (cl == NSFV_CL_DEAD)
        wtof("NSFV050I CLIENT DEAD (ASCB=%08X ASID=%04X) -- REQUEST REAPED,"
             " INFLIGHT=%u REAPED=%u", lascb, lasid, linfl, lreap);
    else if (cl == NSFV_CL_UNKNOWN)
        wtof("NSFV051W CLIENT LIVENESS UNKNOWN (ASCB=%08X ASID=%04X)"
             " -- REQUEST HELD, NOT REAPED", lascb, lasid);
}

static void
nsfv_server_ecb_reset(NSFV_ANCHOR *anchor)
{
    unsigned char savekey;

    if (__super(PSWKEY0, &savekey)) return;
    anchor->server_ecb = 0;
    __prob(savekey, NULL);
}

/* ============================================================
 * In-flight drain.  Poll anchor->inflight to zero before freeing CSA.  A
 * client parked in the routine bails once it sees ACTIVE cleared, so each poll
 * re-posts its reply ECB (the SVC path has no per-client STIMER; the STC wakes
 * parked clients on quiesce -- ADR-0038 5).  A drain timeout means "retain
 * CSA", never "free anyway".  anchor->inflight is written from another address
 * space -- volatile, read from problem state (no fetch protection).
 * ============================================================ */
#define NSFV_DRAIN_POLL    10U   /* 0.10 s per poll                            */
#define NSFV_DRAIN_MAX    100U   /* 100 * 0.10 s = 10 s ceiling                */
#define NSFV_DRAIN_SETTLE  20U   /* 0.20 s after inflight reaches zero         */

static void
nsfv_pause(unsigned hsec)
{
    ECB local = 0;                          /* nobody posts it; the timer does */
    ecb_timed_wait(&local, hsec, 0);
}

static void
nsfv_wake_parked(NSFV_ANCHOR *anchor)
{
    unsigned char savekey;
    unsigned      i;

    if (__super(PSWKEY0, &savekey)) return;
    /* EVERY claimed slot (M5-2b3).  The single-slot version nudged one client;
    ** with a pool, a drain that woke one and then timed out on the rest would
    ** look exactly like a hang.
    **
    ** ADR-0040: the nudge is a POST like any other, so it goes only to a
    ** client the guard confirms LIVE -- a client that died in flight is
    ** exactly what this loop is draining, and posting through its ASCB is the
    ** failure the guard exists to prevent.  Silent by design: the drain polls
    ** up to 100 times and a message per poll would bury the shutdown. */
    for (i = 0; i < anchor->nslots; i++) {
        NSFV_SLOT *slot = &anchor->slots[i];

        if (slot->req_state == NSFV_REQ_FREE) continue;
        if (!slot->req_ascb)                  continue;
        if (nsfv_client_state(slot) != NSFREQX_CL_LIVE) continue;
        __xmpost(slot->req_ascb, &slot->reply_ecb, 0);
    }
    __prob(savekey, NULL);
}

static int
nsfv_drain(NSFV_ANCHOR *anchor)
{
    volatile unsigned *inflight = &anchor->inflight;
    unsigned           n;

    for (n = 0; n < NSFV_DRAIN_MAX; n++) {
        if (*inflight == 0) {
            nsfv_pause(NSFV_DRAIN_SETTLE);
            return (*inflight == 0);         /* re-check: catch a racing entry */
        }
        nsfv_wake_parked(anchor);            /* nudge the parked client to bail */
        nsfv_pause(NSFV_DRAIN_POLL);
    }
    return 0;
}

/* ============================================================
 * MODIFY / STOP handling.
 * ============================================================ */
static void
nsfv_process_cib(NSFV_STC *stc, CIB *cib)
{
    if (cib->cibverb == CIBSTOP) {
        wtof("NSFV010I NSFV STOP RECEIVED");
        stc->flags &= ~NSFV_STC_ACTIVE;
        return;
    }
    if (cib->cibverb == CIBMODFY) {
        if (cib->cibdatln >= 4 && memcmp(cib->cibdata, "STOP", 4) == 0) {
            wtof("NSFV010I NSFV STOP RECEIVED (MODIFY)");
            stc->flags &= ~NSFV_STC_ACTIVE;
            return;
        }
        if (stc->anchor)
            wtof("NSFV002I NSFV SERVED=%u INFLIGHT=%u REAPED=%u BUSY=%u"
                 " EXH=%u",
                 stc->anchor->served, stc->anchor->inflight,
                 stc->anchor->reaped, nsfv_busy_slots(stc->anchor),
                 stc->anchor->exhausted);
        else
            wtof("NSFV002I NSFV NO ANCHOR");
        return;
    }
    /* CIBSTART and others: ignore. */
}

/* ============================================================
 * Shutdown -- the ONE teardown path (also the ESTAE-abend path).
 * ============================================================ */
static void
nsfv_shutdown(NSFV_STC *stc, int mode)
{
    NSFV_ANCHOR   *anchor;
    unsigned char  savekey;

    /* Delete ESTAE first: no re-entrant recovery if shutdown itself faults. */
    __estae(ESTAE_DELETE, NULL, NULL);

    anchor = stc->anchor;

    /* Close the door BEFORE anything is freed: restore the SVC slot so no NEW
    ** client is dispatched into the routine (about to be FREEMAINed on the
    ** clean path), then clear ACTIVE so a client already parked bails on its
    ** next wake.  Restoring the slot only redirects it -- safe even under RTM
    ** (a foreign PSW inside the routine keeps its own R6). */
    nsfv_svc_restore(stc);

    if (anchor) {
        if (__super(PSWKEY0, &savekey)) {
            wtof("NSFV097E CANNOT ENTER SUPERVISOR STATE -- CSA RETAINED");
            goto done;                       /* free nothing */
        }
        anchor->flags &= ~NSFV_ANCHOR_ACTIVE;
        __prob(savekey, NULL);
    }

    if (!anchor)
        goto done;

    if (mode == NSFV_SHUT_ABEND) {
        /* Under RTM we cannot drain and must not free: a foreign PSW may still
        ** be executing inside the routine or touching the anchor.  The slot is
        ** already restored (safe); leave the CSA module + anchor and percolate.
        ** A fresh S NSFV re-steals the restored slot cleanly (only the orphaned
        ** CSA leaks until IPL) -- ADR-0038. */
        wtof("NSFV098W ABEND SHUTDOWN -- CSA RETAINED (SLOT RESTORED)");
        goto done;
    }

    /* Drain in-flight clients out of the routine. */
    if (!nsfv_drain(anchor)) {
        wtof("NSFV098W %u CLIENT(S) STILL IN FLIGHT -- CSA RETAINED",
             anchor->inflight);
        goto done;                           /* free nothing */
    }

    if (stc->router_lpa) {
        nsfv_router_unload(stc);
        wtof("NSFV036I SVC ROUTINE UNLOADED");
    }
    nsfv_anchor_free(anchor);
    stc->anchor = NULL;

done:
    wtof("NSFV011I NSFV SHUTDOWN COMPLETE");
}

/* ============================================================
 * ESTAE recovery.  Restore the slot + clear ACTIVE (NSFV_SHUT_ABEND), then
 * percolate for the MVS dump.  CSA is NOT freed under RTM.
 * ============================================================ */
static void
nsfv_recover(SDWA *sdwa)
{
    NSFV_STC *stc;

    if (!sdwa) return;
    stc = (NSFV_STC *)sdwa->SDWAPARM;

    wtof("NSFV900E NSFV ABEND INTERCEPTED -- EMERGENCY SHUTDOWN");

    if (stc) {
        stc->flags &= ~NSFV_STC_ACTIVE;
        nsfv_shutdown(stc, NSFV_SHUT_ABEND);
    }

    sdwa->SDWARCDE = SDWACWT;                /* percolate: MVS dumps + ends AS  */
}

/* ============================================================
 * main
 * ============================================================ */
int
main(int argc, char **argv)
{
    NSFV_STC      stc;
    NSFV_ANCHOR  *anchor;
    COM          *com;
    CIB          *cib;
    unsigned     *ecblist[3];               /* {console, server_ecb} + sentinel */
    unsigned      count;
    int           rc;

    (void)argc;

    memset(&stc, 0, sizeof(stc));
    memcpy(stc.eye, "**NSFV**", 8);
    stc.flags = NSFV_STC_ACTIVE;

    /* --- Console interface --- */
    com = __gtcom();
    if (!com) {
        wtof("NSFV090E UNABLE TO INITIALIZE CONSOLE INTERFACE");
        return 8;
    }
    __cibset(5);                            /* CIBSTART + up to 4 MODIFYs       */

    /* --- APF authorization (SVC 244 -- STC side only; the CLIENT is NOT
    ** authorized -- ADR-0038 red line).  Needed for __loadhi / key-0 CSA /
    ** the SVCTABLE store. --- */
    rc = clib_apf_setup(argv[0]);
    if (rc) {
        wtof("NSFV091E APF SETUP FAILED RC=%d", rc);
        return 8;
    }

    /* --- ESTAE recovery (established before the slot is stolen) --- */
    __estae(ESTAE_CREATE, (void *)nsfv_recover, &stc);

    wtof("NSFV000I NSFV SVC PROBE %s STARTING (SVC %u)",
         NSFV_VERSION, (unsigned)NSFV_SVCNUM);

    /* --- CSA anchor --- */
    anchor = nsfv_anchor_alloc();
    if (!anchor) {
        __estae(ESTAE_DELETE, NULL, NULL);
        wtof("NSFV093E CANNOT ALLOCATE CSA ANCHOR");
        return 8;
    }
    stc.anchor = anchor;

    /* Record the STC ASCB in the anchor (the routine's __xmpost target). */
    {
        unsigned char savekey;
        if (!__super(PSWKEY0, &savekey)) {
            anchor->server_ascb = __ascb(0);
            __prob(savekey, NULL);
        }
    }

    /* --- Load the routine, publish the anchor, then steal the slot --- */
    if (nsfv_router_load(&stc)) {
        nsfv_shutdown(&stc, NSFV_SHUT_NORMAL);
        return 8;
    }
    wtof("NSFV035I SVC ROUTINE LOADED AT %08X", (unsigned)stc.router_epa);

    if (nsfv_svc_steal(&stc)) {
        nsfv_shutdown(&stc, NSFV_SHUT_NORMAL);   /* no clients yet: drains now  */
        return 8;
    }

    wtof("NSFV001I NSFV READY -- ANCHOR=%08X ASCB=%08X",
         (unsigned)anchor, (unsigned)anchor->server_ascb);

    /* --- Main event loop --- */
    while (stc.flags & NSFV_STC_ACTIVE) {
        while ((cib = __cibget()) != NULL) {
            nsfv_process_cib(&stc, cib);
            __cibdel(cib);
            if (!(stc.flags & NSFV_STC_ACTIVE)) break;
        }
        if (!(stc.flags & NSFV_STC_ACTIVE)) break;

        /* Service the pending request: reset server_ecb, then double-check for
        ** a request that arrived between the last service and the reset
        ** (ADR-0022 reset-before-WAIT + double-check-drain).  A request the
        ** guard declines to service leaves PENDING for HELD (ADR-0040 6), so
        ** this loop terminates instead of spinning on it. */
        do {
            unsigned i;
            for (i = 0; i < anchor->nslots; i++) {
                if (anchor->slots[i].req_state == NSFV_REQ_PENDING)
                    nsfv_service(anchor, &anchor->slots[i]);
            }
            nsfv_server_ecb_reset(anchor);
        } while (nsfv_any_pending(anchor));

        if (!(stc.flags & NSFV_STC_ACTIVE)) break;

        /* WAIT on the console CIB ECB and the server_ecb.  server_ecb is in CSA
        ** (key 0), so WAIT in supervisor state. */
        count = 0;
        if (com->comecbpt) {
            ecblist[count++] = (unsigned *)com->comecbpt;
        }
        ecblist[count++] = (unsigned *)&anchor->server_ecb;
        ecblist[count - 1] = (unsigned *)((unsigned)ecblist[count - 1]
                                          | 0x80000000U);
        ecblist[count] = NULL;

        {
            unsigned char savekey;
            if (!__super(PSWKEY0, &savekey)) {
                __asm__("WAIT ECBLIST=(%0)" : : "r"(ecblist));
                __prob(savekey, NULL);
            }
        }
    }

    nsfv_shutdown(&stc, NSFV_SHUT_NORMAL);       /* clean STOP */
    return 0;
}
