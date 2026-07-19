/*
 * tstezat.c -- NSFEZA TCP on-MVS live gate: the M4-5 EZASOKET verb set over the
 * full stack (M4-5, ADR-0035). host = false. The TCP sibling of tstezam: the
 * @@NS* C API (socket/bind/listen/accept/connect/recv/send/getpeername/select/
 * ioctl) over CTCI + IP + TCP + the M3-2 request path, driven by a cthread APP
 * SUBTASK.
 *
 *   main task (EXECUTIVE): pools, request seam, CTCI device, IP, TCP (demux +
 *     PROTOPS + SELECT engine), nsfeza_init, ATTACH the app subtask, arm the
 *     heartbeat, run evt_mainloop.
 *   app subtask (a real other TCB): the M4 verbs, below.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like tstezam. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 *
 * DETERMINISTIC proofs (hard CHECK, no host needed):
 *   - INITAPI / SOCKET(STREAM) / BIND / LISTEN succeed;
 *   - FIONBIO: a non-blocking ACCEPT on an empty listener -> EWOULDBLOCK;
 *   - SELECT with a 2 s timeout on the idle listener -> RETCODE 0 (this exercises
 *     the SELECT engine + the executive TIMER path LIVE -- the issue-#40 cadence).
 *
 * HOST-COORDINATED proofs (reported + bounded, driven from mvsdev per the runbook;
 * verify from the spool + `tcpdump -ni tun0`):
 *   - SELECT-for-read reports a pending connection, then ACCEPT hands back a new
 *     socket + the peer, GETPEERNAME agrees, one line is RECV'd and echoed (SEND),
 *     the connection closes:   `nc 192.168.200.1 3007`  (type a line, ^D/^C).
 *   - a NON-blocking CONNECT to a host listener completes via SELECT-for-write:
 *     `nc -l 192.168.200.2 3008` before the run.
 * The job always terminates (every host-dependent wait is bounded); a missing
 * host peer degrades to a reported "not observed", never a hang or a failed CHECK.
 */
#include "nsfeza.h"
#include "nsftcp.h"
#include "nsfsel.h"
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

#define TCP_LISTEN_PORT  3007u      /* host `nc 192.168.200.1 3007`           */
#define TCP_CONNECT_PORT 3008u      /* host `nc -l 192.168.200.2 3008`        */

/* ---- app subtask results (main task reports + asserts) --------------------- */
static INT  g_ini_rc, g_sock, g_bind_rc, g_listen_rc, g_maxsno;
static INT  g_nbacc_rc, g_nbacc_errno;      /* FIONBIO non-blocking accept      */
static INT  g_sel_tmo_rc;                   /* SELECT timeout -> 0              */
static INT  g_sel_evt_rc, g_accept_ok, g_recv_n;
static UINT g_peer_addr, g_peer_port, g_gp_addr;
static INT  g_conn_rc, g_conn_started;      /* non-blocking active connect      */
static INT  g_term_rc;
static char g_recv_buf[128];
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

/* A single-fullword SELECT mask holding socket number `n` (< 32). On the
 * big-endian target a UINT `1u<<n` in memory matches the facade's right-to-left
 * byte-wise mask read exactly (byte 3 = the LSB end); tstezat is target-only. */
static UINT mask_of(INT n)
{
    return (n >= 0 && n < 32) ? (1u << (UINT)n) : 0u;
}

static int app_sub(void *arg)
{
    NSF_SOCKADDR_IN local, peer, gp, dst;
    NSFECB napt = 0u;
    UINT   rmask, wmask;
    INT    namelen, rc, conn, cs, on = 1, off = 0;
    int    i;

    (void)arg;

    g_maxsno = -1;
    g_ini_rc    = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTEZAT ", &g_maxsno);
    g_sock      = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    mk_sa(&local, (UINT)NSFCTCI_SRC, (USHORT)TCP_LISTEN_PORT);
    g_bind_rc   = nsf_bind(g_sock, &local, (INT)sizeof(local));
    g_listen_rc = nsf_listen(g_sock, 5);

    /* FIONBIO: a non-blocking ACCEPT on the (empty) listener -> EWOULDBLOCK. */
    (void)nsf_ioctl(g_sock, (INT)NSF_FIONBIO, &on);
    g_nbacc_rc    = nsf_accept(g_sock, &peer, &namelen);
    g_nbacc_errno = nsf_lasterrno();
    (void)nsf_ioctl(g_sock, (INT)NSF_FIONBIO, &off);        /* back to blocking  */

    /* SELECT with a 2 s timeout on a FRESH IDLE socket -> RETCODE 0 (proves the
     * SELECT engine + the executive TIMER path live -- the #40 cadence). NOT on the
     * listener: a host connection arriving in the 2 s window would make the listener
     * read-ready and SELECT would correctly return 1, which is not what this check
     * asserts. A just-created socket (never bound/listened/connected) can never
     * become ready, so the timeout is deterministic regardless of the live host. */
    {
        INT idle_s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
        rmask = mask_of(idle_s);
        g_sel_tmo_rc = nsf_select(idle_s + 1, &rmask, NULL, NULL, 2, 0);
        (void)nsf_close(idle_s);
    }

    /* SELECT-for-read live event: poll ~30 s for a host connection; on ready,
     * ACCEPT + GETPEERNAME + RECV one line + echo it. Bounded so the job ends. */
    g_sel_evt_rc = 0; g_accept_ok = 0; g_recv_n = -1;
    for (i = 0; g_have_device && i < 15; i++) {
        rmask = mask_of(g_sock);
        rc = nsf_select(g_sock + 1, &rmask, NULL, NULL, 2, 0);
        if (rc > 0 && (rmask & mask_of(g_sock)) != 0u) {
            g_sel_evt_rc = rc;
            namelen = 0;
            conn = nsf_accept(g_sock, &peer, &namelen);
            if (conn >= 0) {
                g_accept_ok = 1;
                g_peer_addr = sa_addr(&peer);
                g_peer_port = (UINT)sa_port(&peer);
                namelen = 0;
                if (nsf_getpeername(conn, &gp, &namelen) == NSF_RETOK) {
                    g_gp_addr = sa_addr(&gp);
                }
                g_recv_n = nsf_recv(conn, g_recv_buf, (INT)sizeof(g_recv_buf), 0);
                if (g_recv_n > 0) {
                    (void)nsf_send(conn, g_recv_buf, g_recv_n, 0);   /* echo     */
                }
                (void)nsf_close(conn);
            }
            break;
        }
    }

    /* NON-blocking active CONNECT + SELECT-for-write to a host `nc -l ... 3008`.
     * Non-blocking so a missing listener never blocks the job for the RTO series. */
    g_conn_rc = NSF_RETERR; g_conn_started = 0;
    cs = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    if (cs >= 0) {
        (void)nsf_ioctl(cs, (INT)NSF_FIONBIO, &on);
        mk_sa(&dst, (UINT)NSFCTCI_DST, (USHORT)TCP_CONNECT_PORT);
        rc = nsf_connect(cs, &dst, (INT)sizeof(dst));      /* -> EINPROGRESS     */
        g_conn_started = (rc == NSF_RETERR &&
                          nsf_lasterrno() == NSF_EINPROGRESS) ? 1 : 0;
        for (i = 0; g_have_device && i < 5; i++) {         /* ~10 s for write    */
            wmask = mask_of(cs);
            rc = nsf_select(cs + 1, NULL, &wmask, NULL, 2, 0);
            if (rc > 0 && (wmask & mask_of(cs)) != 0u) {
                g_conn_rc = NSF_RETOK;                     /* connect completed  */
                break;
            }
        }
        (void)nsf_close(cs);
    }

    (void)nsf_close(g_sock);
    g_term_rc = nsf_termapi();
    (void)napt;

    nsfevt_stop();
    return 0;
}

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

    printf("=== nsf370 NSFEZA TCP MVS live gate (M4-5 verb set + full stack) ===\n");

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
    nsfsel_init();
    nsfeza_init();
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
    printf("--- device-free skeleton (no NSFCTCI_CUU): links + deterministic verbs ---\n");
    nsfip_local_add((UINT)NSFCTCI_SRC);
#endif

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);
    evt_mainloop();
    CHECK_EQ((long)nsfthr_join(t, 90u), 0, "app subtask joined");

    /* -- report ----------------------------------------------------------- */
    printf("INITAPI rc=%d maxsno=%d  SOCKET fd=%d  BIND rc=%d  LISTEN rc=%d\n",
           (int)g_ini_rc, (int)g_maxsno, (int)g_sock, (int)g_bind_rc, (int)g_listen_rc);
    printf("FIONBIO accept rc=%d errno=%d  SELECT-timeout rc=%d\n",
           (int)g_nbacc_rc, (int)g_nbacc_errno, (int)g_sel_tmo_rc);
    printf("SELECT-event rc=%d  ACCEPT ok=%d peer=%u.%u.%u.%u:%u  GETPEER=%u.%u.%u.%u  RECV n=%d\n",
           (int)g_sel_evt_rc, (int)g_accept_ok,
           (unsigned)((g_peer_addr >> 24) & 0xFFu), (unsigned)((g_peer_addr >> 16) & 0xFFu),
           (unsigned)((g_peer_addr >> 8) & 0xFFu),  (unsigned)(g_peer_addr & 0xFFu),
           (unsigned)g_peer_port,
           (unsigned)((g_gp_addr >> 24) & 0xFFu), (unsigned)((g_gp_addr >> 16) & 0xFFu),
           (unsigned)((g_gp_addr >> 8) & 0xFFu),  (unsigned)(g_gp_addr & 0xFFu),
           (int)g_recv_n);
    printf("CONNECT started(EINPROGRESS)=%d completed=%d  TERMAPI rc=%d\n",
           (int)g_conn_started, (int)(g_conn_rc == NSF_RETOK), (int)g_term_rc);

    /* -- deterministic assertions (host-independent) ---------------------- */
    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "nsf_initapi ok");
    CHECK_EQ((long)g_maxsno, (long)(NSFEZA_MAXSOC - 1), "nsf_initapi returned MAXSNO");
    CHECK_EQ((long)g_sock, 0L, "nsf_socket returned number 0 (0-based)");
    CHECK_EQ((long)g_bind_rc, (long)NSF_RETOK, "nsf_bind ok");
    CHECK_EQ((long)g_listen_rc, (long)NSF_RETOK, "nsf_listen ok");
    CHECK_EQ((long)g_nbacc_rc, (long)NSF_RETERR, "FIONBIO accept -> -1");
    CHECK_EQ((long)g_nbacc_errno, (long)NSF_EWOULDBLOCK, "FIONBIO accept -> EWOULDBLOCK");
    CHECK_EQ((long)g_sel_tmo_rc, 0L, "SELECT 2 s timeout on idle listener -> RETCODE 0");
    CHECK_EQ((long)g_term_rc, (long)NSF_RETOK, "nsf_termapi ok");
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
    return mbt_test_summary("TSTEZAT");
}
