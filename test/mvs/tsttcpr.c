/*
 * tsttcpr.c -- NSFTCP on-MVS live gate for the M4-4 ZERO-WINDOW PERSIST path
 * over the real 0500/0501 CTCI pair. host = false. The full stack (CTCI + IP +
 * TCP + the request path) driven by a cthread APP SUBTASK (the tsttcpd/tsttcph
 * pattern). ONE scenario, chosen because it needs no root on the host:
 *
 *   GUEST STREAMS to a host `nc` that gets kill -STOP'd mid-transfer. The guest
 *   accepts a connection and sends a large stream (STREAM_TOTAL) in slices. When
 *   the operator `kill -STOP`s the receiving `nc`, the host kernel keeps ACKing
 *   into its receive buffer until it fills, then advertises `win 0`. The guest
 *   has nothing in flight (everything ACKed) + a zero send window + data still to
 *   send -> the PERSIST timer arms and probes one byte beyond the window at
 *   backing-off intervals (RFC 1122 §4.2.2.17). `kill -CONT` drains the buffer,
 *   the peer re-advertises a window, the buffered data flows, and the parked
 *   blocking SEND completes -- the transfer finishes.
 *
 * This is the LIVE proof that the persist path works against a real stack: the
 * window probes are visible in tcpdump at backing-off intervals, and `wndprobe`
 * is non-zero in the stats. It also re-proves the M4-4 timer wiring does not
 * disturb the lossless path: `rexmit` MUST stay 0 (a zero-window stall arms
 * persist, never rexmit -- nothing is in flight; and the un-stalled stream is
 * ACKed well inside the 1 s base RTO). Genuine-loss RTO firing cannot be induced
 * on the lossless CTCI link without root on mvsdev, so the RTO behaviour is
 * HOST-proven (test/tsttcp.c) -- this gate does not approximate it.
 *
 * Gated behind -DNSFCTCI_CUU (a live pair), like TSTTCPD. Build with
 * -DNSFCTCI_CUU=0x0500 -DNSFCTCI_SRC=0xC0A8C801u -DNSFCTCI_DST=0xC0A8C802u.
 * Without it, a device-free skeleton that only proves the machinery links.
 *
 * Live procedure (host mvsdev, `tcpdump -ni tun0 tcp` up FIRST):
 *   1. `nc -N 192.168.200.1 3020 >/dev/null &`  -- connect + discard the stream.
 *   2. Within ~1 s (before the whole stream drains) `kill -STOP %1` (or the nc
 *      PID). tcpdump now shows the guest's window probes -- single-byte segments
 *      at 1 s, 2 s, 4 s, 8 s ... backing-off intervals, each drawing a `win 0`
 *      ACK from the STOP'd host.
 *   3. `kill -CONT %1` -- the host drains, re-advertises a window, and the
 *      transfer completes. The job spool prints `STREAM sent=... wndprobe=...`.
 *   Watch tcpdump; be ready to `P NSF` if a leftover STC holds the CTCI pair.
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

#define STREAM_PORT   3020u             /* host `nc -N 192.168.200.1 3020`    */
#define SLICE         1024u             /* app send slice                     */
#define STREAM_TOTAL  (4u * 1024u * 1024u)  /* 4 MB: exceeds the host buffer so
                                             * a mid-stream STOP stalls at win 0 */

/* ---- the app subtask (accept one connection, stream to it) ----------------- */
static INT  g_ini_rc, g_lsock_rc, g_bind_rc, g_listen_rc, g_term_rc;
static int  g_conns;
static UINT g_apptok, g_ldesc;
static UINT g_stream_sent;
static INT  g_last_send_rc;
static volatile int g_have_device;

static void rqe_init(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->eye, NSFRQE_EYE, 4);
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
}

/* Stream STREAM_TOTAL bytes in SLICE chunks. A blocking SEND parks when the send
 * budget fills behind a zero window (peer STOP'd) and completes when the window
 * reopens (peer CONT'd) -- the executive persist timer probes the window in the
 * meantime. Binary-transparent: the payload is an ignorable pattern (the host
 * discards it), so no EBCDIC/ASCII conversion matters. */
static void handle_stream(UINT child)
{
    static UCHAR buf[SLICE];
    UINT  i;

    for (i = 0u; i < SLICE; i++) {
        buf[i] = (UCHAR)(i & 0xFFu);
    }
    while (g_stream_sent < STREAM_TOTAL && g_have_device) {
        NSFRQE rw;
        UINT   want = STREAM_TOTAL - g_stream_sent;
        UINT   n    = (want < SLICE) ? want : SLICE;

        rqe_init(&rw, RQ_SEND, child);
        rw.ubuf = buf; rw.ulen = n;
        nsfreq_call(&rw);                       /* blocks while the window is 0    */
        g_last_send_rc = rw.retcode;
        if (rw.retcode <= 0) {
            break;                              /* peer reset / abort              */
        }
        g_stream_sent += (UINT)rw.retcode;
    }
}

static int app_sub(void *arg)
{
    NSFRQE ri, rs, rb, rl, ra, rcl, rt;
    NSFECB naptmr;
    int    i;

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
    rb.p1 = (UINT)NSFCTCI_SRC; rb.p2 = STREAM_PORT;
    nsfreq_call(&rb);
    g_bind_rc = rb.retcode;

    rqe_init(&rl, RQ_LISTEN, g_ldesc);
    rl.p1 = 4u;
    nsfreq_call(&rl);
    g_listen_rc = rl.retcode;

    /* Accept ONE connection (non-blocking poll for ~90 s), then stream to it. */
    {
        UINT child = 0u;
        int  got   = 0;

        for (i = 0; g_have_device && i < 180; i++) {
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
        if (got) {
            g_conns++;
            handle_stream(child);
            rqe_init(&rcl, RQ_CLOSE, child);    /* our FIN -> clean 4-way close   */
            nsfreq_call(&rcl);
        }
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

    printf("=== nsf370 NSFTCP MVS live gate (M4-4 zero-window persist) ===\n");

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

    CHECK_EQ((long)nsfthr_join(t, 120u), 0, "app subtask joined");

    printf("INITAPI rc=%d tok=%08X | LSOCK rc=%d BIND rc=%d LISTEN rc=%d | conns=%d\n",
           (int)g_ini_rc, (unsigned)g_apptok, (int)g_lsock_rc, (int)g_bind_rc,
           (int)g_listen_rc, g_conns);
    printf("STREAM sent=%u last_rc=%d\n", (unsigned)g_stream_sent, (int)g_last_send_rc);
    printf("STATS tcp estab=%u rexmit=%u wndprobe=%u resetrcvd=%u oooseg=%u\n",
           sts_value("NSFTCP", "established"), sts_value("NSFTCP", "rexmit"),
           sts_value("NSFTCP", "wndprobe"),    sts_value("NSFTCP", "resetrcvd"),
           sts_value("NSFTCP", "oooseg"));

    CHECK_EQ((long)g_ini_rc, (long)NSF_RETOK, "INITAPI round-tripped");
    CHECK(g_lsock_rc >= 0, "listen SOCKET returned a descriptor");
    CHECK_EQ((long)g_bind_rc, (long)NSF_RETOK, "BIND ok");
    CHECK_EQ((long)g_listen_rc, (long)NSF_RETOK, "LISTEN ok");
    if (g_conns >= 1) {
        /* A substantial transfer completed through the window mechanism ... */
        CHECK(g_stream_sent > (UINT)NSFTCP_RCVWND_DEFAULT,
              "stream: more than one window transferred");
        /* ... and the STOP/CONT stall drove the PERSIST path (the new behaviour). */
        CHECK(sts_value("NSFTCP", "wndprobe") > 0u,
              "persist: zero-window probes fired during the STOP (wndprobe > 0)");
    }
    /* The lossless link never loses a segment, so the retransmit timer must never
     * fire -- a zero-window stall arms PERSIST, not rexmit (nothing in flight). */
    CHECK_EQ((long)sts_value("NSFTCP", "rexmit"), 0L, "no retransmit on the lossless link");
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
    return mbt_test_summary("TSTTCPR");
}
