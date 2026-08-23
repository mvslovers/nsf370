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
#include "nsfreqx.h"          /* the host-tested field rules                   */
#include "nsfevtp.h"          /* NSFECB_POSTED                                 */

#include <string.h>
#include <clibos.h>           /* __super/__prob/__ascb/__xmpost/getmain/loadhi */
#include <clibwto.h>          /* wtof                                          */
#include <cvt.h>              /* CVT, CVTPTR                                   */
#include <ihascvt.h>          /* SCVT (scvtsvct), SVCTABLE, SVCENTRY           */
#include <ihaasvt.h>          /* ASVT (asvtmaxu, asvtenty) -- liveness guard   */

#define NSFSX_ASVT_AVAIL  0x80000000U   /* ASVTAVAI: the entry is free         */

/* Client-liveness verdicts (ADR-0040, used as proven -- not redesigned). */
#define NSFSX_CL_LIVE     0
#define NSFSX_CL_DEAD     1
#define NSFSX_CL_UNKNOWN  2

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

/* The request the executive is working on, and whether one is in flight. */
static NSFRQE        g_priv;
static int           g_busy;

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
        anchor->version   = 1;
        anchor->flags     = NSFV_ANCHOR_ACTIVE;
        anchor->req_state = NSFV_REQ_FREE;
        anchor->server_ascb = __ascb(0);
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
nsfsx_client_state(void)
{
    CVT      *cvt;
    ASVT     *asvt;
    unsigned  asid;
    unsigned  entry;

    if (!g_anchor->req_ascb) return NSFSX_CL_UNKNOWN;
    cvt = CVTPTR;
    if (!cvt) return NSFSX_CL_UNKNOWN;
    asvt = (ASVT *)cvt->cvtasvt;
    if (!asvt || asvt->asvtmaxu == 0U) return NSFSX_CL_UNKNOWN;
    asid = g_anchor->req_asid;
    if (asid == 0U || asid > asvt->asvtmaxu) return NSFSX_CL_UNKNOWN;

    entry = (unsigned)asvt->asvtenty[asid - 1U];
    if (entry & NSFSX_ASVT_AVAIL)                  return NSFSX_CL_DEAD;
    if (entry != (unsigned)g_anchor->req_ascb)     return NSFSX_CL_DEAD;
    return NSFSX_CL_LIVE;
}

/* Reap a request whose client the guard proved DEAD: give the in-flight count
 * back, clear the staging, release the slot -- and never post into it. */
static void
nsfsx_reap(void)
{
    memset(g_anchor->stage, 0, sizeof(g_anchor->stage));
    memset(g_anchor->rqe,   0, sizeof(g_anchor->rqe));
    g_anchor->xlen      = 0;
    g_anchor->req_ascb  = NULL;
    g_anchor->req_asid  = 0;
    g_anchor->req_state = NSFV_REQ_FREE;
    if (g_anchor->inflight) g_anchor->inflight--;
    g_anchor->reaped++;
}

/* ==========================================================================
 * Public: bring-up and teardown
 * ========================================================================== */
int
nsfsx_start(void)
{
    g_anchor = nsfsx_anchor_alloc();
    if (!g_anchor) {
        wtof("NSF045E CANNOT ALLOCATE CSA ANCHOR");
        return -1;
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

void
nsfsx_stop(void)
{
    unsigned char savekey;

    /* Reverse of start: no new client can enter once the slot is restored. */
    nsfsx_svc_restore();

    if (g_anchor && !__super(PSWKEY0, &savekey)) {
        /* Clear ACTIVE so a client parked in the routine bails, invalidate the
        ** published ECB address (this storage dies with the STC), and wake
        ** anyone waiting so they are not stranded. */
        g_anchor->flags &= ~NSFV_ANCHOR_ACTIVE;
        g_anchor->server_ecb_ptr = NULL;
        if (g_anchor->req_ascb && nsfsx_client_state() == NSFSX_CL_LIVE)
            __xmpost(g_anchor->req_ascb, &g_anchor->reply_ecb, 0);
        __prob(savekey, NULL);
    }
    nsfsx_router_unload();
    nsfsx_anchor_free();
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
    if (g_anchor == NULL) return 0;
    if (!g_busy && g_anchor->req_state == NSFV_REQ_PENDING) return 1;
    if (g_busy && (g_priv.ecb & NSFECB_POSTED)) return 1;
    return 0;
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

    /* ---- 1. Finish a completed request ------------------------------------
     * The executive has run soc_complete on the private copy, so retcode /
     * errno_ / the fn-specific outputs are set.  Copy ONLY those back
     * (ADR-0041 4 -- notably NOT ubuf, which holds the STC's staging address
     * in the private copy), re-check the client, then reply. */
    if (g_busy && (g_priv.ecb & NSFECB_POSTED)) {
        if (__super(PSWKEY0, &savekey)) return;

        nsfreqx_result_out((NSFRQE *)g_anchor->rqe, &g_priv);

        cl    = nsfsx_client_state();
        lascb = (unsigned)g_anchor->req_ascb;
        lasid = g_anchor->req_asid;

        if (cl == NSFSX_CL_DEAD) {
            nsfsx_reap();
            g_busy = 0;
        } else if (cl == NSFSX_CL_UNKNOWN) {
            /* Neither post nor reap.  HELD keeps the slot busy to the SVC
            ** routine while taking it off this drain's work list. */
            g_anchor->req_state = NSFV_REQ_HELD;
            g_busy = 0;
        } else {
            g_anchor->served++;
            g_anchor->req_state = NSFV_REQ_DONE;
            __xmpost(g_anchor->req_ascb, &g_anchor->reply_ecb, 0);
            g_busy = 0;
        }
        __prob(savekey, NULL);

        if (cl == NSFSX_CL_DEAD)
            wtof("NSF050I CLIENT DEAD (ASCB=%08X ASID=%04X) -- REQUEST REAPED",
                 lascb, lasid);
        else if (cl == NSFSX_CL_UNKNOWN)
            wtof("NSF051W CLIENT LIVENESS UNKNOWN (ASCB=%08X ASID=%04X)"
                 " -- REQUEST HELD", lascb, lasid);
    }

    /* ---- 2. Take a newly arrived request ---------------------------------- */
    if (!g_busy && g_anchor->req_state == NSFV_REQ_PENDING) {
        int ok = 0;

        if (__super(PSWKEY0, &savekey)) return;

        cl    = nsfsx_client_state();
        lascb = (unsigned)g_anchor->req_ascb;
        lasid = g_anchor->req_asid;

        if (cl == NSFSX_CL_DEAD) {
            nsfsx_reap();
        } else if (cl == NSFSX_CL_UNKNOWN) {
            g_anchor->req_state = NSFV_REQ_HELD;
        } else if (g_anchor->xfunc == NSFV_REQ_RQE) {
            /* Hop 2: the CSA slot into the STC-private copy.  ubuf is rewritten
            ** to the staging buffer and ulen to the count ACTUALLY staged --
            ** anchor->xlen, the very value the SVC routine's clamp used for the
            ** move, so the op reports what really crossed (ADR-0041 2). */
            nsfreqx_dispatch_in(&g_priv, (const NSFRQE *)g_anchor->rqe,
                                g_anchor->stage, g_anchor->xlen);
            ok = 1;
        } else {
            /* A probe verb reached the production STC: reject cleanly rather
            ** than fall through into a dispatch (the probe STC serves those). */
            g_anchor->req_state = NSFV_REQ_HELD;
        }
        __prob(savekey, NULL);

        if (cl == NSFSX_CL_DEAD)
            wtof("NSF050I CLIENT DEAD (ASCB=%08X ASID=%04X) -- REQUEST REAPED",
                 lascb, lasid);
        else if (cl == NSFSX_CL_UNKNOWN)
            wtof("NSF051W CLIENT LIVENESS UNKNOWN (ASCB=%08X ASID=%04X)"
                 " -- REQUEST HELD", lascb, lasid);

        if (ok) {
            /* Dispatch OUTSIDE the key window: the executive runs in its own
            ** key 8 on ordinary storage, exactly as in Phase 1.  It may
            ** complete the request inline (soc_complete POSTs g_priv.ecb) or
            ** park it -- either way step 1 above picks it up, this pass or a
            ** later one. */
            g_busy = 1;
            nsfreq_dispatch(&g_priv);
        }
    }
}
