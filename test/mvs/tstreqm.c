/*
 * tstreqm.c -- NSFREQ same-address-space transport round-trip ON MVS (M3-2).
 *
 * MVS-only (project.toml host = false): it drives the REAL Phase-1 request
 * transport -- an in-address-space request queue (NSFXQ) + a requestECB the
 * executive WAITs in its ECBLIST, with a cthread APP SUBTASK as the producer and
 * the executive main task as the consumer. This is what the host TSTREQ pthread
 * round-trip stands in for; here it runs over ecb_post (SVC 2) / ecb_wait (SVC 1)
 * on a real other TCB, so the queue+POST+WAIT seam is proven on 3.8j.
 *
 *   main task (EXECUTIVE): build the pools, wire the request seam
 *     (evt_set_request), register a dummy protocol, ATTACH the app subtask, arm
 *     the ADR-0017 liveness heartbeat, and run evt_mainloop. The loop WAITs on
 *     {timerECB, requestECB, ...}; the subtask's ecb_post of requestECB wakes it
 *     (the proven TSTCTHR pattern), it drains + dispatches, and completes each
 *     request with ecb_post of the request's own ecb.
 *   app subtask (PRODUCER): INITAPI -> SOCKET -> CLOSE -> TERMAPI, each via
 *     nsfreq_call (submit + WAIT on the request's ecb), then nsfevt_stop() and
 *     return. The heartbeat guarantees the executive observes g_stop within
 *     ~100 ms (nsfevt_stop only sets the bit on MVS -- the loop's liveness is the
 *     heartbeat, exactly as in nsfmain.c).
 *
 * NO user-visible feature: sockets are still unreachable from EZASOKET (that is
 * M3-3 UDP + NSFEZA). This proves the M3-2 backbone, nothing more.
 *
 * Diagnosing (read build/test-runner.spool):
 *   CC 0 + all retcodes ok + pools at baseline -> the transport round-trips.
 *   hang / S322                                 -> a request was lost (never
 *      dispatched) or the requestECB POST did not wake the executive's WAIT.
 */
#include "nsfreq.h"
#include "nsfsoc.h"
#include "nsfevt.h"
#include "nsfthr.h"
#include "nsfstim.h"            /* nsftmr_plat_arm (the liveness heartbeat)     */
#include "nsftmr.h"
#include "nsftrc.h"
#include "nsfbuf.h"
#include "nsfsts.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>

#define DUMMY_PROTO   254u

/* ---- test-only dummy protocol (only attach/detach are exercised here) ------- */
static int g_attach, g_detach;
static int d_attach(SOCKCB *s)             { (void)s; g_attach++; return 0; }
static int d_detach(SOCKCB *s)             { (void)s; g_detach++; return 0; }
static int d_bind(SOCKCB *s)               { (void)s; return 0; }
static int d_connect(SOCKCB *s, NSFRQE *r) { (void)s; (void)r; return 0; }
static int d_listen(SOCKCB *s, int bl)     { (void)s; (void)bl; return 0; }
static int d_send(SOCKCB *s, NSFRQE *r)    { (void)s; (void)r; return 0; }
static int d_recv(SOCKCB *s, NSFRQE *r)    { (void)s; (void)r; return 0; }
static int d_close(SOCKCB *s, NSFRQE *r)   { (void)s; (void)r; return 0; }

static PROTOPS dummy_ops = {
    .attach = d_attach, .bind = d_bind, .connect = d_connect, .listen = d_listen,
    .send = d_send, .recv = d_recv, .close = d_close, .detach = d_detach
};

/* ---- the app subtask (producer) -------------------------------------------- */
static INT  g_ini_rc, g_sock_rc, g_close_rc, g_term_rc;
static UINT g_apptok, g_desc;

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
}

static int app_sub(void *arg)
{
    NSFRQE ri, rs, rc, rt;

    (void)arg;

    rqe_init(&ri, RQ_INITAPI, 0u);
    nsfreq_call(&ri);                       /* submit + WAIT on ri.ecb (SVC 1)  */
    g_ini_rc = ri.retcode;
    g_apptok = ri.apptok;

    rqe_init(&rs, RQ_SOCKET, 0u);
    rs.apptok = g_apptok;
    rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_DGRAM; rs.p3 = DUMMY_PROTO;
    nsfreq_call(&rs);
    g_sock_rc = rs.retcode;
    g_desc    = (UINT)rs.retcode;

    rqe_init(&rc, RQ_CLOSE, g_desc);
    nsfreq_call(&rc);
    g_close_rc = rc.retcode;

    rqe_init(&rt, RQ_TERMAPI, 0u);
    rt.apptok = g_apptok;
    nsfreq_call(&rt);
    g_term_rc = rt.retcode;

    nsfevt_stop();                          /* ask the executive to shut down   */
    return 0;
}

int main(void)
{
    NSFTHR *t;

    printf("=== nsf370 NSFREQ MVS round-trip (queue + requestECB + SVC 2/1) ===\n");

    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) {
        printf("nsfevt_init failed\n");
        return 1;
    }
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    mm_init_complete();

    soc_init();
    nsfreq_init();
    CHECK_EQ((long)nsfreq_register_proto((UCHAR)DUMMY_PROTO, &dummy_ops), 0L,
             "register dummy protocol");
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);                    /* ~100 ms heartbeat -> stop liveness */
    evt_mainloop();                         /* runs until the subtask STOPs it   */

    CHECK_EQ((long)nsfthr_join(t, 20u), 0, "app subtask joined");

    printf("INITAPI rc=%d tok=%08X  SOCKET rc=%d  CLOSE rc=%d  TERMAPI rc=%d\n",
           (int)g_ini_rc, (unsigned)g_apptok, (int)g_sock_rc,
           (int)g_close_rc, (int)g_term_rc);

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "INITAPI round-tripped ok");
    CHECK(g_apptok != 0u, "INITAPI returned an app token");
    CHECK(g_sock_rc >= 0, "SOCKET round-tripped and returned a descriptor");
    CHECK_EQ((long)g_attach, 1L, "SOCKET ran the protocol attach on the executive");
    CHECK_EQ((long)g_close_rc, (long)NSF_RETOK, "CLOSE round-tripped ok");
    CHECK_EQ((long)g_detach, 1L, "CLOSE ran the protocol detach");
    CHECK_EQ((long)g_term_rc, (long)NSF_RETOK, "TERMAPI round-tripped ok");
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open (leak gate)");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "EVT pool at baseline after shutdown");

    mm_shutdown();
    return mbt_test_summary("TSTREQM");
}
