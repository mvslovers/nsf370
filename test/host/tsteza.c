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
#include "nsfevt.h"
#include "nsfthr.h"
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
static int d_attach(SOCKCB *s)             { (void)s; return 0; }
static int d_bind(SOCKCB *s)               { (void)s; return 0; }
static int d_connect(SOCKCB *s, NSFRQE *r) { (void)s; (void)r; return 0; }
static int d_listen(SOCKCB *s, int bl)     { (void)s; (void)bl; return 0; }
static int d_close(SOCKCB *s, NSFRQE *r)   { (void)s; (void)r; return 0; }
static int d_detach(SOCKCB *s)             { (void)s; return 0; }

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
    d_attach, d_bind, d_connect, d_listen, d_send, d_recv, d_close, d_detach
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

    /* an unimplemented-but-known code (CONNECT) -> same clean EOPNOTSUPP */
    er = 0; rcv = 0;
    pl.func = "CONN";
    (void)nsf_ezasoh03(&pl);
    CHECK_EQ((long)rcv, (long)NSF_RETERR, "CONN (unsupported) -> RETCODE -1");
    CHECK_EQ((long)er, (long)NSF_EOPNOTSUPP, "CONN (unsupported) -> ERRNO EOPNOTSUPP");

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

    soc_init();
    nsfreq_init();
    CHECK_EQ((long)nsfreq_register_proto((UCHAR)TEST_PROTO, &dummy_ops), 0L,
             "register dummy protocol at 17");
    nsfeza_init();
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);

    pthread_create(&exec, NULL, exec_thread, NULL);

    test_lifecycle();
    test_implicit_init();
    test_maxsoc();
    test_termapi_teardown();
    test_roundtrip();
    test_decoder();

    nsfevt_stop();
    pthread_join(exec, NULL);

    /* Final leak gate: everything returned to baseline. */
    CHECK_EQ((long)soc_count(), 0L, "no sockets left open");
    CHECK_EQ((long)sock_inuse(), 0L, "SOCKET pool fully returned");
    CHECK_EQ((long)nsfevt_inuse(), 0L, "EVT pool fully returned");

    mm_shutdown();
    return mbt_test_summary("TSTEZA");
}
