/*
 * nsfp.c -- M5 Stage-0a SSI cross-AS probe: the probe STC (NSFP).
 *
 * ADR-0036.  The target address space for the SSI transport probe.  It owns the
 * CSA rendezvous anchor, dynamically registers the NSFP probe subsystem
 * (SSCT/SSVT into the JESCT chain -- no IEFSSNxx / IPL), loads the thin router
 * (NSFPSSIR) into CSA, and runs an event loop that WAITs (supervisor / key 0)
 * on {server_ecb, console-CIB ECB}.  Per wake it services the one pending
 * request -- increment the token, bump a served counter -- and wakes the client
 * with __xmpost.  No NSFRQE, no socket, no protocol: this proves the transport
 * mechanics on an empty token before M5-2 rides the real request over them.
 *
 * Modelled VERBATIM on ufsd/src/ufsd.c (STC lifecycle, in-flight drain, ESTAE)
 * and ufsd/src/ufsd#csa.c / ufsd#sct.c (CSA + SSCT).  The task authorises itself
 * at runtime via clib_apf_setup (SVC 244) -- NSF.LINKLIB need not be APF (see
 * ADR-0036 -- verified against SYS1.PARMLIB(IEAAPF00)).
 */
#ifndef NSFP_VERSION
#define NSFP_VERSION "0.1.0-probe"
#endif

#include "nsfpssi.h"
#include <string.h>
#include <clibos.h>          /* __super/__prob/__ascb/__xmpost/getmain/loadhi   */
#include <clibwto.h>         /* wtof                                            */
#include <clibcib.h>         /* COM / CIB / __gtcom / __cibget / __cibdel       */
#include <clibssct.h>        /* ssct_new/install/remove/free/find               */
#include <clibssvt.h>        /* ssvt_new/set/funcmap/reset/free                 */
#include <clibstae.h>        /* __estae, ESTAE_CREATE/DELETE                    */
#include <clibsdwa.h>        /* SDWA, SDWACWT                                   */

/* ============================================================
 * STC control block (main's stack).  The CSA anchor stays MINIMAL (cross-AS
 * rendezvous only); the SSCT/SSVT/router registration handles live here, STC-
 * private, so the anchor carries no cleanup pointers the router never reads.
 * ============================================================ */
typedef struct nsfp_stc {
    char          eye[8];       /* "**NSFP**"                                 */
    unsigned      flags;        /* NSFP_STC_ACTIVE                            */
    NSFP_ANCHOR  *anchor;       /* CSA anchor (SP=241)                        */
    SSCT         *ssct;         /* registered SSCT (deregister on stop)       */
    SSVT         *ssvt;         /* SSVT (freed with the SSCT)                 */
    void         *ssir_lpa;     /* NSFPSSIR CSA load-module base (freemain)   */
    unsigned      ssir_size;    /* NSFPSSIR module size                       */
} NSFP_STC;

#define NSFP_STC_ACTIVE   0x80000000U
#define NSFP_SHUT_NORMAL  0
#define NSFP_SHUT_ABEND   1

static void nsfp_shutdown(NSFP_STC *stc, int mode);

/* ============================================================
 * CSA anchor allocation (SP=241, key 0).  Caller task must be APF authorised.
 * ============================================================ */
static NSFP_ANCHOR *
nsfp_anchor_alloc(void)
{
    NSFP_ANCHOR   *anchor;
    unsigned char  savekey;

    if (__super(PSWKEY0, &savekey)) return NULL;
    anchor = (NSFP_ANCHOR *)getmain((unsigned)sizeof(NSFP_ANCHOR), 241);
    if (anchor) {
        memset(anchor, 0, sizeof(NSFP_ANCHOR));
        memcpy(anchor->eye, "NSFPANCR", 8);
        anchor->version = 1;
        anchor->flags   = NSFP_ANCHOR_ACTIVE;
        anchor->req_state = NSFP_REQ_FREE;
    }
    __prob(savekey, NULL);
    return anchor;
}

static void
nsfp_anchor_free(NSFP_ANCHOR *anchor)
{
    unsigned char savekey;

    if (!anchor) return;
    if (__super(PSWKEY0, &savekey)) return;
    /* Invalidate the eye catcher BEFORE releasing the block: freed SP=241
    ** storage is reused, not zeroed, so a client still looping in the router
    ** must not accept the reused anchor as valid (its timeout branch
    ** revalidates "NSFPANCR"). */
    memset(anchor->eye, 0, sizeof(anchor->eye));
    freemain(anchor);
    __prob(savekey, NULL);
}

/* ============================================================
 * SSCT/SSVT registration into the JESCT chain (dynamic; no PARMLIB / IPL).
 * ssctsuse = the anchor, so the router recovers it from ssct_find(NSFP).
 * ============================================================ */
static int
nsfp_ssct_init(NSFP_STC *stc)
{
    unsigned char savekey;
    SSVT         *ssvt;
    SSCT         *ssct;
    int           rc;

    if (__super(PSWKEY0, &savekey)) {
        wtof("NSFP091E CANNOT ENTER SUPERVISOR STATE FOR SSCT INIT");
        return -1;
    }

    ssvt = ssvt_new(NSFP_SSVT_ROUTER);      /* room for one function routine   */
    if (!ssvt) {
        __prob(savekey, NULL);
        wtof("NSFP093E CANNOT ALLOCATE SSVT");
        return -1;
    }

    ssct = ssct_new(NSFP_SUBSYS, ssvt, (void *)stc->anchor);
    if (!ssct) {
        ssvt_free(ssvt);
        __prob(savekey, NULL);
        wtof("NSFP094E CANNOT ALLOCATE SSCT");
        return -1;
    }

    rc = ssct_install(ssct, NULL);
    if (rc) {
        ssct_free(ssct);
        ssvt_free(ssvt);
        __prob(savekey, NULL);
        wtof("NSFP025E CANNOT INSTALL SSCT, RC=%d", rc);
        return -1;
    }

    stc->ssct = ssct;
    stc->ssvt = ssvt;

    __prob(savekey, NULL);
    return 0;
}

static void
nsfp_ssct_free(NSFP_STC *stc)
{
    unsigned char savekey;

    if (!stc->ssct) return;
    if (__super(PSWKEY0, &savekey)) return;

    ssct_remove(stc->ssct);
    ssct_free(stc->ssct);
    ssvt_free(stc->ssvt);

    stc->ssct = NULL;
    stc->ssvt = NULL;

    __prob(savekey, NULL);
}

/* ============================================================
 * Load the router into CSA and register its entry in the SSVT.
 * __loadhi requires supervisor state + key 0.
 * ============================================================ */
static int
nsfp_ssi_load(NSFP_STC *stc)
{
    unsigned char savekey;
    void         *lpa;
    void         *epa;
    unsigned      size;
    int           rc;

    if (!stc->ssvt) return -1;

    if (__super(PSWKEY0, &savekey)) {
        wtof("NSFP026E CANNOT ENTER SUPERVISOR FOR SSI LOAD");
        return -1;
    }

    rc = __loadhi(NSFP_ROUTER_MOD, &lpa, &epa, &size);
    if (rc) {
        __prob(savekey, NULL);
        wtof("NSFP027E CANNOT LOAD %s INTO CSA, RC=%d", NSFP_ROUTER_MOD, rc);
        return -1;
    }

    ssvt_set(stc->ssvt, NSFP_SSVT_ROUTER, epa);
    ssvt_funcmap(stc->ssvt, NSFP_SSVT_ROUTER, NSFP_SSOBFUNC);

    stc->ssir_lpa  = lpa;
    stc->ssir_size = size;

    __prob(savekey, NULL);
    return 0;
}

static void
nsfp_ssi_unload(NSFP_STC *stc)
{
    unsigned char savekey;

    if (!stc->ssir_lpa) return;
    if (__super(PSWKEY0, &savekey)) return;

    freemain(stc->ssir_lpa);
    stc->ssir_lpa  = NULL;
    stc->ssir_size = 0;

    __prob(savekey, NULL);
}

/* ============================================================
 * Service the one pending request: read the token, echo it incremented, bump
 * the served counter, then wake the client (cross-AS POST, supervisor state).
 * The router owns the slot's FREE<->PENDING/DONE lifecycle; this only moves
 * PENDING -> DONE.
 * ============================================================ */
static void
nsfp_service(NSFP_ANCHOR *anchor)
{
    unsigned char savekey;

    if (__super(PSWKEY0, &savekey)) return;

    if (anchor->req_state == NSFP_REQ_PENDING) {
        UINT   t  = anchor->req_token;
        void  *ca = anchor->req_ascb;
        ECB   *ce = anchor->req_ecb;

        anchor->req_token = t + 1u;             /* echo / increment             */
        anchor->served++;
        anchor->req_state = NSFP_REQ_DONE;      /* PENDING -> DONE              */

        if (ca && ce)
            __xmpost(ca, ce, 0);                /* wake the parked client       */
    }

    __prob(savekey, NULL);
}

static void
nsfp_server_ecb_reset(NSFP_ANCHOR *anchor)
{
    unsigned char savekey;

    if (__super(PSWKEY0, &savekey)) return;
    anchor->server_ecb = 0;
    __prob(savekey, NULL);
}

/* ============================================================
 * In-flight drain: poll anchor->inflight to zero before freeing CSA.  A client
 * parked in the router needs up to one NSFP_WAIT_INTERVAL (5 s) to notice a
 * cleared ANCHOR_ACTIVE, so the ceiling exceeds that.  A drain timeout means
 * "retain CSA", never "free anyway".  anchor->inflight is written from another
 * address space -- volatile, read from problem state (no fetch protection).
 * ============================================================ */
#define NSFP_DRAIN_POLL    10U   /* 0.10 s per poll                            */
#define NSFP_DRAIN_MAX    100U   /* 100 * 0.10 s = 10 s ceiling                */
#define NSFP_DRAIN_SETTLE  20U   /* 0.20 s after inflight reaches zero         */

static void
nsfp_pause(unsigned hsec)
{
    ECB local = 0;                          /* nobody posts it; the timer does  */
    ecb_timed_wait(&local, hsec, 0);
}

static int
nsfp_drain(NSFP_ANCHOR *anchor)
{
    volatile unsigned *inflight = &anchor->inflight;
    unsigned           n;

    for (n = 0; n < NSFP_DRAIN_MAX; n++) {
        if (*inflight == 0) {
            nsfp_pause(NSFP_DRAIN_SETTLE);
            return (*inflight == 0);         /* re-check: catch a racing entry  */
        }
        nsfp_pause(NSFP_DRAIN_POLL);
    }
    return 0;
}

/* ============================================================
 * MODIFY / STOP handling.  STOP (P NSFP, or F NSFP,STOP) shuts down cleanly --
 * this is the double-start test: after a clean stop the SSCT is deregistered,
 * so S NSFP works again.  Any other MODIFY reports the probe counters.
 * ============================================================ */
static void
nsfp_process_cib(NSFP_STC *stc, CIB *cib)
{
    if (cib->cibverb == CIBSTOP) {
        wtof("NSFP010I NSFP STOP RECEIVED");
        stc->flags &= ~NSFP_STC_ACTIVE;
        return;
    }
    if (cib->cibverb == CIBMODFY) {
        if (cib->cibdatln >= 4 && memcmp(cib->cibdata, "STOP", 4) == 0) {
            wtof("NSFP010I NSFP STOP RECEIVED (MODIFY)");
            stc->flags &= ~NSFP_STC_ACTIVE;
            return;
        }
        if (stc->anchor)
            wtof("NSFP002I NSFP SERVED=%u INFLIGHT=%u",
                 stc->anchor->served, stc->anchor->inflight);
        else
            wtof("NSFP002I NSFP NO ANCHOR");
        return;
    }
    /* CIBSTART and others: ignore. */
}

/* ============================================================
 * Shutdown -- the ONE teardown path (also the ESTAE-abend path).
 * ============================================================ */
static void
nsfp_shutdown(NSFP_STC *stc, int mode)
{
    NSFP_ANCHOR   *anchor;
    unsigned char  savekey;

    /* Delete ESTAE first: no re-entrant recovery if shutdown itself faults. */
    __estae(ESTAE_DELETE, NULL, NULL);

    anchor = stc->anchor;

    if (anchor) {
        /* Close the door: null the SSVT function entry BEFORE anything is
        ** freed, so IEFSSREQ stops routing new clients into the router (which
        ** is code inside the very CSA module about to be FREEMAINed).  Then
        ** clear ACTIVE so parked clients bail on their next timeout. */
        if (__super(PSWKEY0, &savekey)) {
            wtof("NSFP097E CANNOT ENTER SUPERVISOR STATE -- CSA RETAINED");
            return;                          /* free nothing */
        }
        if (stc->ssvt) {
            ssvt_reset(stc->ssvt, NSFP_SSVT_ROUTER);
            ssvt_funcmap(stc->ssvt, 0, NSFP_SSOBFUNC);
        }
        anchor->flags &= ~NSFP_ANCHOR_ACTIVE;
        __prob(savekey, NULL);
    }

    if (!anchor)
        goto done;

    if (mode == NSFP_SHUT_ABEND) {
        /* Under RTM we cannot drain and must not free: a foreign PSW may still
        ** be executing inside the router or touching the anchor.  Leave
        ** everything and percolate.  The SSCT stays registered until IPL. */
        wtof("NSFP098W ABEND SHUTDOWN -- CSA RETAINED, IPL TO CLEAR SSCT");
        goto done;
    }

    /* Drain in-flight clients out of the router. */
    if (!nsfp_drain(anchor)) {
        wtof("NSFP098W %u CLIENT(S) STILL IN FLIGHT -- CSA RETAINED",
             anchor->inflight);
        goto done;                           /* free nothing */
    }

    /* Deregister the SSCT (frees the SSVT), unload the router, free the anchor
    ** -- same order as UFSD. */
    if (stc->ssct) {
        nsfp_ssct_free(stc);
        wtof("NSFP095I SSCT DEREGISTERED");
    }
    if (stc->ssir_lpa) {
        nsfp_ssi_unload(stc);
        wtof("NSFP036I SSI ROUTER UNLOADED");
    }
    nsfp_anchor_free(anchor);
    stc->anchor = NULL;

done:
    wtof("NSFP011I NSFP SHUTDOWN COMPLETE");
}

/* ============================================================
 * ESTAE recovery.  Close the door + clear ACTIVE (UFSD_SHUT_ABEND), then
 * percolate for the MVS dump.  CSA is NOT freed under RTM.
 * ============================================================ */
static void
nsfp_recover(SDWA *sdwa)
{
    NSFP_STC *stc;

    if (!sdwa) return;
    stc = (NSFP_STC *)sdwa->SDWAPARM;

    wtof("NSFP900E NSFP ABEND INTERCEPTED -- EMERGENCY SHUTDOWN");

    if (stc) {
        stc->flags &= ~NSFP_STC_ACTIVE;
        nsfp_shutdown(stc, NSFP_SHUT_ABEND);
    }

    sdwa->SDWARCDE = SDWACWT;                /* percolate: MVS dumps + ends AS   */
}

/* ============================================================
 * main
 * ============================================================ */
int
main(int argc, char **argv)
{
    NSFP_STC      stc;
    NSFP_ANCHOR  *anchor;
    COM          *com;
    CIB          *cib;
    unsigned     *ecblist[3];               /* {console, server_ecb} + sentinel */
    unsigned      count;
    int           rc;

    (void)argc;

    memset(&stc, 0, sizeof(stc));
    memcpy(stc.eye, "**NSFP**", 8);
    stc.flags = NSFP_STC_ACTIVE;

    /* --- Console interface --- */
    com = __gtcom();
    if (!com) {
        wtof("NSFP090E UNABLE TO INITIALIZE CONSOLE INTERFACE");
        return 8;
    }
    __cibset(5);                            /* CIBSTART + up to 4 MODIFYs       */

    /* --- APF authorization (SVC 244 -- library need not be APF) --- */
    rc = clib_apf_setup(argv[0]);
    if (rc) {
        wtof("NSFP091E APF SETUP FAILED RC=%d", rc);
        return 8;
    }

    /* --- ESTAE recovery (established before the SSCT is registered) --- */
    __estae(ESTAE_CREATE, (void *)nsfp_recover, &stc);

    wtof("NSFP000I NSFP SSI PROBE %s STARTING", NSFP_VERSION);

    /* Refuse to start over a stale registration (an earlier abend left the
    ** SSCT in the JESCT chain).  A clean stop deregisters it, so S NSFP after
    ** P NSFP works; only an abended run needs an IPL to clear it. */
    if (ssct_find(NSFP_SUBSYS)) {
        __estae(ESTAE_DELETE, NULL, NULL);
        wtof("NSFP092E SUBSYSTEM %s ALREADY REGISTERED -- IPL TO CLEAR",
             NSFP_SUBSYS);
        return 8;
    }

    /* --- CSA anchor --- */
    anchor = nsfp_anchor_alloc();
    if (!anchor) {
        __estae(ESTAE_DELETE, NULL, NULL);
        wtof("NSFP093E CANNOT ALLOCATE CSA ANCHOR");
        return 8;
    }
    stc.anchor = anchor;

    /* Record the STC ASCB in the anchor (the router's __xmpost target). */
    {
        unsigned char savekey;
        if (!__super(PSWKEY0, &savekey)) {
            anchor->server_ascb = __ascb(0);
            __prob(savekey, NULL);
        }
    }

    /* --- Register the subsystem + load the router --- */
    if (nsfp_ssct_init(&stc)) {
        nsfp_shutdown(&stc, NSFP_SHUT_NORMAL);   /* no clients yet: drains at once */
        return 8;
    }
    wtof("NSFP034I SSCT REGISTERED, SUBSYSTEM=%s", NSFP_SUBSYS);

    if (nsfp_ssi_load(&stc)) {
        nsfp_shutdown(&stc, NSFP_SHUT_NORMAL);
        return 8;
    }
    wtof("NSFP035I SSI ROUTER LOADED AT %08X", (unsigned)stc.ssir_lpa);

    wtof("NSFP001I NSFP READY -- ANCHOR=%08X ASCB=%08X",
         (unsigned)anchor, (unsigned)anchor->server_ascb);

    /* --- Main event loop --- */
    while (stc.flags & NSFP_STC_ACTIVE) {
        /* Drain all pending CIBs unconditionally (the startup CIBSTART trap:
        ** MVS may queue it without posting the ECB). */
        while ((cib = __cibget()) != NULL) {
            nsfp_process_cib(&stc, cib);
            __cibdel(cib);
            if (!(stc.flags & NSFP_STC_ACTIVE)) break;
        }
        if (!(stc.flags & NSFP_STC_ACTIVE)) break;

        /* Service the pending request: reset server_ecb, then double-check for
        ** a request that arrived between the last service and the reset
        ** (ADR-0022 reset-before-WAIT + double-check-drain). */
        do {
            if (anchor->req_state == NSFP_REQ_PENDING)
                nsfp_service(anchor);
            nsfp_server_ecb_reset(anchor);
        } while (anchor->req_state == NSFP_REQ_PENDING);

        if (!(stc.flags & NSFP_STC_ACTIVE)) break;

        /* WAIT for the next console event OR an incoming SSI request.  High bit
        ** on the LAST pointer marks end of ECBLIST; a posted ECB returns at
        ** once.  server_ecb is in CSA (key 0), so WAIT in supervisor state. */
        count = 0;
        if (com->comecbpt) {
            ecblist[count++] = (unsigned *)com->comecbpt;
        }
        ecblist[count++] = (unsigned *)&anchor->server_ecb;
        /* mark the last entry */
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

    nsfp_shutdown(&stc, NSFP_SHUT_NORMAL);       /* clean STOP */
    return 0;
}
