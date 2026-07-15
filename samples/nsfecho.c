/*
 * nsfecho.c -- NSFECHO: a UDP echo server, the first user-visible NSF program
 * and the M3 exit-gate demonstration (spec ch. 15 / milestone M3-5).
 *
 * It is a WORKED EXAMPLE of the EZASOKET C API (include/nsfeza.h): bring the
 * stack up, INITAPI/SOCKET/BIND a UDP socket, then loop forever echoing every
 * datagram back to its sender until a QUIT sentinel arrives. Clarity is the
 * point -- this doubles as API documentation, so it favours the obvious over
 * the clever throughout.
 *
 * PHASE 1 SHAPE (spec 5.2, ADR-0022). The stack and the application share one
 * address space and run on two tasks:
 *
 *   main task (EXECUTIVE): builds the pools, wires the request seam, brings up
 *     the CTCI device + IP + UDP, ATTACHes the echo subtask, arms the liveness
 *     heartbeat and runs evt_mainloop() -- the run-to-completion event loop that
 *     drives the CTCI I/O subtasks (incl. the ADR-0027 IOHALT read-park for the
 *     locally-originated reply SENDTO) and drains + dispatches socket requests.
 *   echo subtask (a real second TCB, over the nsfthr seam): the application --
 *     nsf_initapi -> nsf_socket -> nsf_bind -> the blocking recvfrom/sendto echo
 *     loop -> on QUIT nsf_close -> nsf_termapi -> nsfevt_stop().
 *
 * In M5 the stack moves behind a cross-memory transport (the NSFS subsystem);
 * this application relinks unchanged, because the NSFRQE request block -- the
 * only thing crossing the app<->stack boundary -- is frozen (CLAUDE.md §3).
 *
 * BUILD/RUN. NSFECHO is its own load module ([[module]] in project.toml, so it
 * carries the whole Phase-1 stack). Deploy it into NSF.LINKLIB (make deploy)
 * and submit jcl/NSFECHO.jcl; PARM is the UDP port to listen on (default 7, the
 * well-known echo port). Drive it from the host with samples/host/echo_client.py.
 * See samples/README.md.
 *
 * The device is wired to the mvsdev CTCI pair (0500/0501, 192.168.200.1 <-> .2)
 * by the constants below; rebuild with -DNSFECHO_CUU=... etc. for a different
 * pair. (A production STC reads this from PROFILE.TCPIP -- see src/nsfmain.c;
 * the sample hardcodes it to stay small and self-contained.)
 */
#include "nsfeza.h"             /* the EZASOKET C API this sample showcases     */
#include "nsfsoc.h"             /* NSF_AF_INET / NSF_SOCK_DGRAM / soc_count      */
#include "nsfudp.h"             /* nsfudp_reserve/_init/_protops                 */
#include "nsfreq.h"             /* nsfreq_init/_register_proto/_ecb/_drain/...   */
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
#include "nsfsts.h"             /* sts_value (read drop counters at shutdown)    */
#include "nsfmm.h"
#include <stdio.h>
#include <stdlib.h>             /* atoi (EBCDIC-aware on libc370)                */
#include <string.h>             /* memcmp / memcpy / memset                      */

/* ---- device / addressing (edit or -D to match your CTCI pair) --------------- */
#ifndef NSFECHO_CUU
#define NSFECHO_CUU  0x0500u        /* the CTCI read/write subchannel pair       */
#endif
#ifndef NSFECHO_SRC
#define NSFECHO_SRC  0xC0A8C801u    /* 192.168.200.1 -- this MVS guest (HOME)    */
#endif
#ifndef NSFECHO_DST
#define NSFECHO_DST  0xC0A8C802u    /* 192.168.200.2 -- the CTCI peer (host)     */
#endif
#ifndef NSFECHO_MTU
#define NSFECHO_MTU  1500u
#endif

#define ECHO_DEV_NAME      "CTCA"   /* interface name (stats register under it)  */
#define ECHO_DEFAULT_PORT  7u       /* the well-known echo port (RFC 862)        */

/* The receive buffer must hold the largest datagram v1 accepts without
 * fragmenting: MTU - 20 (IP header) - 8 (UDP header). For a 1500 MTU that is
 * 1472; a smaller buffer would silently truncate the datagram (M3-3 truncation
 * semantics) and the echo would come back short. 2048 covers it with margin. */
#define ECHO_BUFSIZE       2048

/* The QUIT sentinel and its BYE reply are the ONE termination path (there is no
 * operator STOP in this sample, by decision). Both are compared and sent as RAW
 * BYTES -- never C string literals: on the EBCDIC target `"QUIT"` compiles to
 * EBCDIC (0xD8 0xE4...) and would NEVER match the ASCII bytes a host client puts
 * on the wire. This is exactly the host/MVS divergence class the project guards
 * against (spec 15.3, binary transparency: NSF never transcodes payload). */
static const UCHAR QUIT_SENTINEL[4] = { 0x51u, 0x55u, 0x49u, 0x54u };  /* "QUIT" */
static const UCHAR BYE_REPLY[3]     = { 0x42u, 0x59u, 0x45u };         /* "BYE"  */

/* ---- shared state (main task <-> echo subtask) ----------------------------- */
static INT           g_port;              /* UDP port from PARM                  */
static volatile int  g_device_up;         /* CTCI came up: the loop may run      */

/* Results the echo subtask records for the main task to report after the join. */
static INT           g_ini_rc, g_sock, g_bind_rc, g_maxsno;
static INT           g_term_rc;
static UINT          g_echoed;            /* datagrams echoed back               */
static UINT          g_send_fail;         /* replies that failed (should be 0)   */
static int           g_quit_seen;         /* clean shutdown via QUIT             */

/* The receive buffer lives in static storage, not on the echo subtask's stack
 * (MVS stacks are limited, CLAUDE.md §3) -- the same choice tstezam makes. Only
 * the single echo subtask touches it, so one shared buffer is safe. */
static UCHAR         g_recv_buf[ECHO_BUFSIZE];

/* ---- sockaddr_in helpers (byte-wise, network order -- the M2 discipline) ---- *
 * Read/write the port + address one byte at a time so the one source is correct
 * on the big-endian S/370 target (spec 11.1 / nsfeza.h). */
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
 * Bring the API up, bind the port, then block in recvfrom until a datagram
 * arrives; echo it straight back to the sender with sendto. A 4-byte QUIT
 * datagram ends the loop: reply BYE, close, termapi, and stop the executive.
 *
 * recvfrom here is a BLOCKING call: with no NSF_MSG_DONTWAIT flag it PARKS the
 * request inside the stack (socket-owned) and the app subtask WAITs on the
 * request's own ECB until the datagram completes it -- nsfreq_call handles that
 * reset-before-WAIT internally, so there is no second wait layer here. This is
 * the natural, minimal way to write a server loop.
 */
static int echo_server(void *arg)
{
    NSF_SOCKADDR_IN local, peer;
    INT             namelen, n, sent;

    (void)arg;

    g_maxsno = -1;
    g_ini_rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "NSFECHO ", &g_maxsno);
    printf("NSFECHO: INITAPI  rc=%d maxsno=%d\n", (int)g_ini_rc, (int)g_maxsno);
    if (g_ini_rc != NSF_RETOK) {
        nsfevt_stop();
        return 0;
    }

    g_sock = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    printf("NSFECHO: SOCKET   rc=%d (socket number)\n", (int)g_sock);
    if (g_sock < 0) {
        (void)nsf_termapi();
        nsfevt_stop();
        return 0;
    }

    mk_sa(&local, (UINT)NSFECHO_SRC, (USHORT)g_port);
    g_bind_rc = nsf_bind(g_sock, &local, (INT)sizeof(local));
    printf("NSFECHO: BIND     rc=%d port=%d\n", (int)g_bind_rc, (int)g_port);
    if (g_bind_rc != NSF_RETOK) {
        (void)nsf_close(g_sock);
        (void)nsf_termapi();
        nsfevt_stop();
        return 0;
    }

    printf("NSFECHO: listening on UDP port %d -- send \"QUIT\" to stop\n",
           (int)g_port);

    for (;;) {
        namelen = 0;
        n = nsf_recvfrom(g_sock, g_recv_buf, (INT)sizeof(g_recv_buf), 0,
                         &peer, &namelen);
        if (n < 0) {
            /* A blocking recvfrom should only fail on a torn-down socket; if it
             * does, stop rather than spin. */
            printf("NSFECHO: recvfrom rc=%d errno=%d -- stopping\n",
                   (int)n, (int)nsf_lasterrno());
            break;
        }

        /* The one termination path: a 4-byte "QUIT" (raw bytes, see above). */
        if (n == (INT)sizeof(QUIT_SENTINEL) &&
            memcmp(g_recv_buf, QUIT_SENTINEL, sizeof(QUIT_SENTINEL)) == 0) {
            g_quit_seen = 1;
            (void)nsf_sendto(g_sock, BYE_REPLY, (INT)sizeof(BYE_REPLY), 0,
                             &peer, (INT)sizeof(peer));
            printf("NSFECHO: QUIT from %u.%u.%u.%u:%u -- replied BYE\n",
                   (unsigned)((sa_addr(&peer) >> 24) & 0xFFu),
                   (unsigned)((sa_addr(&peer) >> 16) & 0xFFu),
                   (unsigned)((sa_addr(&peer) >> 8) & 0xFFu),
                   (unsigned)(sa_addr(&peer) & 0xFFu),
                   (unsigned)sa_port(&peer));
            break;
        }

        /* Echo: send the exact bytes back to the sender. This locally-originated
         * SENDTO drives the ADR-0027 IOHALT read-park in production, once per
         * datagram -- the pattern M2/M3 hardened. */
        sent = nsf_sendto(g_sock, g_recv_buf, n, 0, &peer, (INT)sizeof(peer));
        if (sent == n) {
            g_echoed++;
        } else {
            g_send_fail++;
        }
    }

    (void)nsf_close(g_sock);
    g_term_rc = nsf_termapi();
    printf("NSFECHO: TERMAPI  rc=%d  echoed=%u send_fail=%u\n",
           (int)g_term_rc, (unsigned)g_echoed, (unsigned)g_send_fail);

    nsfevt_stop();                  /* bring the executive loop down cleanly     */
    return 0;
}

/* EV_PACKET_RECEIVED terminus: hand each received packet to the IP layer, which
 * takes ownership of the PBUF (validates, demuxes, frees). */
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
    cfg->cuu    = (USHORT)NSFECHO_CUU;
    cfg->type   = NSFDEV_T_CTCI;
    cfg->ipaddr = (UINT)NSFECHO_SRC;
    cfg->mtu    = (USHORT)NSFECHO_MTU;
}

/* One line per counter, read straight from the registry (sts_value; a
 * text-render + sscanf readback returns 0 on MVS -- the libc370 sscanf trap). */
static void dump_stat(const char *comp, const char *name)
{
    printf("NSFECHO:   %-6s %-9s = %u\n", comp, name, (unsigned)sts_value(comp, name));
}

int main(int argc, char **argv)
{
    NSFTHR *t;
    DEVCFG  cfg;
    NETDEV *dev = NULL;
    int     leak = 0;

    /* PARM is the UDP port (libc370's C startup parses the JCL PARM into argv;
     * atoi is EBCDIC-aware on the target). */
    g_port = (argc > 1) ? atoi(argv[1]) : (INT)ECHO_DEFAULT_PORT;
    if (g_port <= 0 || g_port > 65535) {
        g_port = (INT)ECHO_DEFAULT_PORT;
    }

    printf("=== NSFECHO -- NSF UDP echo server (M3-5 sample) ===\n");
    printf("NSFECHO: port=%d  CTCI=%04X  home=%u.%u.%u.%u\n",
           (int)g_port, (unsigned)NSFECHO_CUU,
           (unsigned)(((UINT)NSFECHO_SRC >> 24) & 0xFFu),
           (unsigned)(((UINT)NSFECHO_SRC >> 16) & 0xFFu),
           (unsigned)(((UINT)NSFECHO_SRC >> 8) & 0xFFu),
           (unsigned)((UINT)NSFECHO_SRC & 0xFFu));

    /* 1. Foundation + pools (the init window; mm_pool_create is sealed after). */
    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) {
        printf("NSFECHO: nsfevt_init failed\n");
        return 8;
    }
    if (buf_init() != 0)                       { printf("NSFECHO: buf_init failed\n");    return 8; }
    if (soc_reserve(0) != 0)                   { printf("NSFECHO: soc_reserve failed\n"); return 8; }
    if (nsfudp_reserve(0) != 0)                { printf("NSFECHO: udp_reserve failed\n"); return 8; }
    if (ctci_reserve(1u, CTCI_BUF_DEFAULT) != 0) { printf("NSFECHO: ctci_reserve failed\n"); return 8; }
    mm_init_complete();

    /* 2. Protocol + request-path init; register UDP and wire the request seam. */
    soc_init();
    nsfreq_init();
    nsfeza_init();
    nsfip_init();
    nsficmp_init();
    nsfudp_init();
    if (nsfreq_register_proto(17u, nsfudp_protops()) != 0) {
        printf("NSFECHO: register UDP failed\n");
        mm_shutdown();
        return 8;
    }
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);
    evt_register(EV_PACKET_RECEIVED, rx_packet);

    /* 3. Bring up the CTCI interface (SVC 99 allocate + subtasks OPEN) and add a
     *    default route through it. Refuse to run if the device will not start. */
    dev_init();
    make_cfg(&cfg);
    dev = dev_register(&cfg, ctci_devops());
    if (dev == NULL || dev_start(dev) != 0) {
        printf("NSFECHO: CTCI %04X failed to start -- cannot run\n",
               (unsigned)NSFECHO_CUU);
        if (dev != NULL) {
            dev_shutdown(dev);
        }
        mm_shutdown();
        return 8;
    }
    nsfip_local_add((UINT)NSFECHO_SRC);
    (void)nsfip_route_add(0u, 0u, dev, 0u);        /* default route -> CTCI      */
    g_device_up = 1;

    /* 4. ATTACH the echo subtask, arm the heartbeat, run the executive loop
     *    until the subtask QUITs and calls nsfevt_stop(). */
    if (nsfthr_setup() != 0) {
        printf("NSFECHO: nsfthr_setup failed\n");
        dev_shutdown(dev);
        mm_shutdown();
        return 8;
    }
    t = nsfthr_create(echo_server, NULL);
    if (t == NULL) {
        printf("NSFECHO: could not ATTACH the echo subtask\n");
        dev_shutdown(dev);
        mm_shutdown();
        return 8;
    }

    nsftmr_plat_arm(1u);
    evt_mainloop();
    (void)nsfthr_join(t, 60u);

    /* 5. Report. Counts + the drop counters that tell the story (the client's
     *    oversize case ticks NSFIP fragdrop / NSFUDP badlen). */
    printf("NSFECHO: === shutdown ===\n");
    printf("NSFECHO: echoed=%u send_fail=%u quit=%s\n",
           (unsigned)g_echoed, (unsigned)g_send_fail,
           g_quit_seen ? "yes" : "no");
    dump_stat("NSFUDP", "in");
    dump_stat("NSFUDP", "out");
    dump_stat("NSFUDP", "noport");
    dump_stat("NSFUDP", "rxfull");
    dump_stat("NSFIP",  "in");
    dump_stat("NSFIP",  "out");
    dump_stat("NSFIP",  "fragdrop");
    dump_stat("NSFIP",  "badlen");
    /* Device (CTCI) counters. On an IDLE link a locally-originated reply can be
     * held in the CTCI write path until the next inbound frame (the open issue
     * #28 / ADR-0027 read-park race) -- `rpurge` (reads the write-kick actively
     * purged) low relative to the echoed count is the direct signal, and `ierr`
     * must stay 0. See samples/README.md. */
    dump_stat(ECHO_DEV_NAME, "in");
    dump_stat(ECHO_DEV_NAME, "out");
    dump_stat(ECHO_DEV_NAME, "rpurge");
    dump_stat(ECHO_DEV_NAME, "ierr");

    /* 6. Leak gate. soc_count() is the production indicator (sockets still
     *    open); it must be 0 after termapi. nsfevt_inuse() must be 0 after the
     *    loop drained. Under an NSF_DEBUG build the pool in-use counters are also
     *    checked. mm_debug_live_regions is deferred to AFTER mm_shutdown -- until
     *    teardown it legitimately counts the pools themselves. */
    if (soc_count() != 0u) {
        printf("NSFECHO: LEAK -- %u socket(s) still open\n", (unsigned)soc_count());
        leak = 1;
    }
    if (nsfevt_inuse() != 0u) {
        printf("NSFECHO: LEAK -- %u EVT still in use\n", (unsigned)nsfevt_inuse());
        leak = 1;
    }
#if NSF_DEBUG
    if (soc_debug_inuse() != 0u) {
        printf("NSFECHO: LEAK -- SOCKET pool in-use=%u\n", (unsigned)soc_debug_inuse());
        leak = 1;
    }
    if (nsfudp_debug_inuse() != 0u) {
        printf("NSFECHO: LEAK -- UDPPCB pool in-use=%u\n", (unsigned)nsfudp_debug_inuse());
        leak = 1;
    }
#endif

    /* 7. Teardown: quiesce the device (release the SVC 99 allocation + I/O
     *    buffers) while the pools still exist, then free the regions. */
    dev_shutdown(dev);
    mm_shutdown();

#if NSF_DEBUG
    if (mm_debug_live_regions() != 0u) {
        printf("NSFECHO: LEAK -- %u pool region(s) after shutdown\n",
               (unsigned)mm_debug_live_regions());
        leak = 1;
    }
#endif

    if (leak) {
        printf("NSFECHO: LEAK GATE FAILED\n");
        return 8;
    }
    printf("NSFECHO: leak gate clean -- CC 0\n");
    return 0;
}
