/*
 * tsteza.c -- NSFEZA host unit tests: the EZASOKET C API + the EZASOH03 plist
 * decoder, over the M3-2 request transport (spec 15, ADR-0029).
 *
 * HOST-ONLY (project.toml mvs = false). The @@NS* C-API functions submit + WAIT
 * (nsfreq_call), so they need a running drainer: this test uses the TSTREQ
 * round-trip harness -- an EXECUTIVE thread runs evt_mainloop with the request
 * seam wired, the MAIN thread (an "app" task) issues the API calls, and a
 * test-only UDP-shaped dummy PROTOPS registered at proto 17 makes SOCKET/BIND/
 * SENDTO/RECVFROM actually complete. No CTCI, no IP, no real transport -- the
 * MVS full-stack proof is test/mvs/tstezam.c; the EZASOH03 asm veneer is proved
 * by test/mvs/tstezah.asm. Nothing here bypasses the transport (per the advisor
 * note): every call goes NSFEZA -> NSFRQE -> executive -> dummy op -> completion.
 *
 * Coverage: 0-based numbering, MAXSOC clamp + MAXSNO, table exhaustion (EMFILE),
 * implicit INITAPI, EBADF-after-CLOSE + double-CLOSE, non-blocking RECVFROM
 * (EWOULDBLOCK), a blocking RECVFROM round-trip with the peer address, TERMAPI
 * closing all + emptying the registry, RETCODE/ERRNO mapping per function, the
 * EZASOH03 decoder (happy path per code + unsupported/unknown -> EOPNOTSUPP +
 * R15 always 0), and the leak gates (SOCKET pool + mm back to baseline after
 * TERMAPI and after an abandoned-socket TERMAPI).
 */
#include "nsfeza.h"
#include "nsfreq.h"
#include "nsfsoc.h"
#include "nsfsel.h"
#include "nsfevt.h"
#include "nsfthr.h"
#include "nsftmr.h"
#include "nsfbuf.h"
#include "nsfsts.h"
#include "nsfmm.h"
#include <mbtcheck.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define TEST_PROTO   17u            /* dummy registered where SOCKET(...,DGRAM,0) lands */

/* A known peer the dummy recv delivery reports, to prove RECVFROM's addr/port. */
#define PEER_ADDR    0xC0A8C802u    /* 192.168.200.2 */
#define PEER_PORT    5555u

/* ---- test-only dummy protocol (the TSTREQ PROTOPS shape) ------------------- */
static PROTOPS dummy_ops;                    /* fwd: d_accept spawns a child      */

/* Per-socket SELECT readiness, indexed by SOCKCB.id (set before an immediate-
 * ready nsf_select so it never parks in the threaded harness). */
static int g_ready[NSFSOC_MAX_DEFAULT];

static int d_attach(SOCKCB *s)             { (void)s; return 0; }
static int d_bind(SOCKCB *s)               { (void)s; return 0; }
static int d_listen(SOCKCB *s, int bl)     { (void)s; (void)bl; return 0; }
static int d_detach(SOCKCB *s)             { (void)s; return 0; }
static int d_poll(SOCKCB *s, int want)     { return g_ready[s->id] & want; }

/* CONNECT: record the peer (so GETPEERNAME can read it) and complete RETOK. */
static int d_connect(SOCKCB *s, NSFRQE *r)
{
    s->faddr = r->p1;
    s->fport = (USHORT)r->p2;
    s->state = (UCHAR)SOC_ST_CONNECTED;
    soc_complete(r, NSF_RETOK, 0);
    return 0;
}

/* ACCEPT: spawn a child socket (scoped to the same app), hand its descriptor + a
 * fixed peer back -- exercises the facade's accept->new-number mapping. */
static int d_accept(SOCKCB *s, NSFRQE *r)
{
    SOCKCB *child = soc_create(s->domain, s->type, s->proto, &dummy_ops);

    if (child == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EMFILE);
        return 0;
    }
    child->owner_ascb = s->owner_ascb;      /* TERMAPI mass-teardown finds it     */
    child->faddr      = PEER_ADDR;
    child->fport      = (USHORT)PEER_PORT;
    child->state      = (UCHAR)SOC_ST_CONNECTED;
    r->p1 = child->faddr;
    r->p2 = (UINT)child->fport;
    soc_complete(r, (INT)soc_desc(child), 0);
    return 0;
}

/* SHUTDOWN: acknowledge (a UDP-shaped dummy has no FIN path). */
static int d_shutdown(SOCKCB *s, NSFRQE *r)
{
    (void)s;
    soc_complete(r, NSF_RETOK, 0);
    return 0;
}

static int d_close(SOCKCB *s, NSFRQE *r)   { (void)s; (void)r; return 0; }

/* Round-trip handshake: d_recv signals when it parks; a SENDTO trigger's d_send
 * completes the parked recv (the "datagram arrived" moment). */
static pthread_mutex_t g_hs_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_hs_cv  = PTHREAD_COND_INITIALIZER;
static int             g_parked;

static void signal_parked(void)
{
    pthread_mutex_lock(&g_hs_mtx);
    g_parked = 1;
    pthread_cond_broadcast(&g_hs_cv);
    pthread_mutex_unlock(&g_hs_mtx);
}
static void wait_parked(void)
{
    pthread_mutex_lock(&g_hs_mtx);
    while (!g_parked) {
        pthread_cond_wait(&g_hs_cv, &g_hs_mtx);
    }
    pthread_mutex_unlock(&g_hs_mtx);
}

static int d_recv(SOCKCB *s, NSFRQE *r)
{
    if (r->flags & RQ_F_NONBLOCK) {                 /* empty + non-blocking      */
        soc_complete(r, NSF_RETERR, NSF_EWOULDBLOCK);
        return 0;
    }
    {
        int rc = soc_park(s, r, SOC_PEND_RECV);     /* empty + blocking: park    */
        if (rc == 0) {
            signal_parked();
        }
        return rc;
    }
}

/* SENDTO: deliver to a parked recv (set the peer, complete with 7 bytes), then
 * complete the send itself with its own byte count. */
static int d_send(SOCKCB *s, NSFRQE *r)
{
    if (s->pend_recv != NULL) {
        s->pend_recv->p1 = PEER_ADDR;
        s->pend_recv->p2 = PEER_PORT;
        soc_complete(s->pend_recv, 7, 0);
    }
    soc_complete(r, (INT)r->ulen, 0);
    return 0;
}

static PROTOPS dummy_ops = {
    d_attach, d_bind, d_connect, d_listen, d_send, d_recv, d_close, d_detach,
    d_accept, d_poll, d_shutdown
};

/* ---- harness --------------------------------------------------------------- */
static void *exec_thread(void *arg)
{
    (void)arg;
    evt_mainloop();
    return NULL;
}

static UINT sock_inuse(void)
{
#if NSF_DEBUG
    return soc_debug_inuse();
#else
    return 0u;
#endif
}

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

/* ---- scenarios ------------------------------------------------------------- */

/* S1: full lifecycle + RETCODE/ERRNO mapping + 0-based numbering. */
static void test_lifecycle(void)
{
    NSF_SOCKADDR_IN sa, sa2, dst;
    INT s, rc, namelen;

    nsfeza_init();

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);     /* implicit INITAPI      */
    CHECK_EQ((long)s, 0L, "first socket is number 0 (0-based)");

    mk_sa(&sa, 0xC0A8C801u, 7777u);                     /* 192.168.200.1:7777    */
    rc = nsf_bind(s, &sa, (INT)sizeof(sa));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "BIND ok");

    namelen = 0;
    rc = nsf_getsockname(s, &sa2, &namelen);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "GETSOCKNAME ok");
    CHECK_EQ((long)sa_addr(&sa2), (long)0xC0A8C801u, "GETSOCKNAME returned the bound addr");
    CHECK_EQ((long)sa_port(&sa2), 7777L, "GETSOCKNAME returned the bound port");
    CHECK_EQ((long)namelen, (long)sizeof(NSF_SOCKADDR_IN), "GETSOCKNAME set namelen=16");

    mk_sa(&dst, PEER_ADDR, 9u);
    rc = nsf_sendto(s, "test", 4, 0, &dst, (INT)sizeof(dst));
    CHECK_EQ((long)rc, 4L, "SENDTO returned the byte count (4)");

    /* non-blocking RECVFROM on an empty socket -> EWOULDBLOCK */
    rc = nsf_recvfrom(s, sa.sin_zero, 8, NSF_MSG_DONTWAIT, NULL, NULL);
    CHECK_EQ((long)rc, (long)NSF_RETERR, "non-blocking RECVFROM on empty -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EWOULDBLOCK, "  errno EWOULDBLOCK");

    rc = nsf_close(s);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "CLOSE ok");

    /* any later use of the closed number -> EBADF */
    rc = nsf_bind(s, &sa, (INT)sizeof(sa));
    CHECK_EQ((long)rc, (long)NSF_RETERR, "BIND after CLOSE -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF, "  errno EBADF");
    rc = nsf_close(s);
    CHECK_EQ((long)rc, (long)NSF_RETERR, "double CLOSE -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF, "  errno EBADF");

    CHECK_EQ((long)nsf_termapi(), (long)NSF_RETOK, "TERMAPI ok");
    CHECK_EQ((long)soc_count(), 0L, "no sockets left after TERMAPI");
}

/* S2: implicit INITAPI (SOCKET with no prior INITAPI auto-registers). */
static void test_implicit_init(void)
{
    INT s;

    nsfeza_init();
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "implicit INITAPI: SOCKET without INITAPI succeeds");
    CHECK_EQ((long)soc_count(), 1L, "one socket created");
    (void)nsf_termapi();
    CHECK_EQ((long)soc_count(), 0L, "TERMAPI closed the implicitly-init'd socket");
}

/* S3: explicit INITAPI, MAXSOC clamp, MAXSNO, table exhaustion. */
static void test_maxsoc(void)
{
    INT rc, maxsno, s0, s1, s2;

    /* small MAXSOC -> exhaustion */
    nsfeza_init();
    maxsno = -1;
    rc = nsf_initapi(2, "TCPIP   ", "ADDRSPC ", "SUBTASK ", &maxsno);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "INITAPI(maxsoc=2) ok");
    CHECK_EQ((long)maxsno, 1L, "MAXSNO = clamped_maxsoc - 1 (1)");
    s0 = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    s1 = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK_EQ((long)s0, 0L, "socket 0");
    CHECK_EQ((long)s1, 1L, "socket 1");
    s2 = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK_EQ((long)s2, (long)NSF_RETERR, "3rd socket over MAXSOC -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EMFILE, "  errno EMFILE");
    (void)nsf_termapi();

    /* high MAXSOC clamps to the pool limit */
    nsfeza_init();
    maxsno = -1;
    rc = nsf_initapi(1000, NULL, NULL, NULL, &maxsno);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "INITAPI(maxsoc=1000) ok");
    CHECK_EQ((long)maxsno, (long)(NSFEZA_MAXSOC - 1), "MAXSNO clamped to pool limit-1");
    (void)nsf_termapi();

    /* default MAXSOC (0) -> full pool */
    nsfeza_init();
    maxsno = -1;
    rc = nsf_initapi(0, NULL, NULL, NULL, &maxsno);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "INITAPI(default) ok");
    CHECK_EQ((long)maxsno, (long)(NSFEZA_MAXSOC - 1), "default MAXSNO = pool limit-1");

    /* a second INITAPI without TERMAPI -> EINVAL (one per AS in v1) */
    rc = nsf_initapi(0, NULL, NULL, NULL, &maxsno);
    CHECK_EQ((long)rc, (long)NSF_RETERR, "double INITAPI -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EINVAL, "  errno EINVAL");
    (void)nsf_termapi();
}

/* S4: TERMAPI closes abandoned sockets + the leak gate. */
static void test_termapi_teardown(void)
{
    INT i, s;

    nsfeza_init();
    CHECK_EQ((long)nsf_initapi(0, NULL, NULL, NULL, NULL), (long)NSF_RETOK, "INITAPI ok");
    for (i = 0; i < 3; i++) {
        s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
        CHECK(s >= 0, "opened a socket (no close -- abandoned)");
    }
    CHECK_EQ((long)soc_count(), 3L, "3 sockets open");
    CHECK_EQ((long)nsf_termapi(), (long)NSF_RETOK, "TERMAPI ok");
    CHECK_EQ((long)soc_count(), 0L, "TERMAPI closed all abandoned sockets");
    CHECK_EQ((long)sock_inuse(), 0L, "SOCKET pool back to baseline (leak gate)");
}

/* S5: blocking RECVFROM round-trip with the peer address (a real park/complete
 * across threads). The app thread parks in RECVFROM; the main thread triggers a
 * SENDTO whose dummy op completes it. */
static volatile INT g_rt_s;
static volatile INT g_rt_recv;
static NSF_SOCKADDR_IN g_rt_peer;

static void *rt_app(void *arg)
{
    NSF_SOCKADDR_IN sa;
    INT s, namelen;

    (void)arg;
    nsfeza_init();
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    mk_sa(&sa, 0xC0A8C801u, 7777u);
    (void)nsf_bind(s, &sa, (INT)sizeof(sa));
    g_rt_s = s;

    namelen = 0;
    g_rt_recv = nsf_recvfrom(s, sa.sin_zero, 8, 0, &g_rt_peer, &namelen);  /* blocks */

    (void)nsf_close(s);
    (void)nsf_termapi();
    return NULL;
}

static void test_roundtrip(void)
{
    pthread_t app;
    NSF_SOCKADDR_IN dst;
    INT rc;

    g_parked = 0; g_rt_s = -1; g_rt_recv = -99;
    pthread_create(&app, NULL, rt_app, NULL);

    wait_parked();                          /* the RECVFROM has parked           */
    CHECK_EQ((long)g_rt_recv, -99L, "round-trip: app still blocked (not woken)");

    mk_sa(&dst, PEER_ADDR, 9u);
    rc = nsf_sendto(g_rt_s, "hello!!", 7, 0, &dst, (INT)sizeof(dst));
    CHECK_EQ((long)rc, 7L, "round-trip: SENDTO trigger returned its count");

    pthread_join(app, NULL);
    CHECK_EQ((long)g_rt_recv, 7L, "round-trip: RECVFROM returned the delivered count");
    CHECK_EQ((long)sa_addr(&g_rt_peer), (long)PEER_ADDR, "RECVFROM peer addr");
    CHECK_EQ((long)sa_port(&g_rt_peer), (long)PEER_PORT, "RECVFROM peer port");
    CHECK_EQ((long)soc_count(), 0L, "round-trip: no sockets left");
}

/* S6: the EZASOH03 plist decoder. Builds plists (host-natural pointers) and
 * drives nsf_ezasoh03 -- the exact path the asm veneer will take on MVS. */
typedef struct ezapl_t {                    /* mirrors src/nsfeza.c EZAPL         */
    const char *func;
    INT        *errnop;
    INT        *retcodep;
    void       *arg[8];
} EZAPL_T;

static void test_decoder(void)
{
    EZAPL_T pl;
    INT     er, rcv;                         /* the plist ERRNO / RETCODE slots   */
    INT     af, type, proto, maxsoc, maxsno;
    USHORT  snum;
    char    ident[16];
    char    subtask[8];

    memset(&pl, 0, sizeof(pl));
    pl.errnop = &er; pl.retcodep = &rcv;

    /* unknown code -> RETCODE -1, ERRNO EOPNOTSUPP, return 0 (R15 always 0) */
    er = 0; rcv = 0;
    pl.func = "ZZZZ";
    CHECK_EQ((long)nsf_ezasoh03(&pl), 0L, "decoder returns 0 (R15 always 0)");
    CHECK_EQ((long)rcv, (long)NSF_RETERR, "unknown code -> RETCODE -1");
    CHECK_EQ((long)er, (long)NSF_EOPNOTSUPP, "unknown code -> ERRNO EOPNOTSUPP");

    /* a known-but-still-unimplemented code (GETHOSTBYNAME, M6/DNS) -> same clean
     * EOPNOTSUPP (CONN et al. are implemented from M4-5; see test_decoder_m4). */
    er = 0; rcv = 0;
    pl.func = "GETH";
    (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETERR, "GETH (unsupported) -> RETCODE -1");
    CHECK_EQ((long)er, (long)NSF_EOPNOTSUPP, "GETH (unsupported) -> ERRNO EOPNOTSUPP");

    /* INIT via the decoder */
    nsfeza_init();
    memset(ident, ' ', sizeof(ident));
    memset(subtask, ' ', sizeof(subtask));
    maxsoc = 0; maxsno = -1;
    pl.func = "INIT";
    pl.arg[0] = &maxsoc; pl.arg[1] = ident; pl.arg[2] = subtask; pl.arg[3] = &maxsno;
    er = 0; rcv = -1;
    (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETOK, "decoder INIT -> RETCODE 0");
    CHECK_EQ((long)maxsno, (long)(NSFEZA_MAXSOC - 1), "decoder INIT set MAXSNO");

    /* SOCK via the decoder -> a descriptor (>= 0) in RETCODE */
    af = NSF_AF_INET; type = NSF_SOCK_DGRAM; proto = 0;
    pl.func = "SOCK";
    pl.arg[0] = &af; pl.arg[1] = &type; pl.arg[2] = &proto;
    er = -1; rcv = -1;
    (void)nsf_ezasoh03(&pl);
    CHECK(rcv >= 0, "decoder SOCK -> a descriptor (>= 0) in RETCODE");
    CHECK_EQ((long)er, 0L, "decoder SOCK -> ERRNO 0 on success");
    snum = (USHORT)rcv;

    /* CLOS via the decoder (S is a halfword) */
    pl.func = "CLOS";
    pl.arg[0] = &snum;
    er = -1; rcv = -1;
    (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETOK, "decoder CLOS -> RETCODE 0");

    /* TERM via the decoder (no RETCODE/ERRNO stored) */
    pl.func = "TERM";
    (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)soc_count(), 0L, "decoder TERM emptied the app");
}

/* S7: the M4-5 verbs through the core -- CONNECT/GETPEERNAME, LISTEN/ACCEPT (the
 * new-number mapping), SEND, SHUTDOWN, and the EBADF / ENOTCONN error matrix. */
static void test_m4_verbs(void)
{
    NSF_SOCKADDR_IN peer, got;
    INT s, l, a, rc, namelen;

    nsfeza_init();

    /* CONNECT records a peer; GETPEERNAME reads it back. */
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    CHECK(s >= 0, "m4: stream socket");
    mk_sa(&peer, PEER_ADDR, PEER_PORT);
    rc = nsf_connect(s, &peer, (INT)sizeof(peer));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "m4: CONNECT ok");
    namelen = 0;
    rc = nsf_getpeername(s, &got, &namelen);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "m4: GETPEERNAME ok");
    CHECK_EQ((long)sa_addr(&got), (long)PEER_ADDR, "m4: peer addr");
    CHECK_EQ((long)sa_port(&got), (long)PEER_PORT, "m4: peer port");

    rc = nsf_send(s, "hello", 5, 0);
    CHECK_EQ((long)rc, 5L, "m4: SEND byte count");
    rc = nsf_shutdown(s, SHUT_WR);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "m4: SHUTDOWN(WR) ok");

    /* LISTEN + ACCEPT hands back a NEW socket number (!= the listener). */
    l = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    rc = nsf_listen(l, 5);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "m4: LISTEN ok");
    namelen = 0;
    a = nsf_accept(l, &got, &namelen);
    CHECK(a >= 0, "m4: ACCEPT -> a socket number");
    CHECK(a != l, "m4: ACCEPT number differs from the listener");
    CHECK_EQ((long)sa_addr(&got), (long)PEER_ADDR, "m4: ACCEPT peer addr");
    CHECK_EQ((long)namelen, (long)sizeof(NSF_SOCKADDR_IN), "m4: ACCEPT namelen=16");

    /* GETPEERNAME on an unconnected socket -> ENOTCONN. */
    rc = nsf_getpeername(l, &got, &namelen);
    CHECK_EQ((long)rc, (long)NSF_RETERR, "m4: GETPEERNAME on a listener -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_ENOTCONN, "  errno ENOTCONN");

    /* EBADF matrix: every verb on a closed number. */
    (void)nsf_close(s);
    CHECK_EQ((long)nsf_connect(s, &peer, (INT)sizeof(peer)), (long)NSF_RETERR, "m4: CONNECT closed -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF, "  CONNECT EBADF");
    CHECK_EQ((long)nsf_send(s, "x", 1, 0), (long)NSF_RETERR, "m4: SEND closed -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF, "  SEND EBADF");
    CHECK_EQ((long)nsf_recv(s, &got, 4, 0), (long)NSF_RETERR, "m4: RECV closed -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF, "  RECV EBADF");
    CHECK_EQ((long)nsf_shutdown(s, SHUT_RDWR), (long)NSF_RETERR, "m4: SHUTDOWN closed -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EBADF, "  SHUTDOWN EBADF");
    CHECK_EQ((long)nsf_listen(s, 1), (long)NSF_RETERR, "m4: LISTEN closed -> -1");
    CHECK_EQ((long)nsf_accept(s, NULL, NULL), (long)NSF_RETERR, "m4: ACCEPT closed -> -1");
    CHECK_EQ((long)nsf_getpeername(s, &got, &namelen), (long)NSF_RETERR, "m4: GETPEERNAME closed -> -1");

    (void)nsf_termapi();
    CHECK_EQ((long)soc_count(), 0L, "m4: TERMAPI emptied the app (listener + child)");
}

/* S8: FIONBIO / FCNTL persistence -- the flag threads RQ_F_NONBLOCK into every
 * subsequent request (a non-blocking RECV on an empty socket -> EWOULDBLOCK). */
static void test_fionbio(void)
{
    INT s, rc, on = 1, off = 0, fl;
    char buf[8];

    nsfeza_init();
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "fionbio: socket");

    /* default blocking: F_GETFL has no O_NONBLOCK. */
    fl = nsf_fcntl(s, NSF_F_GETFL, 0);
    CHECK_EQ((long)(fl & NSF_O_NONBLOCK), 0L, "fionbio: default blocking");

    /* IOCTL(FIONBIO, 1) -> a blocking RECV now returns EWOULDBLOCK. */
    rc = nsf_ioctl(s, (INT)NSF_FIONBIO, &on);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "fionbio: IOCTL FIONBIO on");
    fl = nsf_fcntl(s, NSF_F_GETFL, 0);
    CHECK(fl & NSF_O_NONBLOCK, "fionbio: F_GETFL shows O_NONBLOCK");
    rc = nsf_recv(s, buf, (INT)sizeof(buf), 0);         /* flags=0, but persistent */
    CHECK_EQ((long)rc, (long)NSF_RETERR, "fionbio: non-blocking RECV -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EWOULDBLOCK, "  errno EWOULDBLOCK");

    /* IOCTL(FIONBIO, 0) clears it; F_SETFL O_NONBLOCK sets it again. */
    rc = nsf_ioctl(s, (INT)NSF_FIONBIO, &off);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "fionbio: IOCTL FIONBIO off");
    fl = nsf_fcntl(s, NSF_F_GETFL, 0);
    CHECK_EQ((long)(fl & NSF_O_NONBLOCK), 0L, "fionbio: cleared");
    (void)nsf_fcntl(s, NSF_F_SETFL, NSF_O_NONBLOCK);
    rc = nsf_recv(s, buf, (INT)sizeof(buf), 0);
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EWOULDBLOCK, "fionbio: F_SETFL path -> EWOULDBLOCK");

    (void)nsf_close(s);
    (void)nsf_termapi();
}

/* S9: SETSOCKOPT/GETSOCKOPT minimal set. */
static void test_sockopt(void)
{
    INT s, rc, val, len, one = 1;

    nsfeza_init();
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);

    rc = nsf_setsockopt(s, NSF_SOL_SOCKET, NSF_SO_REUSEADDR, &one, (INT)sizeof(one));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "sockopt: SET SO_REUSEADDR accepted");
    rc = nsf_setsockopt(s, NSF_IPPROTO_TCP, NSF_TCP_NODELAY, &one, (INT)sizeof(one));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "sockopt: SET TCP_NODELAY accepted");
    rc = nsf_setsockopt(s, NSF_SOL_SOCKET, 0x7777, &one, (INT)sizeof(one));
    CHECK_EQ((long)rc, (long)NSF_RETERR, "sockopt: SET unknown -> -1");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EOPNOTSUPP, "  errno EOPNOTSUPP");

    val = 0; len = (INT)sizeof(val);
    rc = nsf_getsockopt(s, NSF_SOL_SOCKET, NSF_SO_RCVBUF, &val, &len);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "sockopt: GET SO_RCVBUF ok");
    CHECK_EQ((long)val, (long)NSF_SO_DFLT_BUF, "sockopt: SO_RCVBUF value");
    val = 0; len = (INT)sizeof(val);
    rc = nsf_getsockopt(s, NSF_SOL_SOCKET, NSF_SO_SNDBUF, &val, &len);
    CHECK_EQ((long)val, (long)NSF_SO_DFLT_BUF, "sockopt: SO_SNDBUF value");
    val = 0; len = (INT)sizeof(val);
    rc = nsf_getsockopt(s, NSF_SOL_SOCKET, 0x7777, &val, &len);
    CHECK_EQ((long)rc, (long)NSF_RETERR, "sockopt: GET unknown -> -1");

    (void)nsf_close(s);
    (void)nsf_termapi();
}

/* S10: SELECT mask byte-exactness -- the right-to-left fullword numbering built
 * against the addendum LITERALLY (descriptors 0, 31, 32 -- the fullword boundary).
 * Sockets are made ready BEFORE the call so nsf_select returns immediately (never
 * parks in the threaded harness). */
static void test_select_masks(void)
{
    UCHAR rmask[8], wmask[8];
    INT   i, rc, s;

    nsfeza_init();
    /* create 33 sockets: numbers 0..32 (== SOCKCB.id for a fresh sequence). */
    for (i = 0; i < 33; i++) {
        s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
        CHECK_EQ((long)s, (long)i, "selmask: socket number == i");
    }
    memset(g_ready, 0, sizeof(g_ready));
    g_ready[0]  = (int)SEL_READ;                    /* number 0 read-ready         */
    g_ready[31] = (int)SEL_WRITE;                   /* number 31 write-ready       */
    g_ready[32] = (int)SEL_READ;                    /* number 32 read-ready        */

    /* interest: read on 0 and 32, write on 31 (right-to-left, byte-wise). */
    memset(rmask, 0, sizeof(rmask));
    memset(wmask, 0, sizeof(wmask));
    rmask[3] = 0x01u;                               /* bit 0  (LSB of word 0)      */
    rmask[7] = 0x01u;                               /* bit 32 (LSB of word 1)      */
    wmask[0] = 0x80u;                               /* bit 31 (MSB of word 0)      */

    rc = nsf_select(33, rmask, wmask, NULL, 0, 0);  /* poll form -> immediate      */
    CHECK_EQ((long)rc, 3L, "selmask: 3 sockets ready");

    /* byte-exact output masks. */
    CHECK(rmask[0] == 0x00u && rmask[1] == 0x00u && rmask[2] == 0x00u &&
          rmask[3] == 0x01u, "selmask: read word0 = 0x00000001 (desc 0)");
    CHECK(rmask[4] == 0x00u && rmask[5] == 0x00u && rmask[6] == 0x00u &&
          rmask[7] == 0x01u, "selmask: read word1 = 0x00000001 (desc 32)");
    CHECK(wmask[0] == 0x80u && wmask[1] == 0x00u && wmask[2] == 0x00u &&
          wmask[3] == 0x00u, "selmask: write word0 = 0x80000000 (desc 31)");
    CHECK(wmask[4] == 0x00u && wmask[5] == 0x00u && wmask[6] == 0x00u &&
          wmask[7] == 0x00u, "selmask: write word1 empty");

    memset(g_ready, 0, sizeof(g_ready));
    (void)nsf_termapi();
    CHECK_EQ((long)soc_count(), 0L, "selmask: TERMAPI closed all 33");
}

/* S11: the EZASOH03 decoder for the M4-5 codes (happy path per code). */
static void test_decoder_m4(void)
{
    EZAPL_T pl;
    INT     er, rcv, af, type, proto, val, optlen, how, backlog;
    USHORT  snum, lnum;
    NSF_SOCKADDR_IN peer, got;

    memset(&pl, 0, sizeof(pl));
    pl.errnop = &er; pl.retcodep = &rcv;
    nsfeza_init();

    /* INIT + SOCK a stream socket to operate on. */
    { INT maxsoc = 0; char id[16], st[8];
      memset(id, ' ', sizeof(id)); memset(st, ' ', sizeof(st));
      pl.func = "INIT"; pl.arg[0] = &maxsoc; pl.arg[1] = id; pl.arg[2] = st;
      pl.arg[3] = NULL; (void)nsf_ezasoh03(&pl); }
    af = NSF_AF_INET; type = NSF_SOCK_STREAM; proto = 0;
    pl.func = "SOCK"; pl.arg[0] = &af; pl.arg[1] = &type; pl.arg[2] = &proto;
    (void)nsf_ezasoh03(&pl);
    CHECK(rcv >= 0, "decm4: SOCK -> descriptor");
    snum = (USHORT)rcv;

    /* CONN via the decoder. */
    mk_sa(&peer, PEER_ADDR, PEER_PORT);
    pl.func = "CONN"; pl.arg[0] = &snum; pl.arg[1] = &peer;
    er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETOK, "decm4: CONN -> RETCODE 0");

    /* GETP via the decoder returns the peer. */
    pl.func = "GETP"; pl.arg[0] = &snum; pl.arg[1] = &got;
    er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETOK, "decm4: GETP -> RETCODE 0");
    CHECK_EQ((long)sa_addr(&got), (long)PEER_ADDR, "decm4: GETP peer addr");

    /* SHUT via the decoder. */
    how = SHUT_WR;
    pl.func = "SHUT"; pl.arg[0] = &snum; pl.arg[1] = &how;
    er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETOK, "decm4: SHUT -> RETCODE 0");

    /* GOPT SO_RCVBUF via the decoder. */
    { INT optname = NSF_SO_RCVBUF; val = 0; optlen = (INT)sizeof(val);
      pl.func = "GOPT"; pl.arg[0] = &snum; pl.arg[1] = &optname;
      pl.arg[2] = &val; pl.arg[3] = &optlen;
      er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
      CHECK_EQ((long)rcv, (long)NSF_RETOK, "decm4: GOPT -> RETCODE 0");
      CHECK_EQ((long)val, (long)NSF_SO_DFLT_BUF, "decm4: GOPT SO_RCVBUF value"); }

    /* LIST + ACCE via the decoder (a fresh listener). */
    af = NSF_AF_INET; type = NSF_SOCK_STREAM; proto = 0;
    pl.func = "SOCK"; pl.arg[0] = &af; pl.arg[1] = &type; pl.arg[2] = &proto;
    (void)nsf_ezasoh03(&pl);
    lnum = (USHORT)rcv;
    backlog = 4;
    pl.func = "LIST"; pl.arg[0] = &lnum; pl.arg[1] = &backlog;
    er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETOK, "decm4: LIST -> RETCODE 0");
    pl.func = "ACCE"; pl.arg[0] = &lnum; pl.arg[1] = &got;
    er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
    CHECK(rcv >= 0, "decm4: ACCE -> a new socket number");

    /* SELE via the decoder -- the one EZASOH03 path with real marshaling
     * (copy send-masks into return-masks, run in place, clear ERETMSK). Socket 0
     * (== snum, connected above) is made read-ready, so the poll (0/0) returns
     * immediately: RETCODE 1, RRETMSK bit 0 set, WRETMSK/ERETMSK cleared. The
     * 8-slot plist is MAXSOC, TIMEOUT(2 fullwords), RSND/WSND/ESND, RRET/WRET/ERET. */
    {
        INT   maxsoc = 1;
        INT   tv[2];
        UCHAR rsnd[4], wsnd[4], esnd[4], rret[4], wret[4], eret[4];

        memset(g_ready, 0, sizeof(g_ready));
        g_ready[0] = (int)SEL_READ;             /* SOCKCB id 0 == socket number 0 */
        tv[0] = 0; tv[1] = 0;                   /* poll (0/0)                     */
        memset(rsnd, 0, 4); memset(wsnd, 0, 4); memset(esnd, 0, 4);
        memset(rret, 0xFF, 4); memset(wret, 0xFF, 4); memset(eret, 0xFF, 4); /* dirty */
        rsnd[3] = 0x01u;                        /* interest: read on number 0     */
        pl.func = "SELE";
        pl.arg[0] = &maxsoc; pl.arg[1] = tv;
        pl.arg[2] = rsnd; pl.arg[3] = wsnd; pl.arg[4] = esnd;
        pl.arg[5] = rret; pl.arg[6] = wret; pl.arg[7] = eret;
        er = -1; rcv = -1; (void)nsf_ezasoh03(&pl);
        CHECK_EQ((long)rcv, 1L, "decm4: SELE -> RETCODE 1 (socket 0 ready)");
        CHECK(rret[0] == 0x00u && rret[1] == 0x00u && rret[2] == 0x00u &&
              rret[3] == 0x01u, "decm4: SELE RRETMSK = 0x00000001 (number 0)");
        CHECK(wret[0] == 0x00u && wret[1] == 0x00u && wret[2] == 0x00u &&
              wret[3] == 0x00u, "decm4: SELE WRETMSK cleared");
        CHECK(eret[0] == 0x00u && eret[1] == 0x00u && eret[2] == 0x00u &&
              eret[3] == 0x00u, "decm4: SELE ERETMSK always zero (no TAKESOCKET)");
        memset(g_ready, 0, sizeof(g_ready));
    }

    (void)nsf_termapi();
}

int main(void)
{
    pthread_t exec;

    printf("=== nsf370 NSFEZA (EZASOKET API) tests ===\n");

    sts_init();
    mm_init(NULL);
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    CHECK(nsfevt_init() == 0, "nsfevt_init (EVT pool)");
    mm_init_complete();
    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup");

    nsftmr_init();
    soc_init();
    nsfreq_init();
    nsfsel_init();                          /* registers the SELECT engine (M4-5) */
    CHECK_EQ((long)nsfreq_register_proto((UCHAR)TEST_PROTO, &dummy_ops), 0L,
             "register dummy protocol at 17");
    CHECK_EQ((long)nsfreq_register_proto(6u, &dummy_ops), 0L,
             "register dummy protocol at 6 (STREAM verbs)");
    nsfeza_init();
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);

    pthread_create(&exec, NULL, exec_thread, NULL);

    test_lifecycle();
    test_implicit_init();
    test_maxsoc();
    test_termapi_teardown();
    test_roundtrip();
    test_decoder();
    test_m4_verbs();
    test_fionbio();
    test_sockopt();
    test_select_masks();
    test_decoder_m4();

    nsfevt_stop();
    pthread_join(exec, NULL);

    /* Final leak gate: everything returned to baseline. */
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open");
    CHECK_EQ((long)sock_inuse(), 0L, "SOCKET pool fully returned");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "EVT pool fully returned");

    mm_shutdown();
    return mbt_test_summary("TSTEZA");
}
