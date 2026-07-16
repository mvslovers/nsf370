/*
 * tsttcph.c -- NSFTCP on-MVS live gate: the full stack, both handshake
 * directions + a clean FIN teardown over the real 0500/0501 CTCI pair (M4-2).
 * host = false. The M4-2 connection machine driven by a cthread APP SUBTASK (the
 * tstudpm / tstezam pattern) over the real wire -- no EZASOKET yet (M4-5), so the
 * subtask builds NSFRQEs directly.
 *
 *   main task (EXECUTIVE): build the pools, wire the request seam + CTCI device +
 *     IP + TCP, ATTACH the app subtask, arm the ADR-0017 heartbeat, run
 *     evt_mainloop. The loop drives the CTCI subtasks (service/kick, incl. the
 *     ADR-0030 rarmed read-park for the locally-originated active SYN) and drains
 *     + dispatches requests.
 *   app subtask (SVC 2/1 on a real other TCB):
 *     PASSIVE  -- SOCKET(STREAM) -> BIND(PASSIVE_PORT) -> LISTEN -> a bounded
 *                 non-blocking ACCEPT poll (~30 s). A host `nc 192.168.200.1 3000`
 *                 completes the 3WHS; the subtask then closes the accepted child
 *                 (our FIN) so a clean FIN exchange is on the wire either way
 *                 (nc-closes-first -> CLOSE_WAIT -> our FIN; we-close-first ->
 *                 FIN_WAIT_1). CLOSE the listener.
 *     ACTIVE   -- SOCKET(STREAM) -> CONNECT to 192.168.200.2:ACTIVE_PORT (the
 *                 LOCALLY-ORIGINATED SYN on an idle link -- the ADR-0030 rarmed
 *                 path) -> blocks until the SYN|ACK (a host `nc -l 3001`) or a RST
 *                 (ECONNREFUSED) -> CLOSE. Both directions land in host tcpdump.
 *     TERMAPI -> nsfevt_stop.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like TSTUDPM. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 * Without it, a device-free skeleton that only proves the machinery links + the
 * CONNECT fails cleanly with no route.
 *
 * Live procedure (host mvsdev, with `tcpdump -ni tun0 tcp` up FIRST so "did the
 * SYN leave?" is answered instantly):
 *   1. PASSIVE : `nc </dev/null 192.168.200.1 3000` -> SYN / SYN,ACK / ACK, then a
 *                clean FIN exchange. NOTE `</dev/null` (send NO payload): M4-2 has
 *                no data path, so data ahead of the FIN leaves RCV.NXT behind and
 *                the FIN is not in-order -- payload desyncs the clean-FIN evidence.
 *                TCP passiveopen +1, established +1.
 *   2. ACTIVE  : the idle-link guest SYN (ADR-0030 rarmed read-park -- the passive
 *                close's FIN already exercised this write path). With `nc -l 3001`
 *                listening -> SYN,ACK -> guest ACK (activeopen +1, established +1);
 *                WITHOUT it, a closed host port RSTs promptly -> ECONNREFUSED
 *                (activeopen +1, resetrcvd +1) -- the guaranteed-terminating case.
 *                CONNECT is BLOCKING with no timeout (ETIMEDOUT is M4-4): the only
 *                job-hang is "SYN never left the wire" (a write-path regression) --
 *                watch tcpdump, be ready to `P NSF`.
 *   3. TEARDOWN: a clean 4-way FIN in tcpdump; the TCB pool + SOCKET pool return
 *                to baseline at shutdown (leak gate).
 */
#include "nsftcp.h"
#include "nsfreq.h"
#include "nsfsoc.h"
#include "nsfip.h"
#include "nsficmp.h"
#include "nsfdev.h"
#include "nsfctci.h"
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

#ifndef NSFCTCI_SRC
#define NSFCTCI_SRC  0xC0A8C801u    /* 192.168.200.1 (guest HOME)             */
#endif
#ifndef NSFCTCI_DST
#define NSFCTCI_DST  0xC0A8C802u    /* 192.168.200.2 (host peer)              */
#endif
#ifndef NSFCTCI_MTU
#define NSFCTCI_MTU  1500u
#endif

#define PASSIVE_PORT  3000u         /* host `nc 192.168.200.1 3000`           */
#define ACTIVE_PORT   3001u         /* host `nc -l 3001`                      */

/* ---- the app subtask (producer) -------------------------------------------- */
static INT  g_ini_rc, g_lsock_rc, g_bind_rc, g_listen_rc;
static INT  g_accept_rc, g_asock_rc, g_conn_rc, g_conn_errno, g_term_rc;
static int  g_accepted;             /* a passive 3WHS completed               */
static UINT g_apptok, g_ldesc, g_child, g_adesc;
static volatile int g_have_device;

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
}

static int app_sub(void *arg)
{
    NSFRQE ri, rs, rb, rl, ra, rcl, rt;
    NSFECB naptmr;                          /* dummy: a never-posted sleep ECB  */
    int    i;

    (void)arg;

    rqe_init(&ri, RQ_INITAPI, 0u);
    nsfreq_call(&ri);
    g_ini_rc = ri.retcode;
    g_apptok = ri.apptok;

    /* ---- PASSIVE: LISTEN + a bounded non-blocking ACCEPT poll ---- */
    rqe_init(&rs, RQ_SOCKET, 0u);
    rs.apptok = g_apptok;
    rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_STREAM; rs.p3 = 6u;
    nsfreq_call(&rs);
    g_lsock_rc = rs.retcode;
    g_ldesc    = (UINT)rs.retcode;

    rqe_init(&rb, RQ_BIND, g_ldesc);
    rb.p1 = (UINT)NSFCTCI_SRC; rb.p2 = PASSIVE_PORT;
    nsfreq_call(&rb);
    g_bind_rc = rb.retcode;

    rqe_init(&rl, RQ_LISTEN, g_ldesc);
    rl.p1 = 4u;                             /* backlog                          */
    nsfreq_call(&rl);
    g_listen_rc = rl.retcode;

    naptmr = 0u;
    g_accept_rc = NSF_RETERR;
    for (i = 0; g_have_device && i < 60; i++) {     /* ~30 s window for nc       */
        rqe_init(&ra, RQ_ACCEPT, g_ldesc);
        ra.flags = (USHORT)RQ_F_NONBLOCK;
        nsfreq_call(&ra);
        if (ra.retcode >= 0) {              /* a connection was accepted        */
            g_accept_rc = ra.retcode;
            g_child     = (UINT)ra.retcode;
            g_accepted  = 1;
            break;
        }
        nsfthr_timed_wait(&naptmr, 5u);     /* ~500 ms nap on our OWN TCB        */
    }
    if (g_accepted) {
        /* Let the peer's close (if any) reach us -> CLOSE_WAIT; then our close
         * drives a clean FIN exchange from whichever side started it. */
        nsfthr_timed_wait(&naptmr, 20u);    /* ~2 s                             */
        rqe_init(&rcl, RQ_CLOSE, g_child);
        nsfreq_call(&rcl);
    }
    rqe_init(&rcl, RQ_CLOSE, g_ldesc);      /* close the listener               */
    nsfreq_call(&rcl);

    /* ---- ACTIVE: CONNECT out to a host `nc -l ACTIVE_PORT` ---- */
    rqe_init(&rs, RQ_SOCKET, 0u);
    rs.apptok = g_apptok;
    rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_STREAM; rs.p3 = 6u;
    nsfreq_call(&rs);
    g_asock_rc = rs.retcode;
    g_adesc    = (UINT)rs.retcode;

    if (g_have_device && g_asock_rc >= 0) {
        rqe_init(&ra, RQ_CONNECT, g_adesc);
        ra.p1 = (UINT)NSFCTCI_DST; ra.p2 = ACTIVE_PORT;
        nsfreq_call(&ra);                   /* blocks: SYN|ACK or RST           */
        g_conn_rc    = ra.retcode;
        g_conn_errno = ra.errno_;
        nsfthr_timed_wait(&naptmr, 10u);    /* ~1 s so the peer sees us          */
        rqe_init(&rcl, RQ_CLOSE, g_adesc);
        nsfreq_call(&rcl);
    }

    rqe_init(&rt, RQ_TERMAPI, 0u);
    rt.apptok = g_apptok;
    nsfreq_call(&rt);
    g_term_rc = rt.retcode;

    nsfevt_stop();
    return 0;
}

/* EV_PACKET_RECEIVED terminus: hand each received packet to the IP layer. */
static void rx_packet(EVT *ev)
{
    PBUF   *b   = (PBUF *)ev->p1;
    NETDEV *dev = dev_by_index(ev->u1);
    if (b != NULL) {
        nsfip_input(dev, b);                /* takes ownership */
    }
}

static void make_cfg(DEVCFG *cfg, const char *name, USHORT cuu, USHORT mtu)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->name, name, strlen(name));
    cfg->cuu    = cuu;
    cfg->type   = NSFDEV_T_CTCI;
    cfg->ipaddr = (UINT)NSFCTCI_SRC;
    cfg->mtu    = mtu;
}

int main(void)
{
    NSFTHR *t;
    DEVCFG  cfg;
    NETDEV *dev = NULL;

    printf("=== nsf370 NSFTCP MVS live gate (CTCI + IP + TCP handshake/teardown) ===\n");

    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) { printf("nsfevt_init failed\n"); return 1; }
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    CHECK(nsftcp_reserve(0) == 0, "nsftcp_reserve (TCPTCB pool)");
    CHECK(ctci_reserve(1u, CTCI_BUF_DEFAULT) == 0, "ctci_reserve");
    mm_init_complete();

    soc_init();
    nsfreq_init();
    nsfip_init();
    nsficmp_init();
    nsftcp_init();                          /* registers the IP demux for 6 */
    CHECK_EQ((long)nsfreq_register_proto(6u, nsftcp_protops()), 0L,
             "register TCP protocol");
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);
    evt_register(EV_PACKET_RECEIVED, rx_packet);

    dev_init();
#ifdef NSFCTCI_CUU
    make_cfg(&cfg, "CTCA", (USHORT)NSFCTCI_CUU, (USHORT)NSFCTCI_MTU);
    dev = dev_register(&cfg, ctci_devops());
    CHECK(dev != NULL, "dev_register (CTCI)");
    CHECK(dev != NULL && dev_start(dev) == 0, "dev_start (SVC 99 + subtasks OPEN)");
    if (dev != NULL) {
        nsfip_local_add((UINT)NSFCTCI_SRC);
        CHECK_EQ((long)nsfip_route_add(0u, 0u, dev, 0u), 0L, "default route -> CTCI");
        g_have_device = 1;
    }
#else
    (void)cfg;
    printf("--- device-free skeleton (no NSFCTCI_CUU): links + CONNECT no-route ---\n");
    nsfip_local_add((UINT)NSFCTCI_SRC);
#endif

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);                    /* ~100 ms heartbeat -> stop liveness */
    evt_mainloop();

    CHECK_EQ((long)nsfthr_join(t, 90u), 0, "app subtask joined");

    printf("INITAPI rc=%d tok=%08X | PASSIVE: LSOCK rc=%d BIND rc=%d LISTEN rc=%d ACCEPT=%d rc=%d\n",
           (int)g_ini_rc, (unsigned)g_apptok, (int)g_lsock_rc, (int)g_bind_rc,
           (int)g_listen_rc, g_accepted, (int)g_accept_rc);
    printf("ACTIVE: SOCK rc=%d CONNECT rc=%d errno=%d | TERMAPI rc=%d\n",
           (int)g_asock_rc, (int)g_conn_rc, (int)g_conn_errno, (int)g_term_rc);
    printf("STATS tcp active=%u passive=%u estab=%u rstsent=%u rstrcvd=%u twreclaim=%u\n",
           sts_value("NSFTCP", "activeopen"),  sts_value("NSFTCP", "passiveopen"),
           sts_value("NSFTCP", "established"), sts_value("NSFTCP", "resetsent"),
           sts_value("NSFTCP", "resetrcvd"),  sts_value("NSFTCP", "twreclaim"));

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "INITAPI round-tripped");
    CHECK(g_lsock_rc >= 0, "listen SOCKET returned a descriptor");
    CHECK_EQ((long)g_bind_rc, (long)NSF_RETOK, "BIND ok");
    CHECK_EQ((long)g_listen_rc, (long)NSF_RETOK, "LISTEN ok");
    CHECK_EQ((long)g_term_rc, (long)NSF_RETOK, "TERMAPI round-tripped");
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open (leak gate)");
#if NSF_DEBUG
    CHECK_EQ((long)nsftcp_debug_inuse(), 0L, "TCPTCB pool at baseline (leak gate)");
#endif

#ifdef NSFCTCI_CUU
    if (dev != NULL) {
        dev_shutdown(dev);
    }
#endif
    mm_shutdown();
    return mbt_test_summary("TSTTCPH");
}
