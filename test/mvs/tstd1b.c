/*----------------------------------------------------------------------------
 * tstd1b.c -- M5-2d1 live gates 2.2/2.3/2.4.  MVS-only.  PARM selects a role.
 *
 * Runs only AFTER gate 2.1 (TSTD1A, the cross-AS accept) is green: a check that
 * refuses accepted children makes everything here moot.
 *
 * ROLES
 *   'A'   hold a socket open in address space A and announce it, so B has
 *         something real to aim at.  Ends by itself.
 *   'B'   the attacker, in a DIFFERENT address space:
 *           2.2  SWEEP every descriptor (gen<<16)|idx and try a verb on each.
 *                Descriptors are not merely forgeable but GUESSABLE -- one
 *                socket table, lowest free index, idx in 0..63 -- so the sweep
 *                IS the demonstration, not an assertion about it.
 *           2.3  SELECT with a foreign descriptor in the mask.
 *   'W'   #101 arm 3, the WEDGE: park a block-forever cross-AS SELECT.
 *   'V'   #101 arm 3, the VICTIM: an ordinary request while W is parked.
 *
 * Gate 2.4 (the R8/TPROT validation) is NOT here -- it is its own single-job
 * test, TSTD1R. An earlier version of this header advertised an 'R8' role that
 * has no function and no dispatch in this file.
 *
 * WHY THE SWEEP REPORTS ATTEMPTS AS WELL AS HITS: a sweep that finds nothing
 * looks exactly like a sweep run against an NSFS where A never opened a socket.
 * "0 hits in 128 attempts, with A confirmed holding a socket" is evidence; "0
 * hits" alone is a null nobody can read.  A's presence is established from the
 * console before B is launched, and B prints both numbers.
 *
 * Return codes: 0 passed, 1 ran and failed, 20 could not run.
 *--------------------------------------------------------------------------*/
#include "nsfeza.h"
#include "nsfsoc.h"
#include "nsfreq.h"
#include "nsfreqc.h"
#include "nsfsel.h"     /* NSFSELITEM, SEL_F_TIMED                            */
#include <mbtcheck.h>
#include <clibos.h>
#include <clibecb.h>
#include <stdio.h>
#include <string.h>

#define D1B_SRC      0xC0A8C801u
#define D1B_A_PORT   3011u
#define D1B_HOLD_HS  6000u              /* A holds ~60 s                      */
#define D1B_CC_SKIP  20
#define D1B_SWEEP_N  64                 /* the whole socket table             */
#define D1B_W_PORT   3013u              /* arm 3: the parker's listener       */
#define D1B_GENS     2                  /* gen 0 and 1 -- a fresh STC's range */

static void d1b_pause(unsigned hsec)
{
    ECB local = 0;
    ecb_timed_wait(&local, hsec, 0);
}

/* Facade masks are numbered right-to-left, byte-wise; on a big-endian target a
 * UINT 1u<<n in memory matches that read exactly (byte 3 = the LSB end). Same
 * reasoning as tstezat.c mask_of; this file is target-only. */
static UINT mask_of(INT n)
{
    return (n >= 0 && n < 32) ? (1u << (UINT)n) : 0u;
}

static void mk_sa(NSF_SOCKADDR_IN *sa, UINT addr, USHORT port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = NSF_AF_INET;
    sa->sin_port   = port;
    sa->sin_addr   = addr;
}

/* ---- role A: hold a socket so B has a real target ------------------------ */
static int role_a(void)
{
    NSF_SOCKADDR_IN local;
    INT s, rc;

    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTD1BA ", NULL);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "A: INITAPI across the boundary");

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    CHECK(s >= 0, "A: SOCKET across the boundary");
    mk_sa(&local, D1B_SRC, (USHORT)D1B_A_PORT);
    rc = nsf_bind(s, &local, (INT)sizeof(local));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "A: BIND");
    rc = nsf_listen(s, 5);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "A: LISTEN");

    /* The facade number is A's; what B has to guess is the INTERNAL descriptor,
     * which A does not see. Announce readiness, not the secret. */
    wtof("TSTD1B: A HOLDING SOCKET (facade %d) -- B MAY RUN NOW", (int)s);
    printf("  A holding facade socket %d for ~60 s\n", (int)s);

    d1b_pause(D1B_HOLD_HS);

    /* A's own socket must still be A's, and still LISTENING, at the end -- the
     * check must not have broken the owner while refusing everyone else.
     *
     * THE ASSERTION IS THE REFUSAL, AND THE ERRNO IS WHAT MAKES IT ONE.
     * tcp_listen returns NSF_EINVAL unless the TCB is TCP_CLOSED
     * (src/nsftcp.c:1959, "only a fresh socket may listen"), and there is no
     * close between the first listen and this one -- so a second LISTEN on a
     * listening socket can only ever be refused.  The original assertion here
     * (rc == NSF_RETOK, "its OWN socket still works after B") was therefore
     * STRUCTURALLY ALWAYS-FALSE, and it stayed that way because A's CC appears
     * never to have been read: #100 ran B after A had ended, and the d1c record
     * quotes B's counts and not A's.  A job whose result nobody looks at is
     * CLAUDE.md 8.5 one level out -- so record A's result, not only B's.
     *
     * `rc == NSF_RETERR` ALONE WOULD NOT BE AN ASSERTION: it is green for any
     * failure at all, which is the absent-vs-succeeded shape moved inside the
     * check.  The two refusals are different statements in do_listen
     * (src/nsfreq.c):
     *
     *   EBADF  -- req_socket returned NULL: the descriptor did not resolve, or
     *             is not owned by this caller.  Foreign and never-existing are
     *             indistinguishable here BY CONSTRUCTION (ADR-0046), which is
     *             exactly why EBADF cannot carry this claim.
     *   EINVAL -- the descriptor RESOLVED, ownership HELD, the TCB exists and
     *             is not CLOSED.
     *
     * So EINVAL is the isolation statement in POSITIVE form: A's socket
     * survived B, still belongs to A, and is still listening.  EBADF here
     * would mean B had broken the owner -- the failure this gate exists for.
     *
     * The errno read path is verified, not assumed: nsf_listen puts r->errno_
     * into g_eza_errno UNREMAPPED (src/nsfeza.c:371) and nsf_lasterrno returns
     * it. */
    rc = nsf_listen(s, 5);
    CHECK_EQ((long)rc, (long)NSF_RETERR,
             "A: a second LISTEN on A's OWN socket is refused");
    CHECK_EQ((long)nsf_lasterrno(), (long)NSF_EINVAL,
             "A: refused EINVAL -- resolved, owned and still listening (not EBADF)");
    (void)nsf_close(s);
    (void)nsf_termapi();
    wtof("TSTD1B: A DONE");
    return 0;
}

/* ---- role B: the attacker ------------------------------------------------
 * REBUILT after the first live round, which found three defects in this role.
 * Each repair is named at the thing it repairs.
 * ------------------------------------------------------------------------ */

#define D1B_MAXHIT   8

static UINT g_hit[D1B_MAXHIT];
static int  g_hits;

/* One sweep of the whole descriptor space, recording WHICH descriptors were
 * reached.  DEFECT 1 + 2: the count was the wrong thing to assert -- it does
 * not distinguish B's own socket from A's, and it is not comparable across STC
 * instances because a generation range that does not cover the target returns
 * the same zero as a refusal.  The identity is what the claim is about. */
static int sweep(const char *tag)
{
    NSFRQE r;
    UINT   gen, idx, desc;
    int    attempts = 0;

    g_hits = 0;
    for (gen = 0u; gen < (UINT)D1B_GENS; gen++) {
        for (idx = 0u; idx < (UINT)D1B_SWEEP_N; idx++) {
            desc = (gen << 16) | idx;
            memset(&r, 0, sizeof(r));
            memcpy(r.eye, NSFRQE_EYE, 4);
            r.fn       = (USHORT)RQ_GETSOCKNAME;
            r.sockdesc = desc;
            attempts++;
            nsfreq_call(&r);
            if (r.retcode == NSF_RETOK) {
                if (g_hits < D1B_MAXHIT) g_hit[g_hits] = desc;
                g_hits++;
            }
        }
    }
    printf("  SWEEP %s: %d reached out of %d attempts", tag, g_hits, attempts);
    {
        int i;
        for (i = 0; i < g_hits && i < D1B_MAXHIT; i++)
            printf(" %08X", (unsigned)g_hit[i]);
    }
    printf("\n");
    wtof("TSTD1B: SWEEP %s %d REACHED (first %08X)", tag, g_hits,
         (unsigned)(g_hits > 0 ? g_hit[0] : 0u));
    return attempts;
}

static INT probe_desc(UINT desc, INT *errno_out)
{
    NSFRQE r;

    memset(&r, 0, sizeof(r));
    memcpy(r.eye, NSFRQE_EYE, 4);
    r.fn       = (USHORT)RQ_GETSOCKNAME;
    r.sockdesc = desc;
    nsfreq_call(&r);
    if (errno_out != NULL) *errno_out = r.errno_;
    return r.retcode;
}

static int role_b(void)
{
    NSF_SOCKADDR_IN me;
    INT   s, rc;
    UINT  own = 0u, a_desc = 0u;
    int   i, before, found_own = 0;
    INT   f_rc, f_err, u_rc, u_err;

    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTD1BB ", NULL);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "B: INITAPI across the boundary");

    /* ---- SWEEP 1, OWNING NOTHING.  DEFECT 1's repair: B has no socket yet,
     * so ANY descriptor it reaches here is foreign BY CONSTRUCTION and no
     * exclusion arithmetic is needed. -------------------------------------- */
    (void)sweep("pre-own");
    before = g_hits;
    for (i = 0; i < g_hits && i < D1B_MAXHIT; i++) a_desc = g_hit[i];

    /* ---- B's own socket: the control that B's verbs work at all. --------- */
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "B: SOCKET of its own");
    mk_sa(&me, D1B_SRC, 0);
    rc = nsf_bind(s, &me, (INT)sizeof(me));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "B: BIND on its OWN socket works");

    /* ---- SWEEP 2: THE RANGE'S POSITIVE CONTROL.  DEFECT 2's repair: B has
     * just created a socket, so the sweep MUST now reach at least one more
     * than before.  If it does not, the swept generation range does not cover
     * live descriptors and a zero from sweep 1 would have meant NOTHING --
     * so say so and skip, rather than report a zero. -------------------- */
    (void)sweep("post-own");
    found_own = (g_hits > before);
    CHECK(found_own,
          "B's OWN socket is inside the swept range (the range is adequate)");
    if (!found_own) {
        printf("  THE SWEPT RANGE DOES NOT COVER LIVE DESCRIPTORS -- sweep 1's"
               " result means nothing and is NOT reported as a refusal.\n");
        wtof("TSTD1B: RANGE INADEQUATE -- SWEEP 1 IS NOT EVIDENCE");
        (void)nsf_close(s);
        (void)nsf_termapi();
        return -1;
    }
    for (i = 0; i < g_hits && i < D1B_MAXHIT; i++) own = g_hit[i];

    /* ---- THE CLAIM, in identity form.  Every descriptor reached before B
     * owned anything was foreign; there must be none. ---------------------- */
    CHECK_EQ((long)before, 0L,
             "2.2: owning nothing, B reached NO descriptor (all foreign)");

    /* ---- A's REAL descriptor.  DEFECT 3's repair: never a constant.  A
     * allocated its socket immediately before B on the same table, so A's is
     * B's index minus one at the same generation -- and the revert arm
     * CONFIRMS the derivation by naming that exact value in sweep 1. ------- */
    a_desc = (own & 0xFFFF0000u) | ((own & 0xFFFFu) - 1u);
    printf("  B's own = %08X, so A's is derived as %08X\n",
           (unsigned)own, (unsigned)a_desc);
    wtof("TSTD1B: OWN %08X A-DERIVED %08X", (unsigned)own, (unsigned)a_desc);

    /* ---- 2.2b INDISTINGUISHABLE, against A's REAL descriptor ------------- */
    f_rc = probe_desc(a_desc, &f_err);
    u_rc = probe_desc(0x00BADBADu, &u_err);
    printf("  foreign(%08X) rc=%d errno=%d | unknown rc=%d errno=%d\n",
           (unsigned)a_desc, (int)f_rc, (int)f_err, (int)u_rc, (int)u_err);
    CHECK_EQ((long)f_rc, (long)u_rc,
             "2.2b: foreign and never-existing return the SAME retcode");
    CHECK_EQ((long)f_err, (long)u_err,
             "2.2b: foreign and never-existing return the SAME errno");

    /* ---- 2.3 SELECT, POLL PATH: A's real descriptor plus B's own --------- */
    {
        NSFRQE     r;
        NSFSELITEM it[2];

        memset(it, 0, sizeof(it));
        it[0].desc = a_desc;  it[0].want = (UCHAR)SEL_READ;
        it[1].desc = own;     it[1].want = (UCHAR)SEL_WRITE;

        memset(&r, 0, sizeof(r));
        memcpy(r.eye, NSFRQE_EYE, 4);
        r.fn = (USHORT)RQ_SELECT; r.ubuf = it;
        r.ulen = 2u * (UINT)sizeof(NSFSELITEM);   /* BYTES (ADR-0047) */
        r.p3 = SEL_F_TIMED;                     /* 0/0 poll form: never parks */
        nsfreq_call(&r);
        printf("  SELECT poll: rc=%d errno=%d foreign.ready=%u own.ready=%u\n",
               (int)r.retcode, (int)r.errno_,
               (unsigned)it[0].ready, (unsigned)it[1].ready);
        CHECK_EQ((long)it[0].ready, 0L, "2.3 poll: the foreign entry is NOT ready");
        CHECK(it[1].ready != 0, "2.3 poll: THE REST OF THE MASK IS SERVED");
        CHECK_EQ((long)r.errno_, 0L, "2.3 poll: no error for the call");
    }

    /* ---- 2.3 SELECT, PARKED PATH.  Never driven before this round.  B parks
     * on A's descriptor alone with a timeout; the host connects to A's port
     * meanwhile, which makes A's LISTENER read-ready.  Check on -> B times out
     * (rc 0).  Check off -> the re-scan completes B (rc 1).  THAT is what
     * discriminates, and it is the re-scan path specifically. -------------- */
    {
        NSFRQE     r;
        NSFSELITEM it[1];

        memset(it, 0, sizeof(it));
        it[0].desc = a_desc; it[0].want = (UCHAR)SEL_READ;

        wtof("TSTD1B: PARKING SELECT ON %08X -- CONNECT TO A NOW",
             (unsigned)a_desc);
        memset(&r, 0, sizeof(r));
        memcpy(r.eye, NSFRQE_EYE, 4);
        r.fn = (USHORT)RQ_SELECT; r.ubuf = it;
        r.ulen = 1u * (UINT)sizeof(NSFSELITEM);   /* BYTES (ADR-0047) */
        r.p1 = 8u;                              /* 8 s: it must PARK          */
        r.p3 = SEL_F_TIMED;
        nsfreq_call(&r);
        printf("  SELECT parked: rc=%d errno=%d ready=%u\n",
               (int)r.retcode, (int)r.errno_, (unsigned)it[0].ready);
        wtof("TSTD1B: PARKED SELECT RC=%d READY=%u",
             (int)r.retcode, (unsigned)it[0].ready);
        CHECK_EQ((long)r.retcode, 0L,
                 "2.3 parked: A becoming ready does NOT complete B's SELECT");
        CHECK_EQ((long)it[0].ready, 0L, "2.3 parked: ...and the entry stays not-ready");
    }

    (void)nsf_close(s);
    (void)nsf_termapi();
    wtof("TSTD1B: B DONE");
    return 0;
}

/* ---- ARM 3 (issue #101 Stage 2): the wedge ------------------------------
 *
 * WHAT THE FIX CHANGES HERE IS PERMANENCE, NOT THE WEDGE.  Read from source
 * before this arm was written: soc_complete is the only thing that posts
 * g_priv.ecb (nsfsoc.c), nsfsel_dispatch's PARK path calls no soc_complete,
 * and g_busy has exactly one clear (nsfsx.c) gated on that POSTED bit.  So a
 * parked block-forever SELECT holds g_busy on the FIXED module too -- that is
 * serialised service (ADR-0042 10), not the ulen defect.
 *
 * What the defect adds is that the wedge can never LIFT: nsfsel_on_notify
 * re-scans the STORED array (sel_scan(cb->items, cb->nitems, ...)), and with a
 * count-valued ulen those items are residue, so no readiness poke can ever
 * match and the parked SELECT is never completed.
 *
 * Hence the arm makes W's socket BECOME ready mid-run:
 *   FIXED    poke matches -> sel_finish -> soc_complete -> g_busy clears -> V served
 *   UNFIXED  items are residue -> poke never matches -> W parked forever -> V never served
 *
 * THE EXECUTIVE IS NOT HUNG EITHER WAY.  Only cross-AS request service stalls;
 * timers, devices and the console keep running, so `F NSFS,STATS` answering
 * during the window is the positive control that distinguishes "V was not
 * served" from "the STC died".  Both roles mark the console with wtof rather
 * than printf: a job that hangs loses its buffered SYSPRINT to the cancel
 * (the M4-5 lesson), and on the unfixed arm hanging is the expected outcome. */

/* Role W: park a BLOCK-FOREVER SELECT (tv_sec < 0 -> no SEL_F_TIMED) on its own
 * listener, through the FACADE, which is the path a real application takes. */
static int role_w(void)
{
    NSF_SOCKADDR_IN local;
    UINT  rmask;
    INT   s, rc;

    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTD1BW ", NULL);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "W: INITAPI across the boundary");

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    CHECK(s >= 0, "W: SOCKET across the boundary");
    mk_sa(&local, D1B_SRC, (USHORT)D1B_W_PORT);
    rc = nsf_bind(s, &local, (INT)sizeof(local));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "W: BIND");
    rc = nsf_listen(s, 5);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "W: LISTEN");

    rmask = mask_of(s);
    wtof("TSTD1B: W PARKING BLOCK-FOREVER SELECT ON PORT %u (facade %d)"
         " -- RUN V NOW", (unsigned)D1B_W_PORT, (int)s);
    printf("  W parking a block-forever SELECT on facade socket %d\n", (int)s);

    rc = nsf_select(s + 1, &rmask, NULL, NULL, -1, 0);   /* no timeout at all */

    /* Reached only if a readiness poke completed it. On the unfixed module this
     * line is never printed and the job must be cancelled -- which IS the
     * result, and why the marker above is a WTO. */
    wtof("TSTD1B: W SELECT COMPLETED RC=%d ERRNO=%d MASK=%08X",
         (int)rc, (int)nsf_lasterrno(), (unsigned)rmask);
    printf("  W SELECT completed rc=%d errno=%d mask=%08X\n",
           (int)rc, (int)nsf_lasterrno(), (unsigned)rmask);
    CHECK(rc > 0, "arm3 W: the readiness poke COMPLETED the parked SELECT");
    CHECK((rmask & mask_of(s)) != 0u, "arm3 W: ...and it named W's own socket");

    (void)nsf_close(s);
    (void)nsf_termapi();
    wtof("TSTD1B: W DONE");
    return 0;
}

/* Role V: the victim -- an ordinary cross-AS request issued while W is parked.
 * Served or not served is the whole observation, and both markers are WTOs. */
static int role_v(void)
{
    INT s, rc;

    wtof("TSTD1B: V REQUEST ISSUED -- WAITING TO BE SERVED");
    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTD1BV ", NULL);
    wtof("TSTD1B: V SERVED RC=%d", (int)rc);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "arm3 V: served while W was parked");

    s = nsf_socket(NSF_AF_INET, NSF_SOCK_STREAM, 0);
    CHECK(s >= 0, "arm3 V: and a second request is served too");
    if (s >= 0) (void)nsf_close(s);
    (void)nsf_termapi();
    wtof("TSTD1B: V DONE");
    return 0;
}

int main(int argc, char **argv)
{
    const char *role = (argc > 1 && argv[1] != NULL) ? argv[1] : "";
    int rc;

    printf("=== nsf370 M5-2d1 live gates 2.2/2.3 (TSTD1B role '%s') ===\n", role);
    wtof("TSTD1B: START ROLE '%s'", role);

    CHECK_EQ((long)__isauth(), 0L, "the client is UNAUTHORISED");

    if (nsfreqc_init() != 0) {
        printf("  NSFS not reachable -- is it started?\n");
        wtof("TSTD1B: NO NSFS -- SKIPPED");
        return D1B_CC_SKIP;
    }

    if      (role[0] == 'W') rc = role_w();      /* arm 3: the parker        */
    else if (role[0] == 'V') rc = role_v();      /* arm 3: the victim        */
    else if (role[0] == 'A') rc = role_a();
    else if (role[0] == 'B') rc = role_b();
    else {
        printf("  no role given (PARM='A'/'B'/'W'/'V') -- nothing to do\n");
        wtof("TSTD1B: NO ROLE -- SKIPPED");
        return D1B_CC_SKIP;
    }
    (void)rc;
    return mbt_test_summary("TSTD1B");
}
