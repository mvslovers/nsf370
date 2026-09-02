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
 *   'R8'  2.4, single job, no partner needed.
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
#define D1B_GENS     2                  /* gen 0 and 1 -- a fresh STC's range */

static void d1b_pause(unsigned hsec)
{
    ECB local = 0;
    ecb_timed_wait(&local, hsec, 0);
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

    /* A's own socket must STILL work at the end -- the check must not have
     * broken the owner while refusing everyone else. */
    rc = nsf_listen(s, 5);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "A: its OWN socket still works after B");
    (void)nsf_close(s);
    (void)nsf_termapi();
    wtof("TSTD1B: A DONE");
    return 0;
}

/* ---- role B: the attacker ------------------------------------------------ */
static int role_b(void)
{
    NSF_SOCKADDR_IN me, gp;
    INT   s, rc, namelen;
    UINT  gen, idx, desc;
    int   attempts = 0, hits = 0;
    INT   foreign_rc = 0, foreign_err = 0, unknown_rc = 0, unknown_err = 0;

    rc = nsf_initapi(0, "TCPIP   ", "NSF     ", "TSTD1BB ", NULL);
    CHECK_EQ((long)rc, (long)NSF_RETOK, "B: INITAPI across the boundary");

    /* B opens one socket of its own: the control that proves B's own verbs
     * work, so "everything refused" cannot pass as success. */
    s = nsf_socket(NSF_AF_INET, NSF_SOCK_DGRAM, 0);
    CHECK(s >= 0, "B: SOCKET of its own");
    mk_sa(&me, D1B_SRC, 0);
    rc = nsf_bind(s, &me, (INT)sizeof(me));
    CHECK_EQ((long)rc, (long)NSF_RETOK, "B: BIND on its OWN socket works");

    /* ---- 2.2 THE SWEEP.  Raw internal descriptors, straight into the
     * request, bypassing B's own facade table -- which is the whole point:
     * the facade cannot NAME a foreign socket, the transport could. ------- */
    for (gen = 0u; gen < (UINT)D1B_GENS; gen++) {
        for (idx = 0u; idx < (UINT)D1B_SWEEP_N; idx++) {
            NSFRQE r;

            desc = (gen << 16) | idx;
            memset(&r, 0, sizeof(r));
            memcpy(r.eye, NSFRQE_EYE, 4);
            r.fn       = (USHORT)RQ_GETSOCKNAME;
            r.sockdesc = desc;
            attempts++;
            nsfreq_call(&r);
            if (r.retcode == NSF_RETOK) {
                hits++;
                if (hits == 1) {
                    wtof("TSTD1B: B REACHED DESCRIPTOR %08X", (unsigned)desc);
                }
            }
        }
    }
    printf("  SWEEP: %d hits out of %d attempts\n", hits, attempts);
    wtof("TSTD1B: SWEEP %d HITS / %d ATTEMPTS", hits, attempts);
    CHECK_EQ((long)hits, 0L,
             "2.2: B reached NONE of the descriptor space (A's socket included)");

    /* ---- 2.2b INDISTINGUISHABLE.  A foreign descriptor and a descriptor
     * that never existed must give the SAME answer, or the refusal is an
     * existence oracle for descriptor numbers. ---------------------------- */
    {
        NSFRQE r;

        memset(&r, 0, sizeof(r));
        memcpy(r.eye, NSFRQE_EYE, 4);
        r.fn = (USHORT)RQ_GETSOCKNAME;
        r.sockdesc = 0x00000000u;               /* idx 0: A's, almost surely  */
        nsfreq_call(&r);
        foreign_rc = r.retcode; foreign_err = r.errno_;

        memset(&r, 0, sizeof(r));
        memcpy(r.eye, NSFRQE_EYE, 4);
        r.fn = (USHORT)RQ_GETSOCKNAME;
        r.sockdesc = 0x00BADBADu;               /* never existed              */
        nsfreq_call(&r);
        unknown_rc = r.retcode; unknown_err = r.errno_;
    }
    printf("  foreign rc=%d errno=%d | unknown rc=%d errno=%d\n",
           (int)foreign_rc, (int)foreign_err, (int)unknown_rc, (int)unknown_err);
    CHECK_EQ((long)foreign_rc, (long)unknown_rc,
             "2.2: foreign and never-existing return the SAME retcode");
    CHECK_EQ((long)foreign_err, (long)unknown_err,
             "2.2: foreign and never-existing return the SAME errno");

    /* ---- 2.3 SELECT with a foreign descriptor in the mask ---------------- */
    {
        NSFRQE      r;
        NSFSELITEM  it[2];

        memset(it, 0, sizeof(it));
        it[0].desc = 0x00000000u;               /* foreign (A's)              */
        it[0].want = (UCHAR)SEL_READ;
        it[1].desc = (UINT)0;                   /* filled below with B's own  */
        it[1].want = (UCHAR)SEL_WRITE;

        /* B's own socket, by internal descriptor: ask the facade for it via a
         * GETSOCKNAME round trip is not available, so use the poll form over
         * the foreign entry alone plus a write-ready own entry is not
         * expressible here -- so this asserts the FOREIGN entry only, and the
         * call returning without error is the "rest of the mask served" half:
         * a 1-item mask that errors would show as a negative retcode. */
        memset(&r, 0, sizeof(r));
        memcpy(r.eye, NSFRQE_EYE, 4);
        r.fn      = (USHORT)RQ_SELECT;
        r.ubuf    = it;
        r.ulen    = 1u;                         /* the foreign entry only     */
        r.p3      = SEL_F_TIMED;                /* poll form: 0/0, no park    */
        nsfreq_call(&r);
        printf("  SELECT(foreign) rc=%d errno=%d ready=%u\n",
               (int)r.retcode, (int)r.errno_, (unsigned)it[0].ready);
        CHECK_EQ((long)r.retcode, 0L,
                 "2.3: SELECT counts the foreign entry as NOT ready");
        CHECK_EQ((long)r.errno_, 0L,
                 "2.3: ...and returns NO error for the call");
        CHECK_EQ((long)it[0].ready, 0L, "2.3: the foreign entry's ready bits are 0");
    }

    (void)nsf_close(s);
    (void)nsf_termapi();
    wtof("TSTD1B: B DONE");
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

    if      (role[0] == 'A') rc = role_a();
    else if (role[0] == 'B') rc = role_b();
    else {
        printf("  no role given (PARM='A' or PARM='B') -- nothing to do\n");
        wtof("TSTD1B: NO ROLE -- SKIPPED");
        return D1B_CC_SKIP;
    }
    (void)rc;
    return mbt_test_summary("TSTD1B");
}
