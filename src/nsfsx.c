/* ==========================================================================
 * nsfsx.c -- M5-2a: the NSFS STC's cross-address-space request transport
 *            (ADR-0041 + addendum).
 *
 * Owns the CSA anchor and the private SVC, and turns a request that arrived
 * from another address space into an ordinary nsfreq_dispatch() call on the
 * executive.  See include/nsfsx.h for the contract and for why the plumbing
 * here is modelled on src/nsfv.c rather than extracted from it.
 *
 * The three hops (ADR-0041 1) land here as:
 *   drain-in   CSA slot  -> STC-private NSFRQE  -> nsfreq_dispatch
 *   drain-out  private   -> CSA slot (result fields) -> guard -> reply POST
 *
 * The executive dispatches the PRIVATE copy, so soc_complete's SVC 2 POST
 * targets ordinary key-8 STC storage and nsfsoc.c is untouched.
 * ========================================================================== */

#include "nsfsx.h"
#include "nsfvsvc.h"          /* NSFV_ANCHOR / NSFV_REQ_* / NSFV_SVCNUM        */
#include "nsfreq.h"           /* NSFRQE, nsfreq_dispatch                       */
#include "nsfreqx.h"          /* the host-tested field rules + guard truth table */
#include "nsfevtp.h"          /* NSFECB_POSTED                                 */

#include <string.h>
#include <clibos.h>           /* __super/__prob/__ascb/__xmpost/getmain/loadhi */
#include <clibwto.h>          /* wtof                                          */
#include <cvt.h>              /* CVT, CVTPTR                                   */
#include <ihascvt.h>          /* SCVT (scvtsvct), SVCTABLE, SVCENTRY           */
#include <ihaasvt.h>          /* ASVT (asvtmaxu, asvtenty) -- liveness guard   */

/* Shutdown drain (issue #55).  Same shape and cadence as the probe STC's
 * nsfv_drain: nudge, poll to a ceiling, settle, re-check. */
#define NSFSX_DRAIN_POLL    10U  /* 0.10 s per poll                            */
#define NSFSX_DRAIN_MAX    100U  /* 100 * 0.10 s = 10 s ceiling                */
#define NSFSX_DRAIN_SETTLE  20U  /* 0.20 s after inflight reaches zero         */

/* --------------------------------------------------------------------------
 * Transport state.  Single client / single slot BY CONSTRUCTION (ADR-0041 3):
 * one anchor, one request area, one private NSFRQE, one busy flag.  The
 * 64-slot (= MAXSOC) pool is M5-2b; nothing here is written to generalise.
 * -------------------------------------------------------------------------- */
static NSFV_ANCHOR  *g_anchor;
static void         *g_router_lpa;
static void         *g_router_epa;
static SVCENTRY     *g_svc_slot;
static unsigned char g_svc_saved[8];
static int           g_svc_stolen;

/* The executive's wake target: STC-private, key 8 (ADR-0041 addendum). */
static NSFECB        g_wake_ecb;

/* The request the executive is working on, and whether one is in flight.
 *
 * ONE at a time, deliberately (ADR-0042 10).  b3 makes the CLAIM concurrent --
 * 64 clients can have requests outstanding -- but SERVICE stays serialised:
 * one private NSFRQE, one busy flag, and a record of WHICH SLOT is being
 * served, because the drain can no longer assume there is only one.
 * Concurrent service is b4. */
static NSFRQE        g_priv;
static int           g_busy;
static NSFV_SLOT    *g_busy_slot;

/* ==========================================================================
 * CSA anchor
 * ========================================================================== */
static NSFV_ANCHOR *
nsfsx_anchor_alloc(void)
{
    NSFV_ANCHOR   *anchor;
    unsigned char  savekey;

    if (__super(PSWKEY0, &savekey)) return NULL;
    anchor = (NSFV_ANCHOR *)getmain((unsigned)sizeof(NSFV_ANCHOR), 241);
    if (anchor) {
        memset(anchor, 0, sizeof(NSFV_ANCHOR));
        memcpy(anchor->eye, "NSFVANCR", 8);
        unsigned i;

        anchor->version   = NSFV_ANCHOR_VER;
        anchor->flags     = NSFV_ANCHOR_ACTIVE;
        anchor->server_ascb = __ascb(0);
        /* PUBLISH THE SLOT COUNT (ADR-0042 6).  The SVC routine bounds its
        ** scan by THIS field, never by its own EQU: the party that knows how
        ** much storage exists is the party that allocated it, and NSFVSVC is
        ** a separate load module that can be stale against this one. */
        anchor->nslots    = NSFV_NSLOTS;
        anchor->exhausted = 0;
        /* Stamp every slot's guard once, here, under the same key window as
        ** every other key-0 store.  Checked before every dispatch.  memset
        ** already left the states FREE, which is the value the claim CS
        ** compares against. */
        for (i = 0; i < NSFV_NSLOTS; i++)
            memcpy(anchor->slots[i].rqe_guard, NSFREQX_GUARD,
                   NSFREQX_GUARDLEN);
    }
    __prob(savekey, NULL);
    return anchor;
}

static void
nsfsx_anchor_free(void)
{
    unsigned char savekey;

    if (!g_anchor) return;
    if (__super(PSWKEY0, &savekey)) return;
    /* Invalidate the eyecatcher BEFORE releasing: freed SP=241 storage is
    ** reused, not zeroed, so a client still parked in the routine must not
    ** accept the reused anchor as valid (its wake path revalidates the eye). */
    memset(g_anchor->eye, 0, sizeof(g_anchor->eye));
    freemain(g_anchor);
    __prob(savekey, NULL);
    g_anchor = NULL;
}

/* ==========================================================================
 * Largest contiguous SP=241 block still available, in bytes.
 *
 * b0 measured CSA TOTAL (2064 KB on MVSCE) and a FLOOR on the largest
 * contiguous block -- its doubling search was capped at 1 MB and never failed,
 * so it never reported what was actually free.  This closes that gap in the
 * direction that matters: run AFTER the pool is taken, it says how much
 * headroom is left once NSF has had its 134 KB.
 *
 * Safe because getmain RETURNS NULL on failure rather than abending -- which
 * nsfsx_anchor_alloc already depends on.  Doubling, then a binary refine, and
 * every probe block is freed again immediately.  Caller must be in key 0.
 * ========================================================================== */
#define NSFSX_PROBE_CAP  (2048U * 1024U)    /* stop above any plausible CSA   */

static unsigned
nsfsx_csa_largest(void)
{
    unsigned lo = 0;
    unsigned hi;
    void    *p;

    /* Double until it fails: lo is the last size that worked. */
    for (hi = 4096U; hi <= NSFSX_PROBE_CAP; hi <<= 1) {
        p = getmain(hi, 241);
        if (!p) break;
        freemain(p);
        lo = hi;
    }
    if (hi > NSFSX_PROBE_CAP) return lo;    /* more than we care to measure   */

    /* Refine between lo (works) and hi (does not), to 4 KB. */
    while (hi - lo > 4096U) {
        unsigned mid = lo + ((hi - lo) >> 1);
        p = getmain(mid, 241);
        if (p) { freemain(p); lo = mid; } else { hi = mid; }
    }
    return lo;
}

/* ==========================================================================
 * SVC routine: load into CSA, patch the anchor address, steal / restore.
 * ========================================================================== */
static int
nsfsx_router_load(void)
{
    unsigned char savekey;
    void         *lpa;
    void         *epa;
    unsigned      size;
    int           rc;

    if (__super(PSWKEY0, &savekey)) {
        wtof("NSF046E CANNOT ENTER SUPERVISOR FOR SVC LOAD");
        return -1;
    }
    rc = __loadhi(NSFV_ROUTER_MOD, &lpa, &epa, &size);
    if (rc) {
        __prob(savekey, NULL);
        wtof("NSF047E CANNOT LOAD %s INTO CSA, RC=%d", NSFV_ROUTER_MOD, rc);
        return -1;
    }
    g_router_lpa = lpa;
    g_router_epa = epa;
    /* Written ONCE, before the slot is stolen, then read-only -- no invocation
    ** ever sees an unpatched value and reentrancy is preserved (ADR-0038 3). */
    *(void **)((unsigned char *)epa + NSFV_ANCH_OFF) = (void *)g_anchor;
    __prob(savekey, NULL);
    return 0;
}

static void
nsfsx_router_unload(void)
{
    unsigned char savekey;

    if (!g_router_lpa) return;
    if (__super(PSWKEY0, &savekey)) return;
    freemain(g_router_lpa);
    g_router_lpa = NULL;
    g_router_epa = NULL;
    __prob(savekey, NULL);
}

static int
nsfsx_svc_steal(void)
{
    unsigned char  savekey;
    CVT           *cvt;
    SCVT          *scvt;
    SVCTABLE      *svct;
    SVCENTRY      *slot;
    unsigned char *b;
    void          *unused_ep;
    unsigned       i, j, best;

    if (__super(PSWKEY0, &savekey)) {
        wtof("NSF048E CANNOT ENTER SUPERVISOR FOR SVC STEAL");
        return -1;
    }
    cvt  = CVTPTR;
    scvt = (SCVT *)cvt->cvtabend;
    svct = (SVCTABLE *)scvt->scvtsvct;

    /* The unused-SVC marker: the entry point shared by the most slots in
    ** 200-255 (unused slots dominate).  Same determination as the probe. */
    unused_ep = svct->svcentry[255].svcepa;
    best      = 0;
    for (i = 200; i <= 255; i++) {
        void    *ep = svct->svcentry[i].svcepa;
        unsigned c  = 0;
        for (j = 200; j <= 255; j++)
            if (svct->svcentry[j].svcepa == ep) c++;
        if (c > best) { best = c; unused_ep = ep; }
    }

    slot = &svct->svcentry[NSFV_SVCNUM];
    if (slot->svcepa != unused_ep) {
        __prob(savekey, NULL);
        wtof("NSF049E SVC %u IN USE (EP %08X) -- NOT STOLEN",
             (unsigned)NSFV_SVCNUM, (unsigned)slot->svcepa);
        return -1;
    }
    memcpy(g_svc_saved, slot, 8);
    slot->svcepa = g_router_epa;
    b = (unsigned char *)slot + 4;
    b[0] = 0xC0;                        /* SVCTYPE3, svcapf=0, preemptive     */
    g_svc_slot   = slot;
    g_svc_stolen = 1;
    __prob(savekey, NULL);
    wtof("NSF042I SVC %u STOLEN (EP %08X)",
         (unsigned)NSFV_SVCNUM, (unsigned)g_router_epa);
    return 0;
}

static void
nsfsx_svc_restore(void)
{
    unsigned char savekey;

    if (!g_svc_stolen || !g_svc_slot) return;
    if (__super(PSWKEY0, &savekey)) return;
    memcpy(g_svc_slot, g_svc_saved, 8);
    __prob(savekey, NULL);
    g_svc_stolen = 0;
    g_svc_slot   = NULL;
    wtof("NSF043I SVC %u RESTORED", (unsigned)NSFV_SVCNUM);
}

/* ==========================================================================
 * The ADR-0040 client-death guard, used AS PROVEN.  Runs immediately before
 * every reply POST: __xmpost dereferences the recorded ASCB, and the ASCB of
 * an ended address space is reused SQA.  Compare the ASCB ADDRESS only.
 *
 * The safe-side asymmetry is the rule everything bends around: a dead client
 * called live leaks a slot; a LIVE client called dead has its storage freed
 * underneath it.  So UNKNOWN is neither posted into nor reaped.
 * Caller must already be in key 0.
 * ========================================================================== */
static int
nsfsx_client_state(const NSFV_SLOT *slot)
{
    CVT  *cvt;
    ASVT *asvt;

    /* The POINTER CHASE is platform and stays here; the ARITHMETIC -- the
    ** range check, the asid-1 index, the AVAIL bit, the address compare and
    ** all four verdicts -- is nsfreqx_classify, host-pinned by TSTREQX.  That
    ** split is what turned this function per-slot without re-deriving the
    ** truth table by hand, which is where a classifier gets copied wrong. */
    cvt = CVTPTR;
    if (!cvt) return NSFREQX_CL_UNKNOWN;
    asvt = (ASVT *)cvt->cvtasvt;
    if (!asvt) return NSFREQX_CL_UNKNOWN;

    return nsfreqx_classify((UINT)slot->req_ascb, slot->req_asid,
                            (UINT)asvt->asvtmaxu,
                            (const UINT *)asvt->asvtenty);
}

/* Reap a slot whose client the guard proved DEAD: give the in-flight count
 * back, clear the staging, release the slot -- and never post into it.
 *
 * TWO MOVES, NOT ONE, AND THE FIRST IS A CS (ADR-0042 2).  This is the one
 * real ABA in the design: between observing a slot as reapable and reclaiming
 * it, the owner can complete, release, and a NEW client can claim it.  A blind
 * store to FREE would then hand one slot to two owners -- the worst failure
 * this layer can produce.
 *
 * So: CS the observed state to CLAIMED, which no other party can claim; only
 * then clear; then a plain store to FREE.  Reaping STRAIGHT to FREE would be
 * a smaller version of the same bug -- a new client could claim the slot and
 * begin staging into storage this function is still wiping.
 *
 * A failed compare means the world changed underneath us and the request we
 * meant to reap no longer exists: tolerate it and move on, changing nothing.
 * Returns 1 if the slot was reclaimed.  Caller must already be in key 0. */
static int
nsfsx_reap(NSFV_SLOT *slot, UINT observed)
{
    UINT want = observed;

    if (__cas((unsigned *)&slot->req_state, (unsigned *)&want,
              NSFV_REQ_CLAIMED) != 0) {
        return 0;               /* raced -- not ours to reclaim               */
    }

    memset(slot->stage, 0, sizeof(slot->stage));
    memset(slot->rqe,   0, sizeof(slot->rqe));
    slot->xlen     = 0;
    slot->req_ascb = NULL;
    slot->req_asid = 0;
    slot->req_state = NSFV_REQ_FREE;    /* plain ST: we own it outright now   */

    if (g_anchor->inflight) g_anchor->inflight--;
    g_anchor->reaped++;
    return 1;
}

/* ==========================================================================
 * Public: bring-up and teardown
 * ========================================================================== */
int
nsfsx_start(void)
{
    g_anchor = nsfsx_anchor_alloc();
    if (!g_anchor) {
        /* NAME THE SIZE.  The pool is one contiguous SP=241 GETMAIN, and the
        ** operator's next question is always "how much did it want" -- the
        ** same posture as the SVC slot steal, which refuses rather than
        ** falling back. */
        wtof("NSF045E CANNOT ALLOCATE %u BYTES OF CSA FOR %u REQUEST SLOTS"
             " -- NSFS NOT STARTED",
             (unsigned)sizeof(NSFV_ANCHOR), (unsigned)NSFV_NSLOTS);
        return -1;
    }
    {   /* Diagnostic: what is left after we took ours. */
        unsigned char savekey;
        if (!__super(PSWKEY0, &savekey)) {
            unsigned left = nsfsx_csa_largest();
            __prob(savekey, NULL);
            wtof("NSF055I CSA POOL %u BYTES (%u SLOTS X %u)"
                 " -- LARGEST FREE BLOCK NOW %u",
                 (unsigned)sizeof(NSFV_ANCHOR), (unsigned)NSFV_NSLOTS,
                 (unsigned)sizeof(NSFV_SLOT), left);
        }
    }
    if (nsfsx_router_load()) {
        nsfsx_anchor_free();
        return -1;
    }

    /* PUBLISH THE WAKE-ECB ADDRESS BEFORE STEALING THE SLOT (ADR-0041
    ** addendum).  The steal is the "open for business" signal; a client must
    ** never find a stolen slot together with an unpublished address.  The
    ** anchor is key-0 CSA, so the store needs the key window. */
    {
        unsigned char savekey;
        if (__super(PSWKEY0, &savekey)) {
            nsfsx_router_unload();
            nsfsx_anchor_free();
            return -1;
        }
        g_wake_ecb = 0u;
        g_anchor->server_ecb_ptr = (void *)&g_wake_ecb;
        __prob(savekey, NULL);
    }

    if (nsfsx_svc_steal()) {
        nsfsx_router_unload();
        nsfsx_anchor_free();
        return -1;
    }
    wtof("NSF041I NSFS TRANSPORT READY -- ANCHOR=%08X ECB=%08X",
         (unsigned)g_anchor, (unsigned)&g_wake_ecb);
    return 0;
}

/* Wait out a fraction of a second without a busy loop. */
static void
nsfsx_pause(unsigned hsec)
{
    NSFECB local = 0;               /* nobody posts it; the timer does        */
    ecb_timed_wait((ECB *)&local, hsec, 0);
}

/* Nudge EVERY parked client so it bails out of its WAIT.
 *
 * The probe STC's single-slot nsfv_wake_parked does not survive the pool: with
 * 64 clients claimed and service serialised, 63 are legitimately parked at any
 * moment, and a drain that woke one and then timed out on the other 63 would
 * look exactly like a hang.  Caller must already be in key 0. */
static void
nsfsx_wake_parked(void)
{
    unsigned i;

    for (i = 0; i < g_anchor->nslots; i++) {
        NSFV_SLOT *slot = &g_anchor->slots[i];

        if (slot->req_state == NSFV_REQ_FREE) continue;
        if (!slot->req_ascb)                  continue;
        if (nsfsx_client_state(slot) != NSFREQX_CL_LIVE) continue;
        __xmpost(slot->req_ascb, &slot->reply_ecb, 0);
    }
}

/* Drain to zero in flight, or report that we could not (issue #55).
 *
 * Before b3 this did not exist and nsfsx_stop freed the CSA unconditionally.
 * That was INERT while nothing read the count -- and the pool makes it live,
 * not only after a fault: 63 clients parked on their own reply ECBs during
 * ORDINARY operation all count as in flight, and freeing CSA underneath them
 * is a wild-store generator in every one of their address spaces.
 *
 * The asymmetry decides it, the same way it decides the death guard:
 * retaining CSA leaks 134 KB until IPL; freeing it under a live client
 * corrupts that client. */
static int
nsfsx_drain_inflight(void)
{
    volatile unsigned *inflight = &g_anchor->inflight;
    unsigned           n;

    for (n = 0; n < NSFSX_DRAIN_MAX; n++) {
        if (*inflight == 0) {
            nsfsx_pause(NSFSX_DRAIN_SETTLE);
            return (*inflight == 0);     /* re-check: catch a racing entry    */
        }
        nsfsx_wake_parked();
        nsfsx_pause(NSFSX_DRAIN_POLL);
    }
    return 0;
}

void
nsfsx_stop(void)
{
    unsigned char savekey;
    int           drained = 0;

    /* Reverse of start: no new client can enter once the slot is restored. */
    nsfsx_svc_restore();

    if (g_anchor && !__super(PSWKEY0, &savekey)) {
        /* Clear ACTIVE so a client parked in the routine bails, and invalidate
        ** the published ECB address (this storage dies with the STC), BEFORE
        ** nudging: a woken client must find the anchor already quiescing. */
        g_anchor->flags &= ~NSFV_ANCHOR_ACTIVE;
        g_anchor->server_ecb_ptr = NULL;
        drained = nsfsx_drain_inflight();
        __prob(savekey, NULL);
    }

    nsfsx_router_unload();

    if (g_anchor && !drained) {
        /* RETAIN the CSA.  Leaking it costs 134 KB until IPL; freeing it with
        ** clients still inside the routine corrupts their address spaces. */
        wtof("NSF054W %u CLIENT(S) STILL IN FLIGHT -- CSA RETAINED",
             g_anchor->inflight);
        g_anchor = NULL;
    } else {
        nsfsx_anchor_free();
    }
    wtof("NSF044I NSFS TRANSPORT STOPPED");
}

UINT *
nsfsx_ecb(void)
{
    return (UINT *)&g_wake_ecb;
}

/* ==========================================================================
 * The WAIT gate's probe.  Side-effect-free.
 *
 * TWO states, not one.  The second -- completed but not yet replied -- is
 * reachable whenever the executive parks a request and something later
 * completes it; without it here the loop can commit to a WAIT on top of that
 * state and the reply never goes out (ADR-0041 5; ADR-0025 defect (2)).
 *
 * Reads the CSA req_state from problem state: the anchor is not fetch
 * protected, and a stale read only costs one extra pass.
 * ========================================================================== */
int
nsfsx_pending(void)
{
    unsigned i;

    if (g_anchor == NULL) return 0;
    if (g_busy) return (g_priv.ecb & NSFECB_POSTED) ? 1 : 0;
    for (i = 0; i < g_anchor->nslots; i++) {
        if (g_anchor->slots[i].req_state == NSFV_REQ_PENDING) return 1;
    }
    return 0;
}

/* The first slot with a published request, or NULL.  Lowest index first, so
 * the order is defined rather than incidental -- a test that pre-claims slots
 * and predicts which one serves next needs this to be a rule. */
static NSFV_SLOT *
nsfsx_next_pending(void)
{
    unsigned i;

    for (i = 0; i < g_anchor->nslots; i++) {
        if (g_anchor->slots[i].req_state == NSFV_REQ_PENDING)
            return &g_anchor->slots[i];
    }
    return NULL;
}

/* ==========================================================================
 * One call per executive pass.
 *
 * Completion is decided by the POSTED BIT of the private ECB, never by a
 * non-zero test: a satisfied multi-ECB WAIT leaves RB-address remnants in
 * un-posted ECBs (CLAUDE.md 4).
 * ========================================================================== */
void
nsfsx_drain(void)
{
    unsigned char savekey;
    int           cl    = -1;
    unsigned      lascb = 0;
    unsigned      lasid = 0;

    if (g_anchor == NULL) return;

    /* ---- 1. Finish the completed request ----------------------------------
     * The executive has run soc_complete on the private copy, so retcode /
     * errno_ / the fn-specific outputs are set.  Copy ONLY those back
     * (ADR-0041 4 -- notably NOT ubuf, which holds the STC's staging address
     * in the private copy), re-check the client, then reply.
     *
     * g_busy_slot, not a scan: the slot being served is the one that was
     * PENDING when we took it, and it is no longer PENDING now. */
    if (g_busy && g_busy_slot && (g_priv.ecb & NSFECB_POSTED)) {
        NSFV_SLOT *slot = g_busy_slot;

        if (__super(PSWKEY0, &savekey)) return;

        nsfreqx_result_out((NSFRQE *)slot->rqe, &g_priv);

        cl    = nsfsx_client_state(slot);
        lascb = (unsigned)slot->req_ascb;
        lasid = slot->req_asid;

        if (cl == NSFREQX_CL_DEAD) {
            nsfsx_reap(slot, NSFV_REQ_PENDING);
        } else if (cl == NSFREQX_CL_UNKNOWN) {
            /* Neither post nor reap.  HELD keeps the slot busy to the SVC
            ** routine while taking it off this drain's work list. */
            slot->req_state = NSFV_REQ_HELD;
        } else {
            g_anchor->served++;
            slot->req_state = NSFV_REQ_DONE;
            __xmpost(slot->req_ascb, &slot->reply_ecb, 0);
        }
        g_busy      = 0;
        g_busy_slot = NULL;
        __prob(savekey, NULL);

        if (cl == NSFREQX_CL_DEAD)
            wtof("NSF050I CLIENT DEAD (ASCB=%08X ASID=%04X) -- REQUEST REAPED",
                 lascb, lasid);
        else if (cl == NSFREQX_CL_UNKNOWN)
            wtof("NSF051W CLIENT LIVENESS UNKNOWN (ASCB=%08X ASID=%04X)"
                 " -- REQUEST HELD", lascb, lasid);
    }

    /* ---- 2. Take the next published request --------------------------------
     * ONE at a time (ADR-0042 10): the pool makes the CLAIM concurrent, not
     * the service.  A second PENDING slot simply waits for the next pass, with
     * its client parked on its own reply ECB. */
    if (!g_busy) {
        NSFV_SLOT *slot = nsfsx_next_pending();
        int        act;
        int        ok      = 0;
        int        corrupt = 0;

        if (slot == NULL) return;
        if (__super(PSWKEY0, &savekey)) return;

        cl    = nsfsx_client_state(slot);
        lascb = (unsigned)slot->req_ascb;
        lasid = slot->req_asid;

        /* THE DECISION IS THE HOST-PINNED TRUTH TABLE, not a chain of ifs
        ** re-derived per slot (TSTREQX sweeps all 60 rows and asserts exactly
        ** one of them dispatches).  guard_ok covers an overrun of the 64-byte
        ** RQE move -- the guard sits between the slot's RQE and its staging --
        ** and the pointer check covers corruption OF the word we POST through,
        ** which we are the one party that knows the correct value for. */
        act = nsfreqx_slot_action(slot->req_state, cl,
                                  nsfreqx_guard_ok(slot->rqe_guard),
                                  g_anchor->server_ecb_ptr ==
                                      (void *)&g_wake_ecb);

        switch (act) {
        case NSFREQX_ACT_REAP:
            nsfsx_reap(slot, NSFV_REQ_PENDING);
            break;
        case NSFREQX_ACT_HOLD:
            slot->req_state = NSFV_REQ_HELD;
            break;
        case NSFREQX_ACT_REAP_BAD:
            /* Never dispatch a slot we cannot trust, and above all never
            ** POST: a corrupted wake-ECB pointer is still non-zero, so the SVC
            ** routine would happily key-0 POST to a wrong address inside our
            ** private storage. */
            corrupt = nsfreqx_guard_ok(slot->rqe_guard) ? 2 : 1;
            nsfsx_reap(slot, NSFV_REQ_PENDING);
            break;
        case NSFREQX_ACT_DISPATCH:
            if (slot->xfunc == NSFV_REQ_RQE) {
                /* Hop 2: the CSA slot into the STC-private copy.  ubuf is
                ** rewritten to THIS SLOT's staging buffer and ulen to the count
                ** actually staged -- the very value the SVC routine's clamp
                ** used for the move, so the op reports what really crossed
                ** (ADR-0041 2). */
                nsfreqx_dispatch_in(&g_priv, (const NSFRQE *)slot->rqe,
                                    slot->stage, slot->xlen);
                ok = 1;
            } else {
                /* A probe verb reached the production STC: reject cleanly
                ** rather than fall through into a dispatch. */
                slot->req_state = NSFV_REQ_HELD;
            }
            break;
        default:
            break;
        }
        __prob(savekey, NULL);

        if (corrupt == 1)
            wtof("NSF052E SLOT RQE GUARD CLOBBERED -- REQUEST REAPED,"
                 " NOT POSTED");
        else if (corrupt == 2)
            wtof("NSF053E ANCHOR WAKE-ECB POINTER CORRUPT (%08X, EXPECTED"
                 " %08X) -- REQUEST REAPED, NOT POSTED",
                 (unsigned)g_anchor->server_ecb_ptr, (unsigned)&g_wake_ecb);
        else if (cl == NSFREQX_CL_DEAD)
            wtof("NSF050I CLIENT DEAD (ASCB=%08X ASID=%04X) -- REQUEST REAPED",
                 lascb, lasid);
        else if (cl == NSFREQX_CL_UNKNOWN)
            wtof("NSF051W CLIENT LIVENESS UNKNOWN (ASCB=%08X ASID=%04X)"
                 " -- REQUEST HELD", lascb, lasid);

        if (ok) {
            /* Dispatch OUTSIDE the key window: the executive runs in its own
            ** key 8 on ordinary storage, exactly as in Phase 1.  It may
            ** complete inline (soc_complete POSTs g_priv.ecb) or park it --
            ** either way step 1 picks it up, this pass or a later one. */
            g_busy      = 1;
            g_busy_slot = slot;
            nsfreq_dispatch(&g_priv);
        }
    }
}
