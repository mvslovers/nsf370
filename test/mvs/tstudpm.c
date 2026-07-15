/*
 * tstudpm.c -- NSFUDP on-MVS live gate: the full stack end to end (M3-3).
 * host = false. This is where UDP becomes real: CTCI (a live 3088 pair) + IP +
 * ICMP + UDP + the M3-2 request path, driven by a cthread APP SUBTASK (the
 * TSTREQM pattern) over the real wire. No EZASOKET yet (M3-4) -- the subtask
 * builds NSFRQEs directly.
 *
 *   main task (EXECUTIVE): build the pools, wire the request seam + the CTCI
 *     device + the IP layer, register UDP (demux + PROTOPS), ATTACH the app
 *     subtask, arm the ADR-0017 heartbeat, run evt_mainloop. The loop drives the
 *     CTCI subtasks (io->service / io->kick, incl. the ADR-0027 IOHALT read-park
 *     for the locally-originated SENDTO) and drains + dispatches requests.
 *   app subtask (over ecb_post/ecb_wait, SVC 2/1, a real other TCB): INITAPI ->
 *     SOCKET -> BIND(7777) -> SENDTO to the host peer (the LOCALLY-ORIGINATED
 *     send: the first production exercise of the IOHALT read-park -- watch #28)
 *     -> a bounded non-blocking RECVFROM poll (so the job always terminates;
 *     a host `nc -u <guest> 7777` during the window completes it) -> CLOSE ->
 *     TERMAPI -> nsfevt_stop.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like TSTCTCM part 2. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 * Without it, this is a device-free skeleton that only proves the machinery
 * links + the SENDTO fails cleanly with no route (no CTCI up).
 *
 * The three live scenarios (read build/test-runner.spool + host tcpdump):
 *   1. RECEIVE  : host `echo -n hi | nc -u 192.168.200.1 7777` -> RECVFROM
 *                 completes with "hi" + the peer addr/port.
 *   2. LOCAL SEND: the SENDTO reaches the wire PROMPTLY on an idle link (host
 *                 tcpdump), rpurge +1 / ierr +0 (the IOHALT path, ADR-0027).
 *   3. PORT UNREACH: host UDP to an unbound guest port -> ICMP port unreachable
 *                 in tcpdump, NSFUDP noport +1 / NSFICM errsent +1 (handled by
 *                 udp_input with no app involvement).
 */
#include "nsfudp.h"
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

/* Guest/host IP addresses. Defaults match the live MVSCE CTCI pair on mvsdev
 * (CLAUDE.md §5): guest HOME 192.168.200.1, host TUN 192.168.200.2. */
#ifndef NSFCTCI_SRC
#define NSFCTCI_SRC  0xC0A8C801u    /* 192.168.200.1 (guest HOME)             */
#endif
#ifndef NSFCTCI_DST
#define NSFCTCI_DST  0xC0A8C802u    /* 192.168.200.2 (host peer)              */
#endif
#ifndef NSFCTCI_MTU
#define NSFCTCI_MTU  1500u
#endif

#define UDP_LOCAL_PORT  7777u       /* the bound receive port                 */
#define UDP_HOST_PORT   9u          /* an arbitrary host port for the SENDTO  */

/* ---- the app subtask (producer) -------------------------------------------- */
static INT  g_ini_rc, g_sock_rc, g_bind_rc, g_send_rc;
static INT  g_recv_rc, g_recv_from_ok, g_term_rc;
static UINT g_apptok, g_desc, g_peer_addr, g_peer_port;
static char g_recv_buf[64];
static volatile int g_have_device;      /* a live CTCI device came up (drive I/O) */

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
}

static int app_sub(void *arg)
{
    NSFRQE ri, rs, rb, rsnd, rc, rt;
    NSFECB naptmr;                          /* dummy: a never-posted sleep ECB  */
    int    i;
    static const UCHAR payload[8] =
        { 0x55u, 0x44u, 0x50u, 0x2Du, 0x68u, 0x69u, 0x21u, 0x0Au };  /* "UDP-hi!\n" */

    (void)arg;

    rqe_init(&ri, RQ_INITAPI, 0u);
    nsfreq_call(&ri);
    g_ini_rc = ri.retcode;
    g_apptok = ri.apptok;

    rqe_init(&rs, RQ_SOCKET, 0u);
    rs.apptok = g_apptok;
    rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_DGRAM; rs.p3 = 17u;
    nsfreq_call(&rs);
    g_sock_rc = rs.retcode;
    g_desc    = (UINT)rs.retcode;

    rqe_init(&rb, RQ_BIND, g_desc);
    rb.p1 = (UINT)NSFCTCI_SRC; rb.p2 = UDP_LOCAL_PORT;
    nsfreq_call(&rb);
    g_bind_rc = rb.retcode;

    /* Scenario 2: locally-originated SENDTO to the host peer. On an inbound-idle
     * link this exercises the ADR-0027 IOHALT read-park in production (issue
     * #28). Assert only that the stack accepted it (byte count) -- the wire proof
     * is host tcpdump. */
    rqe_init(&rsnd, RQ_SENDTO, g_desc);
    rsnd.ubuf = (void *)payload; rsnd.ulen = (UINT)sizeof(payload);
    rsnd.p1 = (UINT)NSFCTCI_DST; rsnd.p2 = UDP_HOST_PORT;
    nsfreq_call(&rsnd);
    g_send_rc = rsnd.retcode;

    /* Scenario 1: bounded non-blocking RECVFROM poll -- so the job ALWAYS
     * terminates (~20 s). A host `nc -u 192.168.200.1 7777` during the window
     * completes one. The subtask sleeps between tries on a never-posted ECB
     * (nsfthr_timed_wait on its OWN TCB -- not the executive's, so the heartbeat
     * STIMER is undisturbed, per ADR-0025). */
    naptmr = 0u;
    g_recv_rc = NSF_RETERR;
    g_recv_from_ok = 0;
    for (i = 0; g_have_device && i < 40; i++) {   /* skip the ~20s poll if no dev */
        rqe_init(&rc, RQ_RECVFROM, g_desc);
        rc.flags = (USHORT)RQ_F_NONBLOCK;
        rc.ubuf  = g_recv_buf; rc.ulen = (UINT)sizeof(g_recv_buf);
        nsfreq_call(&rc);
        if (rc.retcode >= 0) {              /* a datagram arrived               */
            g_recv_rc      = rc.retcode;
            g_peer_addr    = rc.p1;
            g_peer_port    = rc.p2;
            g_recv_from_ok = 1;
            break;
        }
        nsfthr_timed_wait(&naptmr, 5u);     /* ~500 ms nap, then retry          */
    }

    rqe_init(&rc, RQ_CLOSE, g_desc);
    nsfreq_call(&rc);

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

    printf("=== nsf370 NSFUDP MVS live gate (CTCI + IP + UDP + request path) ===\n");

    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) { printf("nsfevt_init failed\n"); return 1; }
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    CHECK(nsfudp_reserve(0) == 0, "nsfudp_reserve (UDPPCB pool)");
    CHECK(ctci_reserve(1u, CTCI_BUF_DEFAULT) == 0, "ctci_reserve");
    mm_init_complete();

    soc_init();
    nsfreq_init();
    nsfip_init();
    nsficmp_init();
    nsfudp_init();                          /* registers the IP demux for 17 */
    CHECK_EQ((long)nsfreq_register_proto(17u, nsfudp_protops()), 0L,
             "register UDP protocol");
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
        /* default route out the CTCI link; on a point-to-point link the driver
         * writes to the peer regardless of the next hop (spec 11.4/11.6). */
        CHECK_EQ((long)nsfip_route_add(0u, 0u, dev, 0u), 0L, "default route -> CTCI");
        g_have_device = 1;              /* drive the SENDTO/RECVFROM live gate */
    }
#else
    (void)cfg;
    printf("--- device-free skeleton (no NSFCTCI_CUU): links + SENDTO no-route ---\n");
    nsfip_local_add((UINT)NSFCTCI_SRC);
#endif

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);                    /* ~100 ms heartbeat -> stop liveness */
    evt_mainloop();

    CHECK_EQ((long)nsfthr_join(t, 60u), 0, "app subtask joined");

    printf("INITAPI rc=%d tok=%08X  SOCKET rc=%d  BIND rc=%d  SENDTO rc=%d\n",
           (int)g_ini_rc, (unsigned)g_apptok, (int)g_sock_rc,
           (int)g_bind_rc, (int)g_send_rc);
    printf("RECVFROM got=%d rc=%d peer=%u.%u.%u.%u:%u  TERMAPI rc=%d\n",
           (int)g_recv_from_ok, (int)g_recv_rc,
           (unsigned)((g_peer_addr >> 24) & 0xFFu), (unsigned)((g_peer_addr >> 16) & 0xFFu),
           (unsigned)((g_peer_addr >> 8) & 0xFFu),  (unsigned)(g_peer_addr & 0xFFu),
           (unsigned)g_peer_port, (int)g_term_rc);
    printf("STATS udp in=%u out=%u noport=%u badcksum=%u | icmp errsent=%u\n",
           sts_value("NSFUDP", "in"), sts_value("NSFUDP", "out"),
           sts_value("NSFUDP", "noport"), sts_value("NSFUDP", "badcksum"),
           sts_value("NSFICM", "errsent"));

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "INITAPI round-tripped");
    CHECK(g_sock_rc >= 0, "SOCKET returned a descriptor");
    CHECK_EQ((long)g_bind_rc, (long)NSF_RETOK, "BIND ok");
#ifdef NSFCTCI_CUU
    CHECK_EQ((long)g_send_rc, 8L, "SENDTO returned the payload byte count (8)");
#endif
    CHECK_EQ((long)g_term_rc, (long)NSF_RETOK, "TERMAPI round-tripped");
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open (leak gate)");
#if NSF_DEBUG
    CHECK_EQ((long)nsfudp_debug_inuse(), 0L, "UDPPCB pool at baseline (leak gate)");
#endif

#ifdef NSFCTCI_CUU
    if (dev != NULL) {
        dev_shutdown(dev);
    }
#endif
    mm_shutdown();
    return mbt_test_summary("TSTUDPM");
}
