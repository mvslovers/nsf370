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
#include "nsfsts.h"           /* the wakeposts counter (issue #64, 64-0)       */
#include "nsftmr.h"           /* nsftmr_count (armed timers, reported at STATS) */

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

/* DID THE TRANSPORT'S WAKE EVER LAND? (issue #64, step 64-0.)
 *
 * THIS COUNTER CHANGED MEANING AT 64-1, AND A READER COMPARING IT ACROSS THAT
 * CHANGE IS COMPARING TWO DIFFERENT THINGS.  It has always been incremented on
 * every drain that finds the POSTED bit set.  What changed is underneath it:
 * the drain now RESETS g_wake_ecb (see nsfsx_drain step 0), where before 64-1
 * nothing ever cleared it.
 *
 *   BEFORE 64-1 -- MONOTONE AND LATCHING BY CONSTRUCTION.  The word was
 *   assigned zero exactly once, in nsfsx_start (verified against the wait seam
 *   too: nsfevt_plat_wait copies the list into a local array and libc370's
 *   ecb_waitlist is a bare WAIT ECBLIST, so neither writes the ECB).  Once the
 *   first POST landed the counter went non-zero and thereafter tracked
 *   evtpasses at a constant offset -- measured at exactly 3 361 across four
 *   readings of one instance.  "wakeposts 5000" did NOT mean 5000 wakes; the
 *   whole weight of the measurement was on the zero / non-zero distinction.
 *
 *   FROM 64-1 -- IT COUNTS WAKE EVENTS.  Each observation consumes the bit, so
 *   a reading is the number of passes that found a wake outstanding.  That is
 *   the more useful counter, and it still is not a count of POSTs: two posts
 *   arriving between one observation and the next COALESCE into one, exactly
 *   as they do for any ECB.  It is a lower bound on posts, not a tally.
 *
 * EVERY FIGURE IN docs/nsf-64-0*.md WAS TAKEN UNDER THE OLD SEMANTICS, so the
 * ~8 500/s WAKEPOSTS rates recorded there are the latch tracking evtpasses,
 * not wakes, and nothing in this counter's post-64-1 readings is comparable to
 * them.
 *
 * A ZERO READING ALONGSIDE A NON-ZERO `served` IS STILL THE 64-0 FINDING: it
 * would say every request that instance ever completed was picked up by the
 * WAIT-gate probe on a pass that happened for some other reason, i.e. the
 * transport's wake did not merely lack a floor -- it never arrived.  64-0
 * REFUTED that: the POST lands. */
static STSCTR       *g_wakeposts;

/* The request the executive is working on, and whether one is in flight.
 *
 * ONE at a time, deliberately (ADR-0042 10).  b3 makes the CLAIM concurrent --
 * 64 clients can have requests outstanding -- but SERVICE stays serialised:
 * one private NSFRQE, one busy flag, and a record of WHICH SLOT is being
 * served, because the drain can no longer assume there is only one.
 *
 * M5-2b4 does NOT change that, and the WAIT-gate probe below is written around
 * it: the outcomes that need no private NSFRQE (reap / hold / reap-bad) are
 * consumed whether or not one is in service, and a DISPATCHABLE second request
 * stays invisible to the probe until this flag clears -- reporting it would
 * make the executive skip its WAIT for work the drain then declines, which is
 * a spin, not a latency fix (nsfreqx_actionable).  Concurrent SERVICE is still
 * open. */
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
        anchor->nslots     = NSFV_NSLOTS;
        anchor->exhausted  = 0;
        /* M5-2b4: contended claims.  memset already zeroed both counters --
        ** they are named here for the same reason `exhausted` was: a
        ** diagnostic nobody initialises is a diagnostic nobody remembers to
        ** read. */
        anchor->collisions = 0;
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
nsfsx_reap(NSFV_SLOT *slot, UINT observed, int verdict, int storage_ok)
{
    UINT want = observed;

    /* THE PREDICATE GOVERNS, it does not merely describe.  Routing every reap
     * through nsfreqx_reap_ok keeps ONE encoding of "may this slot be
     * reclaimed" -- two encodings of one rule is how they diverge, and this is
     * the rule that decides whether a live client's storage gets freed.  It is
     * also what makes the seven TSTREQX assertions statements about code that
     * runs. */
    if (!nsfreqx_reap_ok(observed, verdict, storage_ok)) {
        return 0;
    }

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
    /* Register once (sts_register APPENDS, and it never abends -- NULL when
     * the fixed registry is full, so the use site is guarded). */
    if (g_wakeposts == NULL) {
        g_wakeposts = sts_register("NSFSX", "wakeposts");
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

    if (g_anchor && !drained) {
        /* RETAIN EVERYTHING -- the anchor AND the routine.  Leaking 134 KB of
        ** CSA plus the module until IPL is the cheap side of the asymmetry.
        **
        ** THE ROUTINE IS RETAINED FOR A SHARPER REASON THAN THE ANCHOR.  A
        ** client that failed to drain is parked in a WAIT *inside that code*,
        ** supervisor state, key 0.  nsfsx_router_unload freemains the CSA the
        ** routine was __loadhi'd into, so unloading it here would pull the
        ** instructions out from under a task that is going to resume on them.
        ** Retaining the anchor while freeing the code is STRICTLY WORSE than
        ** leaking both, and it would contradict the same safe-side asymmetry
        ** this function is built on.
        **
        ** This is why the probe STC's nsfv_drain path frees NOTHING on a
        ** failed drain -- its retain branch jumps past every free with a
        ** comment saying exactly that -- and it is the shape copied here. */
        wtof("NSF054W %u CLIENT(S) STILL IN FLIGHT -- CSA AND SVC ROUTINE"
             " RETAINED (EXHAUSTED=%u)",
             g_anchor->inflight, g_anchor->exhausted);
        g_anchor = NULL;
    } else {
        nsfsx_router_unload();
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
 * The Phase-2 STATS supplement (issue #64, step 64-0), hung on the operator's
 * STATS handler through nsfopr_set_stats_extra.  Phase 1 registers nothing, so
 * `F NSF,STATS` is byte-for-byte unchanged.
 *
 * WHY A SEAM RATHER THAN TWO MORE NSFSTS COUNTERS.  The raw wake-ECB word is
 * not a counter -- it is a machine word whose MEANING is in its top two bits,
 * and a human reading `X'80A6F210'` next to `wakeposts 0` sees an RB-address
 * remnant for what it is where a bare number would mislead.  It is also the
 * one value that must be read at the instant STATS runs.
 *
 * THE LINES ARE ALSO THE DEPLOY-TOOK-EFFECT CHECK.  If NSF812I is absent from
 * the reply, the running module predates 64-0; if NSF813I carries no `BUSY=`,
 * it predates 64-0c.  CLAUDE.md 5's most expensive failure class, caught here
 * for free rather than reasoned about -- and the check has to name the FIELD,
 * not just the message id, because 64-0b's module already had both lines.
 *
 * 64-1 ADDS NO FIELD, so it has no check of that shape and needs a different
 * one: `POSTED=N` ON AN INSTANCE WHOSE `SERVED` IS NON-ZERO IS ITSELF THE
 * PROOF.  Before 64-1 nothing cleared the word, so that combination was
 * impossible.  ITS COMPLEMENT IS AMBIGUOUS AND MUST NOT BE READ AS A CHECK:
 * `POSTED=Y` means either the reset is absent from the source or the deploy
 * silently did not take, which is exactly the failure 5 warns about -- so a
 * revert arm has to be corroborated independently (the deploy output read for
 * the mid-chain HTTP 500, and the load module on MVS).
 *
 * Everything the 64-0 prediction weighs is on ONE line: the ECB word, its
 * POSTED bit decoded, wakeposts and served.  "wakeposts == 0 while served ==
 * N" is then a single observation, not a cross-reference between two.
 * ========================================================================== */
void
nsfsx_stats_extra(void)
{
    unsigned ecb      = (unsigned)g_wake_ecb;
    int      busyslot = -1;         /* -1 = none; valid indices are 0..63 */

    if (g_anchor != NULL && g_busy_slot != NULL) {
        busyslot = (int)(g_busy_slot - g_anchor->slots);
    }

    /* WPREG IS NOT DECORATION -- IT IS THE THIRD STATE (CLAUDE.md 8.5).
     * sts_register returns NULL when the fixed registry is full, and a
     * never-incremented counter reads 0 -- which is INDISTINGUISHABLE from the
     * finding this whole step exists to establish ("wakeposts == 0 while
     * served == N").  An absence that looks exactly like its own success is
     * the shape this project pays the most for, so registration is reported
     * rather than assumed, and WAKEPOSTS is read through the cached pointer,
     * not through sts_value (which also answers 0 for "no such counter").
     *
     * EVTPASSES needs no such flag, and the reason is worth stating: STATS is
     * dispatched from nsfopr_drain, which runs INSIDE evt_mainloop, so by the
     * time this line is written at least one pass has completed and a healthy
     * counter is >= 1.  EVTPASSES=0 is therefore already a third state -- it
     * can only mean not registered or not incremented, never "no passes". */
    wtof("NSF812I WAKEECB=%08X POSTED=%s EVTPASSES=%u WAKEPOSTS=%u WPREG=%s"
         " SERVED=%u",
         ecb,
         ((g_wake_ecb & NSFECB_POSTED) != 0u) ? "Y" : "N",
         (unsigned)sts_value("NSFEVT", "evtpasses"),
         (g_wakeposts != NULL) ? (unsigned)g_wakeposts->value : 0u,
         (g_wakeposts != NULL) ? "Y" : "N",
         (g_anchor != NULL) ? (unsigned)g_anchor->served : 0u);

    /* BUSY / BUSYSLOT ARE NOT DECORATION EITHER (64-0b).  If a stall
     * reproduces while EVTPASSES is CLIMBING, the loop is running and not
     * taking the work -- and exactly one structure in the drain produces that:
     * g_busy set with g_busy_slot pointing at a slot whose private ECB never
     * gets posted.  Step 1 then waits on a completion that never comes, step 2
     * dispatches nothing, and nsfsx_any_pending_other skips the in-service slot
     * BY DESIGN -- so the WAIT-gate pre-filter answers "nothing to do" while a
     * request sits PENDING.  Without these two fields that state is
     * INDISTINGUISHABLE from a wake failure.
     *
     * BUSYSLOT is -1 for "none", never 0: slot 0 is a perfectly ordinary
     * in-service slot and is what a single client gets every time.
     *
     * WHY THEY MOVED HERE, AND WHY THEY ARE FIRST (64-0c).  The Hercules
     * console log truncates a message at roughly 107 characters of text, and
     * NSF812I is the longest line in the system: once the counters reach seven
     * digits -- i.e. exactly during the investigation these fields were added
     * for -- BUSYSLOT's value was cut off.  The truncation eats the TAIL, so
     * the repair is not only a shorter line but a POSITION: the two fields the
     * round turns on lead NSF813I, and what can be lost is the pure context
     * behind them.  INFLIGHT follows for the same reason -- it is load-bearing
     * in every stall reading so far -- and TMRQ, EXHAUSTED, COLLISIONS and
     * REAPED bring up the rear because losing one costs nothing.
     *
     * The alternative was a `\n` in the NSF812I format: libc370's vwtof splits
     * its text on newlines and issues one wto() per line (src/clib/vwtof.c), so
     * it would have worked.  Rejected because the second line would carry no
     * message id -- unreadable to a grep and odd on a console -- where NSF813I
     * already exists, is short, and is where this context belongs anyway.
     *
     * The timer-queue depth rides along.  nsftmr_count() == 0 does NOT imply
     * the STIMER is disarmed -- nsfsmain's nsftmr_plat_arm(1u) heartbeat sits
     * outside g_armed's bookkeeping and g_armed has no accessor -- so this
     * reports the queue, and EVTPASSES growth over an idle window is what
     * actually measures whether a periodic wake still exists. */
    wtof("NSF813I BUSY=%d BUSYSLOT=%d INFLIGHT=%u TMRQ=%u"
         " EXHAUSTED=%u COLLISIONS=%u REAPED=%u",
         g_busy, busyslot,
         (g_anchor != NULL) ? (unsigned)g_anchor->inflight   : 0u,
         (unsigned)nsftmr_count(),
         (g_anchor != NULL) ? (unsigned)g_anchor->exhausted  : 0u,
         (g_anchor != NULL) ? (unsigned)g_anchor->collisions : 0u,
         (g_anchor != NULL) ? (unsigned)g_anchor->reaped     : 0u);
}

/* Is any slot carrying a published request OTHER THAN THE ONE IN SERVICE?
 *
 * Cheap and key-free: it reads only the CSA state words, which are not fetch
 * protected, and a stale read costs one extra pass.  Kept separate from the
 * selector below so the two cases that dominate -- nothing published, and
 * nothing published but the request already in service -- answer without a key
 * switch.
 *
 * The in-service slot is excluded for the same reason the selector skips it:
 * it stays PENDING until step 1 of the drain finishes it, and it is step 1's
 * alone.  Counting it here would take the key window on every pass of a parked
 * request to conclude there is nothing to do. */
static int
nsfsx_any_pending_other(void)
{
    unsigned i;

    for (i = 0; i < g_anchor->nslots; i++) {
        NSFV_SLOT *slot = &g_anchor->slots[i];

        if (slot->req_state != NSFV_REQ_PENDING) continue;
        if (g_busy && slot == g_busy_slot)       continue;
        return 1;
    }
    return 0;
}

/* The first slot THIS PASS CAN ACT ON, and what to do with it.  Lowest index
 * first, so the order is defined rather than incidental -- a test that
 * pre-claims slots and predicts which one serves next needs this to be a rule.
 *
 * The per-slot decision is still nsfreqx_slot_action's host-pinned truth
 * table; what M5-2b4 adds is the second question, nsfreqx_actionable: can this
 * pass CONSUME that outcome?  A slot whose outcome is reap / hold / reap-bad
 * always can -- all three finish inside the CSA slot.  A DISPATCHABLE one can
 * only when the single private NSFRQE is free.  Both the drain and the WAIT
 * gate ask through here, so "what the drain will do" and "what the probe
 * reports" cannot drift apart: that drift is the spin.
 *
 * Caller must already be in key 0 -- nsfsx_client_state chases control blocks.
 */
static NSFV_SLOT *
nsfsx_next_actionable(int *act_out, int *cl_out)
{
    unsigned i;

    for (i = 0; i < g_anchor->nslots; i++) {
        NSFV_SLOT *slot = &g_anchor->slots[i];
        int        cl;
        int        act;

        if (slot->req_state != NSFV_REQ_PENDING) continue;

        /* THE SLOT IN SERVICE IS STILL PENDING, and it is not ours to touch.
        ** It stays PENDING until step 1 of the drain finishes it, so without
        ** this line a client that died while its request was parked would be
        ** REAPED from under the executive -- and reaping clears `stage`, which
        ** is exactly what the parked request's ubuf points at.  A cross-address
        ** -space use-after-free, and a slot handed to a new client while an
        ** in-flight operation still writes into it.  Step 1 is the ONE place
        ** that finishes the in-service slot, whatever its client is doing. */
        if (g_busy && slot == g_busy_slot) continue;

        cl  = nsfsx_client_state(slot);
        act = nsfreqx_slot_action(slot->req_state, cl,
                                  nsfreqx_guard_ok(slot->rqe_guard),
                                  g_anchor->server_ecb_ptr ==
                                      (void *)&g_wake_ecb);
        if (!nsfreqx_actionable(act, g_busy)) continue;

        *act_out = act;
        *cl_out  = cl;
        return slot;
    }
    return NULL;
}

/* ==========================================================================
 * The WAIT gate's probe.  Side-effect-free (the key window it may take is
 * restored before it returns).
 *
 * THREE states now, and the order of the checks is the cheap-first order.
 *
 *   1. completed but not yet replied -- reachable whenever the executive parks
 *      a request and something later completes it; without it the loop can
 *      commit to a WAIT on top of that state and the reply never goes out
 *      (ADR-0041 5; ADR-0025 defect (2)).
 *   2. nothing published other than the request already in service -- the idle
 *      answer, and it must not cost a key switch.
 *   3. something published.  With nothing in service that is consumable
 *      whatever its outcome, so the answer is yes without classifying.  With a
 *      request in service only the outcomes that need no private NSFRQE are
 *      consumable, and deciding that means classifying the client -- which
 *      chases control blocks and therefore takes the key window.
 *
 * M5-2b4 CHANGED (3).  Before it, this returned early on `g_busy` and never
 * looked at another slot, so a second client that published and then DIED sat
 * un-reaped for as long as an unrelated client's blocking operation ran.  What
 * it did NOT do -- and must not -- is report a DISPATCHABLE second request:
 * the executive skips its WAIT whenever a probe answers non-zero, and the
 * drain would decline the work, so the pass would make no progress and repeat.
 * That is a hot spin on the executive task, which is worse than the latency it
 * would remove.  Curing THAT means concurrent service, not a louder probe.
 * ========================================================================== */
int
nsfsx_pending(void)
{
    unsigned char savekey;
    int           act = NSFREQX_ACT_NONE;
    int           cl  = NSFREQX_CL_LIVE;
    int           r;

    if (g_anchor == NULL) return 0;
    if (g_busy && (g_priv.ecb & NSFECB_POSTED)) return 1;
    if (!nsfsx_any_pending_other())             return 0;
    if (!g_busy)                                return 1;

    if (__super(PSWKEY0, &savekey)) return 0;
    r = (nsfsx_next_actionable(&act, &cl) != NULL);
    __prob(savekey, NULL);
    return r;
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

    /* THE WAKE OBSERVATION (issue #64, 64-0).  Placed ahead of BOTH early
     * returns below: lower down it would silently become "the bit was set on a
     * pass that also had pending work", which is a different claim.
     *
     * POSTED BIT ONLY, never a non-zero test -- a satisfied multi-ECB WAIT
     * leaves an RB-address remnant (X'80......') in the ECBs that were not
     * posted (CLAUDE.md 4), so an un-posted g_wake_ecb reads non-zero the
     * moment the loop has waited once.  A non-zero test would report POSTED
     * for an ECB that never was, and confirm the exact opposite of the truth.
     *
     * It reads the bit; step 0 below consumes it.  The counter therefore counts
     * WAKE EVENTS from 64-1 onward, where before it latched -- see the
     * g_wakeposts declaration, which is where a reader comparing a post-64-1
     * number against docs/nsf-64-0*.md will look. */
    if ((g_wake_ecb & NSFECB_POSTED) != 0u && g_wakeposts != NULL) {
        STS_INC(g_wakeposts);
    }

    /* ---- 0. RESET THE WAKE ECB, AHEAD OF BOTH SCANS BELOW (issue #64, 64-1)
     * ADR-0022's reset-before-WAIT discipline, which Phase 2 never honoured.
     * g_wake_ecb was assigned zero once, in nsfsx_start, and never again, so
     * the first cross-address-space POST latched it for the life of the STC and
     * evt_mainloop's WAIT could never block again.  Phase 1 has always done
     * this -- nsfreq_drain resets g_reqecb before it takes the queue -- and so
     * does the probe STC (nsfv.c, nsfv_server_ecb_reset).  Phase 2 was the one
     * that diverged.  Measured cost of the divergence: ~8 500 passes per second
     * and 26 % of a host core, permanently, on every instance that had ever
     * served one request (docs/nsf-64-0-measurements.md 2).
     *
     * WHY THIS POSITION CLOSES THE WINDOW COMPLETELY.  A client publishes its
     * request (req_state = PENDING) and only THEN POSTs, so relative to this
     * reset there are exactly two orderings, and both are safe:
     *
     *   POST before the reset -> the publish was before it too, and both scans
     *                            below run after it, so the slot is seen on
     *                            this very pass.
     *   POST after the reset  -> the bit stands, and the WAIT returns on it
     *                            (nsfsx_ecb() puts this word in the ECBLIST).
     *
     * There is no third ordering, so no wake can be lost.
     *
     * WHY NOT PHASE 1's do/while RECHECK LOOP.  The recheck half of the
     * discipline is already present here, one level up: it is the WAIT gate,
     * and it asks its question through the same nsfsx_next_actionable this
     * drain does (nsfsx_pending; ADR-0025 defect (2)).  What Phase 2 lacked was
     * the reset half alone.  Wrapping this function in a loop instead would be
     * three regressions, not one improvement:
     *
     *   - the __super failures below are NO-PROGRESS returns, so the loop would
     *     spin on a condition its own failure path cannot clear;
     *   - a client may republish the instant it is replied to, so the loop has
     *     no finiteness argument in Phase 2 (Phase 1's rests on its bounded
     *     fan-in, which does not transfer) -- an unbounded drain is exactly
     *     what the run-to-completion rule forbids, because it starves the
     *     timers and devices that share this pass;
     *   - it would quietly turn ADR-0042 10's ONE unit of work per pass into
     *     "serve every inline-completing request per pass", which is a
     *     behaviour change and not part of this step.
     *
     * NOTHING ELSE IN THE WAKE PATH CHANGES.  The other three readers of this
     * word are unaffected: nsfsx_ecb() hands out its ADDRESS, the corruption
     * check compares server_ecb_ptr against that ADDRESS, and nsfsx_stats_extra
     * reads the value for the operator.  Only the value is consumed here. */
    g_wake_ecb = 0u;

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
            /* The completion path: storage was trusted at dispatch, so the
            ** only reason to reclaim here is that the client died meanwhile. */
            nsfsx_reap(slot, NSFV_REQ_PENDING, cl, 1);
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

    /* ---- 2. Take the next request THIS PASS CAN CONSUME ---------------------
     * ONE DISPATCH at a time (ADR-0042 10): the pool makes the CLAIM
     * concurrent, not the service.  A dispatchable second request waits for
     * the next pass, with its client parked on its own reply ECB.
     *
     * M5-2b4: the outcomes that need NO private NSFRQE -- reap a dead client,
     * hold an unknown one, reap untrusted storage -- are consumed whether or
     * not a request is in service, one per pass.  They finish inside the CSA
     * slot and never POST, so nothing about them waits on the request the
     * executive is working on; before b4 they did wait, for as long as an
     * unrelated client's blocking operation ran.  nsfsx_next_actionable is the
     * ONE place that decides, and the WAIT-gate probe asks the same question
     * through it -- if the two ever disagreed, the disagreement would be a
     * spin (nsfreqx_actionable). */
    {
        NSFV_SLOT *slot;
        int        act     = NSFREQX_ACT_NONE;
        int        ok      = 0;
        int        corrupt = 0;

        /* Cheap pre-filter, key-free: no published request at all is the
        ** common case and must not cost a key switch. */
        if (!nsfsx_any_pending_other()) return;
        if (__super(PSWKEY0, &savekey)) return;

        /* THE DECISION IS THE HOST-PINNED TRUTH TABLE, not a chain of ifs
        ** re-derived per slot (TSTREQX sweeps all 60 rows and asserts exactly
        ** one of them dispatches).  guard_ok covers an overrun of the 64-byte
        ** RQE move -- the guard sits between the slot's RQE and its staging --
        ** and the pointer check covers corruption OF the word we POST through,
        ** which we are the one party that knows the correct value for. */
        slot = nsfsx_next_actionable(&act, &cl);
        if (slot == NULL) {
            __prob(savekey, NULL);
            return;
        }
        lascb = (unsigned)slot->req_ascb;
        lasid = slot->req_asid;

        switch (act) {
        case NSFREQX_ACT_REAP:
            nsfsx_reap(slot, NSFV_REQ_PENDING, cl, 1);
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
            /* storage_ok = 0: this reclaim is mandated by the storage being
            ** untrustworthy, NOT by liveness -- the client here is LIVE, and
            ** passing the verdict alone would have the predicate refuse. */
            nsfsx_reap(slot, NSFV_REQ_PENDING, cl, 0);
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
            /* THE IDENTITY COMES FROM THE SLOT, NOT THE REQUEST (M5-2c1).  The
            ** SVC routine recorded this pair at CLAIMOK from the FLIH's R7,
            ** before any client data was staged, so it is the one thing about
            ** this request that a client cannot influence.  RQ_INITAPI records
            ** it against the new app instance; every other verb is dispatched
            ** exactly as before. */
            nsfreq_dispatch_id(&g_priv, (UINT)slot->req_ascb, slot->req_asid);
        }
    }
}
