/*
 * tstezam.c -- NSFEZA on-MVS live gate: the EZASOKET C API over the full stack
 * (M3-4). host = false. TSTUDPM's live scenario rewritten against the @@NS* C
 * API (nsf_socket/bind/sendto/recvfrom/close/termapi) instead of hand-built
 * NSFRQEs -- so this proves the NSFEZA core end to end over CTCI + IP + UDP +
 * the M3-2 request path, driven by a cthread APP SUBTASK (the TSTREQM pattern).
 * The EZASOH03 asm veneer is proved separately by TSTEZAH; here the app calls
 * the C API directly, exactly as a relinked C application would.
 *
 *   main task (EXECUTIVE): pools, request seam, CTCI device, IP, UDP (demux +
 *     PROTOPS), nsfeza_init, ATTACH the app subtask, arm the heartbeat, run
 *     evt_mainloop (drives the CTCI subtasks incl. the ADR-0027 IOHALT read-park
 *     for the locally-originated SENDTO, drains + dispatches requests).
 *   app subtask (over ecb_post/ecb_wait, a real other TCB): nsf_initapi ->
 *     nsf_socket -> nsf_bind(7777) -> nsf_sendto to the host peer -> a bounded
 *     NSF_MSG_DONTWAIT nsf_recvfrom poll -> nsf_close -> nsf_termapi -> stop.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like TSTUDPM. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 *
 * The three live scenarios (read build/test-runner.spool + host tcpdump):
 *   1. RECEIVE  : host `echo -n hi | nc -u 192.168.200.1 7777` -> recvfrom
 *                 returns "hi" + the peer sockaddr (addr/port).
 *   2. LOCAL SEND: nsf_sendto reaches the wire promptly on an idle link (host
 *                 tcpdump); the IOHALT read-park, ADR-0027 (watch #28).
 *   3. PORT UNREACH: host UDP to an unbound port -> ICMP port unreachable in
 *                 tcpdump (handled by udp_input, no app involvement).
 */
#include "nsfeza.h"
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

#ifndef NSFCTCI_SRC
#define NSFCTCI_SRC  0xC0A8C801u    /* 192.168.200.1 (guest HOME)             */
#endif
#ifndef NSFCTCI_DST
#define NSFCTCI_DST  0xC0A8C802u    /* 192.168.200.2 (host peer)              */
#endif
#ifndef NSFCTCI_MTU
#define NSFCTCI_MTU  1500u
#endif

#define UDP_LOCAL_PORT  7777u
#define UDP_HOST_PORT   9u

/* ---- app subtask (producer) ------------------------------------------------ */
static INT  g_ini_rc, g_sock, g_bind_rc, g_send_rc;
static INT  g_recv_rc, g_recv_ok, g_term_rc, g_maxsno;
static UINT g_peer_addr, g_peer_port;
static char g_recv_buf[64];
static volatile int g_have_device;

static void mk_sa(NSF_SOCKADDR_IN *sa, UINT addr, USHORT port)
{
    UCHAR *a = (UCHAR *)&sa->sin_addr;
    UCHAR *p = (UCHAR *)&sa->sin_port;

    memset(sa, 0, sizeof(*sa));
    sa->sin_family = (USHORT)NSF_AF_INET;
    a[0] = (UCHAR)(addr >> 24); a[1] = (UCHAR)(addr >> 16);
    a[2] = (UCHAR)(addr >> 8);  a[3] = (UCHAR)addr;
    p[0] = (UCHAR)(port >> 8);  p[1] = (UCHAR)port;
}

static UINT sa_addr(const NSF_SOCKADDR_IN *sa)
{
    const UCHAR *a = (const UCHAR *)&sa->sin_addr;
    return ((UINT)a[0] << 24) | ((UINT)a[1] << 16) | ((UINT)a[2] << 8) | (UINT)a[3];
}
static USHORT sa_port(const NSF_SOCKADDR_IN *sa)
{
    const UCHAR *p = (const UCHAR *)&sa->sin_port;
    return (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

static int app_sub(void *arg)
{
    NSF_SOCKADDR_IN local, dst, peer;
    NSFECB naptmr;
    INT    namelen;
    int    i;
    static const UCHAR payload[8] =
        { 0x55u, 0x44u, 0x50u, 0x2Du, 0x68u, 0x69u, 0x21u, 0x0Au };  /* "UDP-hi!\n" */

    (void)arg;

    g_maxsno = -1;
    g_ini_rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTEZAM ", &g_maxsno);

    g_sock = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);

    mk_sa(&local, (UINT)NSFCTCI_SRC, UDP_LOCAL_PORT);
    g_bind_rc = nsf_bind(g_sock, &local, (INT)sizeof(local));

    /* Scenario 2: locally-originated SENDTO to the host peer (the IOHALT
     * read-park in production, #28). Assert only the byte count; wire = tcpdump. */
    mk_sa(&dst, (UINT)NSFCTCI_DST, UDP_HOST_PORT);
    g_send_rc = nsf_sendto(g_sock, payload, (INT)sizeof(payload), 0,
                           &dst, (INT)sizeof(dst));

    /* Scenario 1: bounded non-blocking recvfrom poll (~20 s) so the job always
     * terminates; a host `nc -u 192.168.200.1 7777` completes one. */
    naptmr    = 0u;
    g_recv_rc = NSF_RETERR;
    g_recv_ok = 0;
    for (i = 0; g_have_device && i < 40; i++) {
        namelen = 0;
        g_recv_rc = nsf_recvfrom(g_sock, g_recv_buf, (INT)sizeof(g_recv_buf),
                                 NSF_MSG_DONTWAIT, &peer, &namelen);
        if (g_recv_rc >= 0) {
            g_peer_addr = sa_addr(&peer);
            g_peer_port = (UINT)sa_port(&peer);
            g_recv_ok   = 1;
            break;
        }
        nsfthr_timed_wait(&naptmr, 5u);
    }

    (void)nsf_close(g_sock);
    g_term_rc = nsf_termapi();

    nsfevt_stop();
    return 0;
}

/* EV_PACKET_RECEIVED terminus: hand each received packet to the IP layer. */
static void rx_packet(EVT *ev)
{
    PBUF   *b   = (PBUF *)ev->p1;
    NETDEV *dev = dev_by_index(ev->u1);
    if (b != NULL) {
        nsfip_input(dev, b);
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

    printf("=== nsf370 NSFEZA MVS live gate (EZASOKET C API + full stack) ===\n");

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
    nsfeza_init();
    nsfip_init();
    nsficmp_init();
    nsfudp_init();
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
        CHECK_EQ((long)nsfip_route_add(0u, 0u, dev, 0u), 0L, "default route -> CTCI");
        g_have_device = 1;
    }
#else
    (void)cfg;
    printf("--- device-free skeleton (no NSFCTCI_CUU): links + SENDTO no-route ---\n");
    nsfip_local_add((UINT)NSFCTCI_SRC);
#endif

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);
    evt_mainloop();

    CHECK_EQ((long)nsfthr_join(t, 60u), 0, "app subtask joined");

    printf("INITAPI rc=%d maxsno=%d  SOCKET fd=%d  BIND rc=%d  SENDTO rc=%d\n",
           (int)g_ini_rc, (int)g_maxsno, (int)g_sock, (int)g_bind_rc, (int)g_send_rc);
    printf("RECVFROM got=%d rc=%d peer=%u.%u.%u.%u:%u  TERMAPI rc=%d\n",
           (int)g_recv_ok, (int)g_recv_rc,
           (unsigned)((g_peer_addr >> 24) & 0xFFu), (unsigned)((g_peer_addr >> 16) & 0xFFu),
           (unsigned)((g_peer_addr >> 8) & 0xFFu),  (unsigned)(g_peer_addr & 0xFFu),
           (unsigned)g_peer_port, (int)g_term_rc);

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "nsf_initapi ok");
    CHECK_EQ((long)g_maxsno, (long)(NSFEZA_MAXSOC - 1), "nsf_initapi returned MAXSNO");
    CHECK_EQ((long)g_sock, 0L, "nsf_socket returned number 0 (0-based)");
    CHECK_EQ((long)g_bind_rc, (long)NSF_RETOK, "nsf_bind ok");
#ifdef NSFCTCI_CUU
    CHECK_EQ((long)g_send_rc, 8L, "nsf_sendto returned the payload byte count (8)");
#endif
    CHECK_EQ((long)g_term_rc, (long)NSF_RETOK, "nsf_termapi ok");
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
    return mbt_test_summary("TSTEZAM");
}
