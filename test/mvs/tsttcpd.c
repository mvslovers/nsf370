/*
 * tsttcpd.c -- NSFTCP on-MVS live gate for the M4-3 DATA PATH over the real
 * 0500/0501 CTCI pair. host = false. The full stack (CTCI + IP + TCP + the
 * request path) driven by a cthread APP SUBTASK (the tsttcph pattern). TWO
 * scenarios, deliberately kept separate so a hang is diagnosable:
 *
 *   Connection #1 -- SMALL ECHO (bidirectional). Receive a short line and send
 *     it straight back. Binary-transparent (NSF never converts payload, spec
 *     15.3), so an ASCII `nc` sees its own bytes echoed -> recv + send proven
 *     byte-exact on the wire. A single small locally-originated write (the
 *     well-worn nsfecho / ADR-0030 path).
 *
 *   Connection #2 -- LARGE ONE-WAY DRAIN. Receive a transfer LARGER than the
 *     4096 receive window (e.g. 16 KB) in 1 KB slices until EOF, counting bytes +
 *     a position-sensitive checksum, and send NOTHING back. This is the scenario
 *     that DEADLOCKS if the window-update-on-drain rule (ADR-0032) is wrong: each
 *     drained slice reopens the advertised window from 0, and the pure
 *     window-update ACK is what lets the peer send the next slice. Keeping it
 *     one-way (no bulk echo) isolates the drain rule from the CTCI write path, so
 *     "did it deadlock?" has ONE cause. The total + checksum go to the job spool
 *     (read on the host), not back over TCP -- a guest-generated summary would be
 *     EBCDIC on an ASCII peer.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like TSTTCPH. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 * Without it, a device-free skeleton that only proves the machinery links.
 *
 * Live procedure (host mvsdev, `tcpdump -ni tun0 tcp` up FIRST):
 *   1. ECHO  : `printf 'hello nsf 12345\n' | nc -N 192.168.200.1 3010` -> the
 *              same line echoes back on stdout; tcpdump shows data both ways +
 *              a clean 4-way FIN.  (`-N` half-closes on EOF so the guest sees it.)
 *   2. DRAIN : `head -c 16384 /dev/urandom >/tmp/p; nc -N 192.168.200.1 3010
 *              </tmp/p` -> no stdout (one-way); tcpdump shows the guest window
 *              dropping to `win 0` and a pure ACK with a reopened window after
 *              each drain-from-zero. The job spool prints `DRAIN n=16384 sum=...`.
 *   Watch tcpdump; be ready to `P NSF` if a write never leaves the wire.
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

#define ECHO_PORT   3010u           /* host `nc -N 192.168.200.1 3010`        */
#define SLICE       1024u           /* receive a window in small slices        */

/* ---- the app subtask (echo #1, drain #2) ----------------------------------- */
static INT  g_ini_rc, g_lsock_rc, g_bind_rc, g_listen_rc, g_term_rc;
static int  g_conns;                /* connections handled                     */
static UINT g_apptok, g_ldesc;
static UINT g_echo_recv, g_echo_sent, g_drain_recv, g_drain_sum;
static INT  g_echo_eof = 1, g_drain_eof = 1;
static volatile int g_have_device;

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
}

/* SMALL ECHO: recv a slice, send it straight back, until EOF (binary-transparent
 * -> byte-exact for an ASCII peer). */
static void handle_echo(UINT child)
{
    NSFRQE rr, rw;
    UCHAR  buf[SLICE];

    for (;;) {
        rqe_init(&rr, RQ_RECV, child);
        rr.ubuf = buf; rr.ulen = (UINT)sizeof(buf);
        nsfreq_call(&rr);
        g_echo_eof = rr.retcode;
        if (rr.retcode <= 0) {
            break;                          /* 0 = EOF (peer FIN), <0 = error   */
        }
        g_echo_recv += (UINT)rr.retcode;

        rqe_init(&rw, RQ_SEND, child);
        rw.ubuf = buf; rw.ulen = (UINT)rr.retcode;
        nsfreq_call(&rw);
        if (rw.retcode >= 0) {
            g_echo_sent += (UINT)rw.retcode;
        }
    }
}

/* LARGE ONE-WAY DRAIN: recv 1 KB slices to EOF, counting bytes + a position-
 * sensitive checksum; send nothing back (isolates the window-update rule). */
static void handle_drain(UINT child)
{
    NSFRQE rr;
    UCHAR  buf[SLICE];
    UINT   sum = 0u;
    INT    j;

    for (;;) {
        rqe_init(&rr, RQ_RECV, child);
        rr.ubuf = buf; rr.ulen = (UINT)sizeof(buf);
        nsfreq_call(&rr);
        g_drain_eof = rr.retcode;
        if (rr.retcode <= 0) {
            break;
        }
        for (j = 0; j < rr.retcode; j++) {
            sum = (sum * 31u) + (UINT)buf[j];   /* order-sensitive               */
        }
        g_drain_recv += (UINT)rr.retcode;
    }
    g_drain_sum = sum;
}

static int app_sub(void *arg)
{
    NSFRQE ri, rs, rb, rl, ra, rcl, rt;
    NSFECB naptmr;                          /* dummy: a never-posted sleep ECB  */
    int    conn, i;

    (void)arg;
    naptmr = 0u;

    rqe_init(&ri, RQ_INITAPI, 0u);
    nsfreq_call(&ri);
    g_ini_rc = ri.retcode;
    g_apptok = ri.apptok;

    rqe_init(&rs, RQ_SOCKET, 0u);
    rs.apptok = g_apptok;
    rs.p1 = NSF_AF_INET; rs.p2 = NSF_SOCK_STREAM; rs.p3 = 6u;
    nsfreq_call(&rs);
    g_lsock_rc = rs.retcode;
    g_ldesc    = (UINT)rs.retcode;

    rqe_init(&rb, RQ_BIND, g_ldesc);
    rb.p1 = (UINT)NSFCTCI_SRC; rb.p2 = ECHO_PORT;
    nsfreq_call(&rb);
    g_bind_rc = rb.retcode;

    rqe_init(&rl, RQ_LISTEN, g_ldesc);
    rl.p1 = 4u;
    nsfreq_call(&rl);
    g_listen_rc = rl.retcode;

    /* Handle connections in order: #1 echo, #2+ drain. Each accept polls
     * non-blocking for ~30 s; no further connection ends the run. */
    for (conn = 0; g_have_device && conn < 4; conn++) {
        UINT child = 0u;
        int  got   = 0;

        for (i = 0; i < 180; i++) {             /* ~90 s window per connection    */
            rqe_init(&ra, RQ_ACCEPT, g_ldesc);
            ra.flags = (USHORT)RQ_F_NONBLOCK;
            nsfreq_call(&ra);
            if (ra.retcode >= 0) {
                child = (UINT)ra.retcode;
                got   = 1;
                break;
            }
            nsfthr_timed_wait(&naptmr, 5u);     /* ~500 ms nap on our OWN TCB     */
        }
        if (!got) {
            break;                              /* no more connections           */
        }
        g_conns++;
        if (conn == 0) {
            handle_echo(child);
        } else {
            handle_drain(child);
        }
        rqe_init(&rcl, RQ_CLOSE, child);        /* our FIN -> clean 4-way close   */
        nsfreq_call(&rcl);
    }

    rqe_init(&rcl, RQ_CLOSE, g_ldesc);          /* close the listener            */
    nsfreq_call(&rcl);

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

    printf("=== nsf370 NSFTCP MVS live gate (CTCI + IP + TCP data path) ===\n");

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
    printf("--- device-free skeleton (no NSFCTCI_CUU): links only ---\n");
    nsfip_local_add((UINT)NSFCTCI_SRC);
#endif

    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");
    t = nsfthr_create(app_sub, NULL);
    CHECK(t != NULL, "app subtask ATTACHed");

    nsftmr_plat_arm(1u);                    /* ~100 ms heartbeat -> stop liveness */
    evt_mainloop();

    CHECK_EQ((long)nsfthr_join(t, 90u), 0, "app subtask joined");

    printf("INITAPI rc=%d tok=%08X | LSOCK rc=%d BIND rc=%d LISTEN rc=%d | conns=%d\n",
           (int)g_ini_rc, (unsigned)g_apptok, (int)g_lsock_rc, (int)g_bind_rc,
           (int)g_listen_rc, g_conns);
    printf("ECHO  recv=%u sent=%u eof_rc=%d\n",
           (unsigned)g_echo_recv, (unsigned)g_echo_sent, (int)g_echo_eof);
    printf("DRAIN n=%u sum=%08X eof_rc=%d\n",
           (unsigned)g_drain_recv, (unsigned)g_drain_sum, (int)g_drain_eof);
    printf("STATS tcp estab=%u rstsent=%u rstrcvd=%u oooseg=%u dupack=%u rxfull=%u datadrop=%u\n",
           sts_value("NSFTCP", "established"), sts_value("NSFTCP", "resetsent"),
           sts_value("NSFTCP", "resetrcvd"),  sts_value("NSFTCP", "oooseg"),
           sts_value("NSFTCP", "dupack"),     sts_value("NSFTCP", "rxfull"),
           sts_value("NSFTCP", "datadrop"));

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "INITAPI round-tripped");
    CHECK(g_lsock_rc >= 0, "listen SOCKET returned a descriptor");
    CHECK_EQ((long)g_bind_rc, (long)NSF_RETOK, "BIND ok");
    CHECK_EQ((long)g_listen_rc, (long)NSF_RETOK, "LISTEN ok");
    if (g_conns >= 1) {
        CHECK_EQ((long)g_echo_eof, 0L, "echo #1 ended on a clean EOF (peer FIN)");
        CHECK_EQ((long)g_echo_sent, (long)g_echo_recv, "echo #1: every byte echoed back");
        CHECK(g_echo_recv > 0u, "echo #1: at least one byte received");
    }
    if (g_conns >= 2) {
        CHECK_EQ((long)g_drain_eof, 0L, "drain #2 ended on a clean EOF");
        CHECK(g_drain_recv > (UINT)NSFTCP_RCVWND_DEFAULT,
              "drain #2: a transfer LARGER than the window completed (window-update rule)");
    }
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
    return mbt_test_summary("TSTTCPD");
}
