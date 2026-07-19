/*
 * nsftecho.c -- NSFTECHO: a TCP echo server, the M4 exit-gate demonstration
 * (spec ch. 15 / milestone M4-5). The stream sibling of the M3-5 UDP sample
 * (samples/nsfecho.c) -- same shape, TCP verbs.
 *
 * A WORKED EXAMPLE of the EZASOKET C API stream verbs (include/nsfeza.h): bring
 * the stack up, SOCKET/BIND/LISTEN a TCP socket, then ACCEPT connections one at a
 * time and echo every byte back until the peer closes (recv returns 0 = EOF ->
 * close the connection -> back to accept). Clarity is the point -- this doubles as
 * API documentation, so it favours the obvious over the clever. Sequential
 * connections (one accept/echo at a time) keep it minimal; a `telnet <ip> <port>`
 * from the host is the M4-5 live gate.
 *
 * PHASE 1 SHAPE (spec 5.2, ADR-0022). The stack and the application share one
 * address space and run on two tasks:
 *
 *   main task (EXECUTIVE): builds the pools, wires the request seam, brings up the
 *     CTCI device + IP + TCP, ATTACHes the echo subtask, arms the liveness
 *     heartbeat and runs evt_mainloop() -- the run-to-completion event loop that
 *     drives the CTCI I/O subtasks and drains + dispatches socket requests.
 *   echo subtask (a real second TCB, over the nsfthr seam): the application --
 *     nsf_initapi -> nsf_socket -> nsf_bind -> nsf_listen -> the blocking
 *     accept/recv/send echo loop -> on QUIT nsf_close -> nsf_termapi ->
 *     nsfevt_stop().
 *
 * In M5 the stack moves behind a cross-memory transport (the NSFS subsystem); this
 * application relinks unchanged, because the NSFRQE request block -- the only thing
 * crossing the app<->stack boundary -- is frozen (CLAUDE.md §3).
 *
 * BUILD/RUN. NSFTECHO is its own load module ([[module]] in project.toml, so it
 * carries the whole Phase-1 stack). Deploy it into NSF.LINKLIB (make deploy) and
 * submit jcl/NSFTECHO.jcl; PARM is the TCP port to listen on (default 7, the
 * well-known echo port). Drive it from the host with `telnet 192.168.200.1 <port>`.
 * See samples/README.md.
 *
 * The device is wired to the mvsdev CTCI pair (0500/0501, 192.168.200.1 <-> .2) by
 * the constants below; rebuild with -DNSFTECHO_CUU=... etc. for a different pair.
 */
#include "nsfeza.h"             /* the EZASOKET C API this sample showcases     */
#include "nsfsoc.h"             /* NSF_AF_INET / NSF_SOCK_STREAM / soc_count     */
#include "nsftcp.h"             /* nsftcp_reserve/_init/_protops                 */
#include "nsfreq.h"             /* nsfreq_init/_register_proto/_ecb/_drain/...   */
#include "nsfsel.h"             /* nsfsel_init (SELECT engine registration)      */
#include "nsfip.h"              /* nsfip_input / nsfip_local_add / nsfip_route_add */
#include "nsficmp.h"            /* nsficmp_init                                  */
#include "nsfdev.h"             /* dev_register / dev_start / dev_shutdown       */
#include "nsfctci.h"            /* ctci_reserve / ctci_devops                    */
#include "nsfevt.h"             /* evt_mainloop / evt_set_request / nsfevt_stop  */
#include "nsfthr.h"             /* the app-subtask threading seam                */
#include "nsfstim.h"           /* nsftmr_plat_arm (the liveness heartbeat)      */
#include "nsftmr.h"
#include "nsftrc.h"
#include "nsfbuf.h"
#include "nsfsts.h"             /* sts_value (read counters at shutdown)         */
#include "nsfmm.h"
#include <stdio.h>
#include <stdlib.h>             /* atoi (EBCDIC-aware on libc370)                */
#include <string.h>             /* memcmp / memcpy / memset                      */

/* ---- device / addressing (edit or -D to match your CTCI pair) --------------- */
#ifndef NSFTECHO_CUU
#define NSFTECHO_CUU  0x0500u        /* the CTCI read/write subchannel pair       */
#endif
#ifndef NSFTECHO_SRC
#define NSFTECHO_SRC  0xC0A8C801u    /* 192.168.200.1 -- this MVS guest (HOME)    */
#endif
#ifndef NSFTECHO_DST
#define NSFTECHO_DST  0xC0A8C802u    /* 192.168.200.2 -- the CTCI peer (host)     */
#endif
#ifndef NSFTECHO_MTU
#define NSFTECHO_MTU  1500u
#endif

#define ECHO_DEV_NAME      "CTCA"   /* interface name (stats register under it)  */
#define ECHO_DEFAULT_PORT  7u       /* the well-known echo port (RFC 862)        */
#define ECHO_BACKLOG       5        /* listen backlog                            */
#define ECHO_BUFSIZE       2048     /* per-recv chunk                            */

/* The QUIT sentinel and its BYE reply are the ONE clean-shutdown path. Both are
 * compared and sent as RAW BYTES -- never C string literals: on the EBCDIC target
 * "QUIT" compiles to EBCDIC and would NEVER match the ASCII bytes a host client
 * puts on the wire (spec 15.3, binary transparency). A recv CHUNK that starts with
 * these four bytes ends the server (TCP is a stream, so a real client sends the
 * line and the chunk begins with it). */
static const UCHAR QUIT_SENTINEL[4] = { 0x51u, 0x55u, 0x49u, 0x54u };  /* "QUIT" */
static const UCHAR BYE_REPLY[5]     = { 0x42u, 0x59u, 0x45u, 0x0Du, 0x0Au }; /* "BYE\r\n" */

/* ---- shared state (main task <-> echo subtask) ----------------------------- */
static INT           g_port;              /* TCP port from PARM                  */
static volatile int  g_device_up;         /* CTCI came up: the loop may run      */

static INT           g_ini_rc, g_sock, g_bind_rc, g_listen_rc, g_maxsno;
static INT           g_term_rc;
static UINT          g_conns;             /* connections accepted                */
static UINT          g_echoed;            /* bytes echoed back                   */
static UINT          g_send_fail;         /* echoes that failed (should be 0)    */
static int           g_quit_seen;         /* clean shutdown via QUIT             */

/* The receive buffer lives in static storage, not on the echo subtask's stack
 * (MVS stacks are limited, CLAUDE.md §3) -- the nsfecho choice. Only the single
 * echo subtask touches it, so one shared buffer is safe. */
static UCHAR         g_recv_buf[ECHO_BUFSIZE];

/* ---- sockaddr_in helpers (byte-wise, network order -- the M2 discipline) ---- */
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

/*
 * The echo server, on its own subtask.
 *
 * Bring the API up, bind + listen the port, then ACCEPT one connection at a time
 * and echo every byte back to it until the peer closes (recv returns 0 = EOF). A
 * chunk that begins with "QUIT" ends the whole server: reply BYE, close the
 * connection, stop accepting, termapi, and stop the executive.
 *
 * accept and recv here are BLOCKING calls: with no NSF_MSG_DONTWAIT flag they PARK
 * the request inside the stack (socket-owned) and the app subtask WAITs on the
 * request's own ECB until a connection / data completes it -- the natural, minimal
 * way to write a server loop.
 */
static int echo_server(void *arg)
{
    NSF_SOCKADDR_IN local, peer;
    INT             namelen, conn, n, sent;

    (void)arg;

    g_maxsno = -1;
    g_ini_rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "NSFTECHO", &g_maxsno);
    printf("NSFTECHO: INITAPI  rc=%d maxsno=%d\n", (int)g_ini_rc, (int)g_maxsno);
    if (g_ini_rc != NSF_RETOK) {
        nsfevt_stop();
        return 0;
    }

    g_sock = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    printf("NSFTECHO: SOCKET   rc=%d (socket number)\n", (int)g_sock);
    if (g_sock < 0) {
        (void)nsf_termapi();
        nsfevt_stop();
        return 0;
    }

    mk_sa(&local, (UINT)NSFTECHO_SRC, (USHORT)g_port);
    g_bind_rc = nsf_bind(g_sock, &local, (INT)sizeof(local));
    printf("NSFTECHO: BIND     rc=%d port=%d\n", (int)g_bind_rc, (int)g_port);
    if (g_bind_rc != NSF_RETOK) {
        (void)nsf_close(g_sock);
        (void)nsf_termapi();
        nsfevt_stop();
        return 0;
    }

    g_listen_rc = nsf_listen(g_sock, ECHO_BACKLOG);
    printf("NSFTECHO: LISTEN   rc=%d backlog=%d\n", (int)g_listen_rc, ECHO_BACKLOG);
    if (g_listen_rc != NSF_RETOK) {
        (void)nsf_close(g_sock);
        (void)nsf_termapi();
        nsfevt_stop();
        return 0;
    }

    printf("NSFTECHO: listening on TCP port %d -- send \"QUIT\" to stop\n",
           (int)g_port);

    for (;;) {                              /* accept one connection at a time    */
        namelen = 0;
        conn = nsf_accept(g_sock, &peer, &namelen);
        if (conn < 0) {
            printf("NSFTECHO: accept rc=%d errno=%d -- stopping\n",
                   (int)conn, (int)nsf_lasterrno());
            break;
        }
        g_conns++;
        printf("NSFTECHO: accepted #%u from %u.%u.%u.%u:%u\n",
               (unsigned)g_conns,
               (unsigned)((sa_addr(&peer) >> 24) & 0xFFu),
               (unsigned)((sa_addr(&peer) >> 16) & 0xFFu),
               (unsigned)((sa_addr(&peer) >> 8) & 0xFFu),
               (unsigned)(sa_addr(&peer) & 0xFFu),
               (unsigned)sa_port(&peer));

        for (;;) {                          /* echo until EOF                     */
            n = nsf_recv(conn, g_recv_buf, (INT)sizeof(g_recv_buf), 0);
            if (n < 0) {
                printf("NSFTECHO: recv rc=%d errno=%d\n", (int)n, (int)nsf_lasterrno());
                break;
            }
            if (n == 0) {
                break;                      /* peer closed (EOF) -- back to accept*/
            }
            /* QUIT sentinel (raw bytes): reply BYE and shut the server down. */
            if (n >= (INT)sizeof(QUIT_SENTINEL) &&
                memcmp(g_recv_buf, QUIT_SENTINEL, sizeof(QUIT_SENTINEL)) == 0) {
                g_quit_seen = 1;
                (void)nsf_send(conn, BYE_REPLY, (INT)sizeof(BYE_REPLY), 0);
                printf("NSFTECHO: QUIT on connection #%u -- replied BYE\n",
                       (unsigned)g_conns);
                break;
            }
            /* Echo: send the exact bytes back. */
            sent = nsf_send(conn, g_recv_buf, n, 0);
            if (sent == n) {
                g_echoed += (UINT)n;
            } else {
                g_send_fail++;
            }
        }

        (void)nsf_close(conn);              /* clean FIN on the connection        */
        if (g_quit_seen) {
            break;                          /* QUIT -> stop accepting             */
        }
    }

    (void)nsf_close(g_sock);
    g_term_rc = nsf_termapi();
    printf("NSFTECHO: TERMAPI  rc=%d  conns=%u echoed=%u send_fail=%u\n",
           (int)g_term_rc, (unsigned)g_conns, (unsigned)g_echoed,
           (unsigned)g_send_fail);

    nsfevt_stop();                          /* bring the executive loop down       */
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

/* Build the DEVCFG for the CTCI interface from the constants above. */
static void make_cfg(DEVCFG *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->name, ECHO_DEV_NAME, strlen(ECHO_DEV_NAME));
    cfg->cuu    = (USHORT)NSFTECHO_CUU;
    cfg->type   = NSFDEV_T_CTCI;
    cfg->ipaddr = (UINT)NSFTECHO_SRC;
    cfg->mtu    = (USHORT)NSFTECHO_MTU;
}

static void dump_stat(const char *comp, const char *name)
{
    printf("NSFTECHO:   %-6s %-9s = %u\n", comp, name, (unsigned)sts_value(comp, name));
}

int main(int argc, char **argv)
{
    NSFTHR *t;
    DEVCFG  cfg;
    NETDEV *dev = NULL;
    int     leak = 0;

    g_port = (argc > 1) ? atoi(argv[1]) : (INT)ECHO_DEFAULT_PORT;
    if (g_port <= 0 || g_port > 65535) {
        g_port = (INT)ECHO_DEFAULT_PORT;
    }

    printf("=== NSFTECHO -- NSF TCP echo server (M4-5 sample) ===\n");
    printf("NSFTECHO: port=%d  CTCI=%04X  home=%u.%u.%u.%u\n",
           (int)g_port, (unsigned)NSFTECHO_CUU,
           (unsigned)(((UINT)NSFTECHO_SRC >> 24) & 0xFFu),
           (unsigned)(((UINT)NSFTECHO_SRC >> 16) & 0xFFu),
           (unsigned)(((UINT)NSFTECHO_SRC >> 8) & 0xFFu),
           (unsigned)((UINT)NSFTECHO_SRC & 0xFFu));

    /* 1. Foundation + pools (the init window; mm_pool_create is sealed after). */
    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0)                    { printf("NSFTECHO: nsfevt_init failed\n"); return 8; }
    if (buf_init() != 0)                       { printf("NSFTECHO: buf_init failed\n");    return 8; }
    if (soc_reserve(0) != 0)                   { printf("NSFTECHO: soc_reserve failed\n"); return 8; }
    if (nsftcp_reserve(0) != 0)                { printf("NSFTECHO: tcp_reserve failed\n"); return 8; }
    if (ctci_reserve(1u, CTCI_BUF_DEFAULT) != 0) { printf("NSFTECHO: ctci_reserve failed\n"); return 8; }
    mm_init_complete();

    /* 2. Protocol + request-path init; register TCP and wire the request seam. */
    soc_init();
    nsfreq_init();
    nsfsel_init();
    nsfeza_init();
    nsfip_init();
    nsficmp_init();
    nsftcp_init();
    if (nsfreq_register_proto(6u, nsftcp_protops()) != 0) {
        printf("NSFTECHO: register TCP failed\n");
        mm_shutdown();
        return 8;
    }
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);
    evt_register(EV_PACKET_RECEIVED, rx_packet);

    /* 3. Bring up the CTCI interface + add a default route. */
    dev_init();
    make_cfg(&cfg);
    dev = dev_register(&cfg, ctci_devops());
    if (dev == NULL || dev_start(dev) != 0) {
        printf("NSFTECHO: CTCI %04X failed to start -- cannot run\n",
               (unsigned)NSFTECHO_CUU);
        if (dev != NULL) {
            dev_shutdown(dev);
        }
        mm_shutdown();
        return 8;
    }
    nsfip_local_add((UINT)NSFTECHO_SRC);
    (void)nsfip_route_add(0u, 0u, dev, 0u);        /* default route -> CTCI      */
    g_device_up = 1;

    /* 4. ATTACH the echo subtask, arm the heartbeat, run the executive loop. */
    if (nsfthr_setup() != 0) {
        printf("NSFTECHO: nsfthr_setup failed\n");
        dev_shutdown(dev);
        mm_shutdown();
        return 8;
    }
    t = nsfthr_create(echo_server, NULL);
    if (t == NULL) {
        printf("NSFTECHO: could not ATTACH the echo subtask\n");
        dev_shutdown(dev);
        mm_shutdown();
        return 8;
    }

    nsftmr_plat_arm(1u);
    evt_mainloop();
    (void)nsfthr_join(t, 60u);

    /* 5. Report. */
    printf("NSFTECHO: === shutdown ===\n");
    printf("NSFTECHO: conns=%u echoed=%u send_fail=%u quit=%s\n",
           (unsigned)g_conns, (unsigned)g_echoed, (unsigned)g_send_fail,
           g_quit_seen ? "yes" : "no");
    dump_stat("NSFTCP", "established");
    dump_stat("NSFTCP", "passiveopen");
    dump_stat("NSFTCP", "activeopen");
    dump_stat("NSFTCP", "resetrcvd");
    dump_stat("NSFIP",  "in");
    dump_stat("NSFIP",  "out");
    dump_stat(ECHO_DEV_NAME, "in");
    dump_stat(ECHO_DEV_NAME, "out");
    dump_stat(ECHO_DEV_NAME, "rpurge");
    dump_stat(ECHO_DEV_NAME, "ierr");

    /* 6. Leak gate. soc_count() must be 0 after termapi (listener + every
     *    connection closed); nsfevt_inuse() must be 0 after the loop drained. */
    if (soc_count() != 0u) {
        printf("NSFTECHO: LEAK -- %u socket(s) still open\n", (unsigned)soc_count());
        leak = 1;
    }
    if (nsfevt_inuse() != 0u) {
        printf("NSFTECHO: LEAK -- %u EVT still in use\n", (unsigned)nsfevt_inuse());
        leak = 1;
    }
#if NSF_DEBUG
    if (soc_debug_inuse() != 0u) {
        printf("NSFTECHO: LEAK -- SOCKET pool in-use=%u\n", (unsigned)soc_debug_inuse());
        leak = 1;
    }
    if (nsftcp_debug_inuse() != 0u) {
        printf("NSFTECHO: LEAK -- TCPTCB pool in-use=%u\n", (unsigned)nsftcp_debug_inuse());
        leak = 1;
    }
#endif

    /* 7. Teardown: quiesce the device, then free the regions. */
    dev_shutdown(dev);
    mm_shutdown();

#if NSF_DEBUG
    if (mm_debug_live_regions() != 0u) {
        printf("NSFTECHO: LEAK -- %u pool region(s) after shutdown\n",
               (unsigned)mm_debug_live_regions());
        leak = 1;
    }
#endif

    if (leak) {
        printf("NSFTECHO: LEAK GATE FAILED\n");
        return 8;
    }
    printf("NSFTECHO: leak gate clean -- CC 0\n");
    return 0;
}
