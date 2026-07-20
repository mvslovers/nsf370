/*
 * tsttcpw.c -- NSFTCP on-MVS live gate: TIME_WAIT reclaim under pool pressure
 * (M4-6 Part D). host = false. A cthread APP SUBTASK (the tsttcph pattern) loops
 * N ACTIVE connect->close cycles against a host listener over the real 0500/0501
 * CTCI pair. The guest is the ACTIVE closer, so its side enters TIME_WAIT each
 * cycle; the cycles run faster than the 2MSL drain, so with a small TCB pool the
 * guest's own accumulated TIME_WAITs fill it and a further connection must RECLAIM
 * the oldest (tcp_attach, the M4-6 fold-in fix) -- else the guest walls at EMFILE.
 * The gate: all N connect->close succeed and twreclaim > 0.
 *
 *   main task (EXECUTIVE): pools + request seam + CTCI + IP + TCP, ATTACH the app
 *     subtask, arm the ADR-0017 heartbeat, evt_mainloop.
 *   app subtask: INITAPI; then N x { SOCKET(STREAM) -> CONNECT
 *     192.168.200.2:LISTEN_PORT -> CLOSE (active) -> short nap so the FIN exchange
 *     completes and the guest reaches TIME_WAIT }; TERMAPI -> nsfevt_stop.
 *
 * A SMALL TCB pool (TWPOOL, default 32) with more cycles (TWCYCLES, default 40)
 * guarantees the pool fills with guest TIME_WAITs before the run ends.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like tsttcph. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 *
 * HOST SIDE (mvsdev): a listener that lets the CLIENT close first, so the guest is
 * the active closer (-> guest TIME_WAIT). `nc -lk 3002` keeps its send side open
 * until the client FINs, which works; the bundled script is deterministic:
 *     python3 samples/host/twreclaim_listener.py 192.168.200.2 3002
 * With `tcpdump -ni tun0 tcp port 3002` up first, each cycle shows SYN/SYN,ACK/ACK
 * then a clean 4-way FIN with the GUEST sending the first FIN.
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
#define NSFCTCI_DST  0xC0A8C802u    /* 192.168.200.2 (host peer, the listener)*/
#endif
#ifndef NSFCTCI_MTU
#define NSFCTCI_MTU  1500u
#endif

#define LISTEN_PORT  3002u          /* host `twreclaim_listener.py ... 3002`  */
#ifndef TWPOOL
#define TWPOOL       32u            /* small TCB pool -> TIME_WAITs fill it    */
#endif
#ifndef TWCYCLES
#define TWCYCLES     40             /* > TWPOOL -> forces reclaim              */
#endif

/* ---- the app subtask (producer) -------------------------------------------- */
static INT  g_ini_rc, g_term_rc;
static int  g_connected, g_cycles_run, g_sock_fail, g_conn_fail, g_last_errno;
static UINT g_apptok, g_recl_before, g_recl_after;
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
    NSFRQE ri, rs, rc, rcl, rt;
    NSFECB naptmr;
    int    i;

    (void)arg;
    g_sock_fail = -1;
    g_conn_fail = -1;

    rqe_init(&ri, RQ_INITAPI, 0u);
    nsfreq_call(&ri);
    g_ini_rc = ri.retcode;
    g_apptok = ri.apptok;

    g_recl_before = sts_value("NSFTCP", "twreclaim");

    for (i = 0; g_have_device && i < TWCYCLES; i++) {
        UINT adesc;

        naptmr = 0u;
        rqe_init(&rs, RQ_SOCKET, 0u);
        rs.apptok = g_apptok;
        rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_STREAM; rs.p3 = 6u;
        nsfreq_call(&rs);
        if (rs.retcode < 0) {               /* EMFILE wall (no active reclaim)  */
            g_sock_fail  = i;
            g_last_errno = rs.errno_;
            break;
        }
        adesc = (UINT)rs.retcode;

        rqe_init(&rc, RQ_CONNECT, adesc);
        rc.p1 = (UINT)NSFCTCI_DST; rc.p2 = LISTEN_PORT;
        nsfreq_call(&rc);                   /* blocks: SYN|ACK or RST           */
        if (rc.retcode == NSF_RETOK) {
            g_connected++;
        } else if (g_conn_fail < 0) {
            g_conn_fail  = i;
            g_last_errno = rc.errno_;
        }

        rqe_init(&rcl, RQ_CLOSE, adesc);    /* ACTIVE close -> guest TIME_WAIT  */
        nsfreq_call(&rcl);

        /* Let the FIN exchange complete (BSD-immediate close finishes in the
         * background on the executive) so the guest reaches TIME_WAIT before the
         * next cycle. NO long wait -> the TIME_WAITs do not drain (2MSL = 60 s). */
        nsfthr_timed_wait(&naptmr, 3u);     /* ~300 ms                          */
        g_cycles_run = i + 1;
    }

    g_recl_after = sts_value("NSFTCP", "twreclaim");

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

    printf("=== nsf370 NSFTCP MVS live gate (TIME_WAIT reclaim under pool pressure) ===\n");

    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) { printf("nsfevt_init failed\n"); return 1; }
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    CHECK(nsftcp_reserve(TWPOOL) == 0, "nsftcp_reserve (small TCPTCB pool)");
    CHECK(ctci_reserve(1u, CTCI_BUF_DEFAULT) == 0, "ctci_reserve");
    mm_init_complete();

    soc_init();
    nsfreq_init();
    nsfip_init();
    nsficmp_init();
    nsftcp_init();
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
    printf("--- device-free skeleton (no NSFCTCI_CUU): links + no cycles run ---\n");
    nsfip_local_add((UINT)NSFCTCI_SRC);
#endif

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);                    /* ~100 ms heartbeat -> stop liveness */
    evt_mainloop();

    CHECK_EQ((long)nsfthr_join(t, 90u), 0, "app subtask joined");

    printf("INITAPI rc=%d tok=%08X | cycles_run=%d connected=%d sock_fail=%d conn_fail=%d errno=%d\n",
           (int)g_ini_rc, (unsigned)g_apptok, g_cycles_run, g_connected,
           g_sock_fail, g_conn_fail, g_last_errno);
    printf("POOL=%u CYCLES=%d | twreclaim before=%u after=%u delta=%u | TERMAPI rc=%d\n",
           (unsigned)TWPOOL, TWCYCLES, g_recl_before, g_recl_after,
           g_recl_after - g_recl_before, (int)g_term_rc);
    printf("STATS tcp active=%u estab=%u rstrcvd=%u rexmit=%u twreclaim=%u\n",
           sts_value("NSFTCP", "activeopen"),  sts_value("NSFTCP", "established"),
           sts_value("NSFTCP", "resetrcvd"),   sts_value("NSFTCP", "rexmit"),
           sts_value("NSFTCP", "twreclaim"));

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "INITAPI round-tripped");
    CHECK_EQ((long)g_term_rc, (long)NSF_RETOK, "TERMAPI round-tripped");
#ifdef NSFCTCI_CUU
    if (g_have_device) {
        /* The gate: every active connect->close cycle succeeded (no EMFILE wall),
         * more cycles than the pool ran, and reclaim fired. */
        CHECK_EQ((long)g_sock_fail, -1L, "no active SOCKET walled at EMFILE (reclaim kept up)");
        CHECK_EQ((long)g_cycles_run, (long)TWCYCLES, "all cycles ran");
        CHECK_EQ((long)g_connected, (long)TWCYCLES, "every active connect->close succeeded");
        CHECK(TWCYCLES > (int)TWPOOL, "cycles exceed the pool (pool pressure)");
        CHECK(g_recl_after > g_recl_before, "TIME_WAIT reclaim fired under pool pressure");
    }
#endif
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
    return mbt_test_summary("TSTTCPW");
}
