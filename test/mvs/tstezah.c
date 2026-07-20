/*
 * tstezah.c -- NSFEZA EZASOH03 asm-veneer Stage-0 probe ON MVS (M3-4).
 * host = false. The ONE seam this whole facade design rests on is the asm ->
 * C linkage in asm/ezasoh03.asm: a hand-written socket-API entry that calls a
 * cc370 C function (nsf_ezasoh03/@@NSOH03). It is host-invisible (the issue-#8
 * S0C6-on-the-NEXT-call class), so it is proved HERE in isolation before any
 * reliance -- the "isolated probe first, pinned result" convention.
 *
 * The veneer uses the PROVEN cc370 pattern (PDPPRLG, modelled on libc370's
 * dyn75 socket() entry), which allocates a PER-INVOCATION DSA off each caller's
 * C stack -- so a corrupt save chain would show up (a) on the SECOND of two
 * consecutive calls and (b) under two concurrent subtasks. Both are provoked.
 *
 * EZASOH03 is called straight from C: the EZASOH03 plist IS a cc370 pointer
 * argument list, so `EZASOH03(&func, &errno, &retcode, &p0, ...)` lays down
 * exactly the plist the EZASMI macro builds (+0 A(func), +4 A(ERRNO), +8
 * A(RETCODE), +12.. A(params)); cc370 marshals R1 -> plist, the asm veneer
 * hands R1 to the C decoder, and R15 (the return) must ALWAYS be 0.
 *
 *   Phase A (isolated linkage, no stack): two cthread subtasks each call
 *     EZASOH03("ZZZZ") 50x. Every call must return R15=0, store RETCODE=-1 and
 *     ERRNO=45 (EOPNOTSUPP) through the plist +8/+4 slots, and never abend.
 *   Phase B (functional marshalling through the veneer): the executive loop +
 *     a dummy PROTOPS at proto 17; one app subtask drives INIT -> SOCK -> BIND
 *     -> GETS -> CLOS -> TERM entirely THROUGH EZASOH03, proving the plist
 *     function-specific parameters marshal (S halfword, AF/TYPE/PROTO, NAME).
 *
 * Diagnosing (read build/test-runner.spool): CC 0 = the seam holds. An S0C6 /
 * wild branch on the SECOND consecutive call, or under the two subtasks, = the
 * FUNHEAD/PDPPRLG save chain is miswired (Mike's predicted failure mode).
 */
#include "nsfeza.h"
#include "nsfreq.h"
#include "nsfsoc.h"
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

#define DUMMY_PROTO   17u          /* where SOCKET(...,DGRAM,0) lands           */
#define PROBE_ADDR    0xC0A8C801u  /* 192.168.200.1 */
#define PROBE_PORT    7777u
#define PROBE_ITERS   50

/* The asm veneer, called with the EZASOH03 plist laid out as a cc370 arg list. */
extern INT EZASOH03(void *funcp, void *errnop, void *retcodep,
                    void *a0, void *a1, void *a2, void *a3, void *a4)
                    asm("EZASOH03");

/* ---- test-only dummy protocol (Phase B needs SOCKET/BIND/CLOSE to complete) - */
static int d_attach(SOCKCB *s)             { (void)s; return 0; }
static int d_bind(SOCKCB *s)               { (void)s; return 0; }
static int d_connect(SOCKCB *s, NSFRQE *r) { (void)s; (void)r; return 0; }
static int d_listen(SOCKCB *s, int bl)     { (void)s; (void)bl; return 0; }
static int d_send(SOCKCB *s, NSFRQE *r)    { (void)s; (void)r; return 0; }
static int d_recv(SOCKCB *s, NSFRQE *r)    { (void)s; (void)r; return 0; }
static int d_close(SOCKCB *s, NSFRQE *r)   { (void)s; (void)r; return 0; }
static int d_detach(SOCKCB *s)             { (void)s; return 0; }

static PROTOPS dummy_ops = {
    .attach = d_attach, .bind = d_bind, .connect = d_connect, .listen = d_listen,
    .send = d_send, .recv = d_recv, .close = d_close, .detach = d_detach
};

/* ---- Phase A: two subtasks hammer the unknown-code path -------------------- */
static volatile int g_a_calls[2];
static volatile int g_a_bad_r15[2];
static volatile int g_a_bad_rc[2];
static volatile int g_a_bad_er[2];

static int probe_sub(void *arg)
{
    int slot = (int)(long)arg;   /* 0 or 1 */
    int i;

    for (i = 0; i < PROBE_ITERS; i++) {
        INT er  = 0x5A5A5A5A;    /* sentinels: must be overwritten by the store */
        INT rcv = 0x5A5A5A5A;
        INT r15;

        r15 = EZASOH03((void *)"ZZZZ", &er, &rcv, (void *)0, (void *)0,
                       (void *)0, (void *)0, (void *)0);
        g_a_calls[slot]++;
        if (r15 != 0)               { g_a_bad_r15[slot]++; }
        if (rcv != NSF_RETERR)      { g_a_bad_rc[slot]++;  }
        if (er  != NSF_EOPNOTSUPP)  { g_a_bad_er[slot]++;  }
    }
    return 0;
}

/* ---- Phase B: a full lifecycle THROUGH the veneer -------------------------- */
static INT g_b_init_rc, g_b_sock_rc, g_b_bind_rc, g_b_gets_rc, g_b_clos_rc;
static INT g_b_maxsno, g_b_r15_ok;
static UINT g_b_gname_addr, g_b_gname_port;

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

static int lifecycle_sub(void *arg)
{
    NSF_SOCKADDR_IN sa;
    INT    er, rcv, r15;
    INT    maxsoc, af, type, proto;
    USHORT snum;
    char   ident[16];
    char   subtask[8];

    (void)arg;
    g_b_r15_ok = 1;

    /* INIT */
    memset(ident, ' ', sizeof(ident));
    memset(subtask, ' ', sizeof(subtask));
    maxsoc = 0; g_b_maxsno = -1; er = 0; rcv = -9;
    r15 = EZASOH03((void *)"INIT", &er, &rcv, &maxsoc, ident, subtask,
                   &g_b_maxsno, (void *)0);
    if (r15 != 0) { g_b_r15_ok = 0; }
    g_b_init_rc = rcv;

    /* SOCK -> a descriptor in RETCODE */
    af = NSF_AF_INET; type = NSF_SOCK_DGRAM; proto = 0; er = -1; rcv = -9;
    r15 = EZASOH03((void *)"SOCK", &er, &rcv, &af, &type, &proto,
                   (void *)0, (void *)0);
    if (r15 != 0) { g_b_r15_ok = 0; }
    g_b_sock_rc = rcv;
    snum = (USHORT)rcv;

    /* BIND (S is a halfword; NAME the sockaddr) */
    mk_sa(&sa, PROBE_ADDR, PROBE_PORT);
    er = -1; rcv = -9;
    r15 = EZASOH03((void *)"BIND", &er, &rcv, &snum, &sa, (void *)0,
                   (void *)0, (void *)0);
    if (r15 != 0) { g_b_r15_ok = 0; }
    g_b_bind_rc = rcv;

    /* GETS returns the bound name */
    memset(&sa, 0, sizeof(sa));
    er = -1; rcv = -9;
    r15 = EZASOH03((void *)"GETS", &er, &rcv, &snum, &sa, (void *)0,
                   (void *)0, (void *)0);
    if (r15 != 0) { g_b_r15_ok = 0; }
    g_b_gets_rc    = rcv;
    g_b_gname_addr = sa_addr(&sa);
    g_b_gname_port = (UINT)sa_port(&sa);

    /* CLOS */
    er = -1; rcv = -9;
    r15 = EZASOH03((void *)"CLOS", &er, &rcv, &snum, (void *)0, (void *)0,
                   (void *)0, (void *)0);
    if (r15 != 0) { g_b_r15_ok = 0; }
    g_b_clos_rc = rcv;

    /* TERM (no RETCODE/ERRNO) */
    (void)EZASOH03((void *)"TERM", (void *)0, (void *)0, (void *)0, (void *)0,
                   (void *)0, (void *)0, (void *)0);

    nsfevt_stop();
    return 0;
}

int main(void)
{
    NSFTHR *pa, *pb, *lc;
    int     total_bad;

    printf("=== nsf370 NSFEZA EZASOH03 asm-veneer probe (MVS) ===\n");

    nsftrc_init();
    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0) { printf("nsfevt_init failed\n"); return 1; }
    CHECK(buf_init() == 0, "buf_init");
    CHECK(soc_reserve(0) == 0, "soc_reserve (SOCKET pool)");
    mm_init_complete();

    soc_init();
    nsfreq_init();
    nsfeza_init();
    CHECK_EQ((long)nsfreq_register_proto((UCHAR)DUMMY_PROTO, &dummy_ops), 0L,
             "register dummy protocol");
    CHECK_EQ((long)nsfthr_setup(), 0, "nsfthr_setup (IDENTIFY CTHREAD)");

    /* -- Phase A: two subtasks hammer the asm->C seam (no executive needed) -- */
    pa = nsfthr_create(probe_sub, (void *)0);
    pb = nsfthr_create(probe_sub, (void *)1);
    CHECK(pa != NULL && pb != NULL, "two probe subtasks ATTACHed");
    CHECK_EQ((long)nsfthr_join(pa, 30u), 0, "probe subtask 0 joined");
    CHECK_EQ((long)nsfthr_join(pb, 30u), 0, "probe subtask 1 joined");

    total_bad = g_a_bad_r15[0] + g_a_bad_r15[1] + g_a_bad_rc[0] + g_a_bad_rc[1]
              + g_a_bad_er[0]  + g_a_bad_er[1];
    printf("Phase A: calls=%d/%d  bad_r15=%d bad_rc=%d bad_errno=%d\n",
           g_a_calls[0], g_a_calls[1],
           g_a_bad_r15[0] + g_a_bad_r15[1],
           g_a_bad_rc[0]  + g_a_bad_rc[1],
           g_a_bad_er[0]  + g_a_bad_er[1]);
    CHECK_EQ((long)(g_a_calls[0] + g_a_calls[1]), (long)(2 * PROBE_ITERS),
             "Phase A: every EZASOH03 call returned (no abend/hang)");
    CHECK_EQ((long)total_bad, 0L,
             "Phase A: R15=0 + RETCODE=-1 + ERRNO=45 on every call, both subtasks");

    /* -- Phase B: a full lifecycle through the veneer (needs the executive) -- */
    evt_set_request(nsfreq_ecb(), nsfreq_drain, nsfreq_pending);
    lc = nsfthr_create(lifecycle_sub, (void *)0);
    CHECK(lc != NULL, "lifecycle subtask ATTACHed");
    nsftmr_plat_arm(1u);
    evt_mainloop();
    CHECK_EQ((long)nsfthr_join(lc, 30u), 0, "lifecycle subtask joined");

    printf("Phase B: INIT rc=%d maxsno=%d SOCK rc=%d BIND rc=%d GETS rc=%d CLOS rc=%d\n",
           (int)g_b_init_rc, (int)g_b_maxsno, (int)g_b_sock_rc,
           (int)g_b_bind_rc, (int)g_b_gets_rc, (int)g_b_clos_rc);
    CHECK(g_b_r15_ok == 1, "Phase B: EZASOH03 returned R15=0 on every call");
    CHECK_EQ((long)g_b_init_rc, (long)NSF_RETOK, "veneer INIT -> RETCODE 0");
    CHECK_EQ((long)g_b_maxsno, (long)(NSFEZA_MAXSOC - 1), "veneer INIT stored MAXSNO");
    CHECK(g_b_sock_rc >= 0, "veneer SOCK -> a descriptor in RETCODE");
    CHECK_EQ((long)g_b_bind_rc, (long)NSF_RETOK, "veneer BIND -> RETCODE 0");
    CHECK_EQ((long)g_b_gets_rc, (long)NSF_RETOK, "veneer GETS -> RETCODE 0");
    CHECK_EQ((long)g_b_gname_addr, (long)PROBE_ADDR, "veneer GETS returned the bound addr");
    CHECK_EQ((long)g_b_gname_port, (long)PROBE_PORT, "veneer GETS returned the bound port");
    CHECK_EQ((long)g_b_clos_rc, (long)NSF_RETOK, "veneer CLOS -> RETCODE 0");
    CHECK_EQ((long)soc_count(), 0L, "no sockets left after veneer TERM (leak gate)");

    mm_shutdown();
    return mbt_test_summary("TSTEZAH");
}
