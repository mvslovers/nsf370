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
 * Modelled on Stage-0a's nsfp.c (STC lifecycle, in-flight drain, ESTAE) and the
 * CBT/mvs38j-ip SVCTABLE steal (STCPSVC0/STCPSVC9: CVT->CVTABEND->SCVTSVCT).
 * The STC (not the client) self-authorises via clib_apf_setup (SVC 244) for
 * __loadhi / key-0 CSA / the SVCTABLE store -- NSF.LINKLIB need not be APF.
 * The RED LINE is that the CLIENT stays unauthorised; the STC does not.
 */
#ifndef NSFV_VERSION
#define NSFV_VERSION "0.1.0-probe"
#endif

#include "nsfpsvc.h"
#include <string.h>
#include <clibos.h>          /* __super/__prob/__ascb/__xmpost/getmain/loadhi   */
#include <clibwto.h>         /* wtof                                            */
#include <clibcib.h>         /* COM / CIB / __gtcom / __cibget / __cibdel       */
#include <clibstae.h>        /* __estae, ESTAE_CREATE/DELETE                    */
#include <clibsdwa.h>        /* SDWA, SDWACWT                                   */
#include <cvt.h>             /* CVT, CVTPTR                                     */
#include <ihascvt.h>         /* SCVT (scvtsvct), SVCTABLE, SVCENTRY             */

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
        anchor->version   = 1;
        anchor->flags     = NSFV_ANCHOR_ACTIVE;
        anchor->req_state = NSFV_REQ_FREE;
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
 * Service the one pending request: read the token, echo it incremented, bump
 * the served counter, wake the client (cross-AS POST, supervisor state).
 * The SVC routine owns the slot's FREE<->PENDING/DONE lifecycle; this only
 * moves PENDING -> DONE.
 * ============================================================ */
static void
nsfv_service(NSFV_ANCHOR *anchor)
{
    unsigned char savekey;

    if (__super(PSWKEY0, &savekey)) return;

    if (anchor->req_state == NSFV_REQ_PENDING) {
        UINT   t  = anchor->req_token;
        void  *ca = anchor->req_ascb;

        anchor->req_token = t + 1u;             /* echo / increment            */
        anchor->served++;
        anchor->req_state = NSFV_REQ_DONE;      /* PENDING -> DONE             */

        if (ca)
            __xmpost(ca, &anchor->reply_ecb, 0);/* wake the parked client      */
    }

    __prob(savekey, NULL);
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
    void         *ca;

    if (__super(PSWKEY0, &savekey)) return;
    ca = anchor->req_ascb;
    if (ca)
        __xmpost(ca, &anchor->reply_ecb, 0);
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
            wtof("NSFV002I NSFV SERVED=%u INFLIGHT=%u",
                 stc->anchor->served, stc->anchor->inflight);
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
        ** (ADR-0022 reset-before-WAIT + double-check-drain). */
        do {
            if (anchor->req_state == NSFV_REQ_PENDING)
                nsfv_service(anchor);
            nsfv_server_ecb_reset(anchor);
        } while (anchor->req_state == NSFV_REQ_PENDING);

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
