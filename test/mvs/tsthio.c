/*
 * tsthio.c -- M3-0a Stage 0 (issue TBD, feeds a future ADR-0027): can a
 * PROBLEM-STATE program halt an outstanding CTCI READ? host = false.
 *
 * This is a standalone, single-task diagnostic probe against the real
 * 500/501 pair -- NOT the M1-4b subtask/DEVOPS driver. It reuses the M1-3
 * raw primitives AS-IS (ctci_alloc_unit / ctci_open_sub / ctci_read /
 * ctci_status / ctci_close_sub, from src/nsfctci.c + asm/nsfctcio.asm) and
 * adds zero production surface: the two SVC-issuing entries this probe
 * needs (neither exists anywhere in the codebase, per a grep of nsf370,
 * libc370, ufsd and mvslovers/mvs38j-ip) live in test/asm/tsthalt.asm, not
 * asm/. No changes to src/ or asm/.
 *
 * RUNBOOK: the NSF STC must be STOPPED first (P NSF) so the devices are
 * free. Build TWICE, one probe per run (MBT only honours [build].cflags,
 * not per-test cflags -- add the -D temporarily, revert before commit):
 *   1. default build                       -> the IOHALT (SVC 33) probe.
 *   2. [build].cflags += "-DTSTHIO_PURGE"   -> the PURGE (SVC 16) probe.
 * Each run needs a QUIET link (no ping) for its first ~1s (the "READ has
 * NOT completed" assertion), then ONE host ping during its bounded ~60s
 * re-arm wait (the runbook performs it -- see CLAUDE.md 5, "background
 * pings on mvsdev need setsid + tcpdump verification"). Running both
 * probes in one job/one ping schedule risks the second probe's quiet
 * window overlapping the first probe's re-arm ping; two clean runs avoid
 * that entirely.
 *
 * PRIMARY SOURCES (read directly, not from memory -- CLAUDE.md 5):
 *
 *   IOHALT macro -- SYS1.MACLIB(IOHALT) on mvsdev (read live, verbatim).
 *   The macro's no-OFFSET (HALTCD) expansion: R0 = UCB (common segment)
 *   address, R1 = X'00000001' (low bit set -- "USE IOS HALT I/O ROUTINE"),
 *   SVC 33. No OFFSET operand means the EXCP-modify path (R1 = X'81......')
 *   is NOT taken. The macro's own header comment documents no
 *   authorization restriction and no return code.
 *
 *   PURGE macro -- OS/VS2 System Programming Library: Data Management,
 *   GC26-3830-4 (Release 3.8, Oct 1981), "PURGE -- Halt or Finish
 *   I/O-Request Processing" (pp. 120-123). SVC 16. A 12- or 16-byte
 *   parameter list; byte 0 bit 7 ("this is a 16-byte parameter list")
 *   is the ONLY way to get a real return code -- with it off, R15 is
 *   forced to zero regardless of outcome. Return codes: 0 success; 4 "TCB
 *   was not honored... as it must when the requestor is in problem
 *   state" (i.e. problem state may ONLY purge its own job-step TCB); 8
 *   address-space-scope purge without supervisor state, or dataset-scope
 *   purge with no DEB address; 14 preempted, retry. This probe uses
 *   TCB-scope (byte 0 bit 6, TCB address = 0 = "the one you're running
 *   under") specifically because it is the one documented as available
 *   from problem state with no DEB lookup required.
 *
 *   Figure 13 ("Event Control Block After Posting of Completion Code
 *   (EXCP)"), same manual, p.76: completion code 7F000000 = normal;
 *   48000000 = "the request queue element for a channel program has been
 *   made available after it has been purged" -- and the CSW (hence the
 *   residual count) "does not apply" on that code. CONFIRMED LIVE (this
 *   probe, both mechanisms): the post code, not the residual/xferred
 *   count, is the discriminator -- IOHALT and PURGE BOTH surface post=48,
 *   not the raw hardware CE+DE/full-residual signature a literal reading
 *   of Hercules ctc_ctci.c's halt_or_clear would predict; IOS reclassifies
 *   the completion the same way for both problem-state halt mechanisms.
 *
 *   DCB -> DEB -> UCB pointer chase (needed only for the IOHALT probe;
 *   PURGE's TCB-scope form needs no DEB/UCB lookup at all): OS/VS2 System
 *   Programming Library: Debugging Handbook Volume 2, GC28-0709-1 (Dec
 *   1978) -- "DEB Basic Section" (DCB+44 = DCBDEBAD, a fullword whose
 *   byte 0 is DCBIFLGS and bytes 1-3 are DCBDEBA, the clean DEB address;
 *   confirmed independently in SYS1.MACLIB(IHADCB) on mvsdev) and the
 *   "Unit Record, Magnetic Tape, Telecommunications Devices Section"
 *   (DEB+32 = DEBSUCBA, a fullword whose byte 0 is DEBSDVM -- a device
 *   modifier, unused for unit-record/telecom -- and bytes 1-3 are
 *   DEBSUCBB, the UCB address; explicitly the section a CTC gets, being
 *   neither DASD nor ISAM). Read byte-wise (project convention, ADR-0024)
 *   -- no struct overlay on a control block this project does not own.
 *
 *   UCB sanity check -- same Debugging Handbook, Volume 3 (GC28-0710-0,
 *   Dec 1978), "IOS Unit Control Block": UCB+13, 3 EBCDIC characters,
 *   UCBNAME ("unit name"). Before issuing SVC 33 at a computed address,
 *   this probe checks UCBNAME against the device's own "%03X" string (the
 *   same 3-hex-digit convention ctci_chan_alloc already uses, ADR-0020) --
 *   a wrong pointer chase is caught as a clean, non-abending assertion
 *   failure instead of an SVC fired at garbage storage.
 */
#include "nsfctci.h"
#include "nsfmm.h"
#include "nsffmt.h"
#include <mbtcheck.h>
#include <string.h>
#include <stdio.h>
#include <clibecb.h>

#ifndef NSFCTCI_CUU
#define NSFCTCI_CUU 0x0500u   /* the read subchannel of the live 500/501 pair */
#endif

/* test/asm/tsthalt.asm -- see its header for the exact SVC recipes. */
extern void tsthio_halt(UINT ucb) asm("TSTHHALT");
extern int  tsthio_purge(void *parmlist) asm("TSTHPURG");

/* test/asm/tststmw.asm (M0-5's timer-accuracy helper) -- reused as-is for
 * a plain task-synchronous wait; no new wait primitive forged here. */
extern void stimer_wait(UINT centisecs) asm("STIMWAIT");

/* ---- byte-wise pointer chase: DCB -> DEB -> UCB (see header citations).
 * No struct overlay -- these are control blocks this project does not own
 * (ADR-0024's "byte-wise, big-endian" discipline applied to a foreign
 * fixed layout, same as the CTCIHDR/CTCISEG codec). ---- */

/* A 3-byte 24-bit address field at base+off (the byte at off-1 in the
 * enclosing fullword is always a flags/modifier byte, never part of the
 * address -- true of both DCBDEBAD/DCBIFLGS and DEBSUCBA/DEBSDVM). */
static UINT addr24(const UCHAR *base, UINT off)
{
    return ((UINT)base[off] << 16) | ((UINT)base[off + 1] << 8) |
           (UINT)base[off + 2];
}

static UINT dcb_to_deb(const void *dcb) { return addr24((const UCHAR *)dcb, 45); }
static UINT deb_to_ucb(UINT deb)        { return addr24((const UCHAR *)deb, 33); }

/* UCBNAME (UCB+13, 3 EBCDIC chars) against the device's own "%03X" text. */
static int ucb_name_matches(UINT ucb, USHORT cuu)
{
    const UCHAR *u = (const UCHAR *)ucb;
    char         want[4];

    nsf_snprintf(want, sizeof(want), "%03X", (unsigned)cuu);
    return u[13] == (UCHAR)want[0] && u[14] == (UCHAR)want[1] &&
           u[15] == (UCHAR)want[2];
}

/* ---- PURGE (SVC 16) parameter list: TCB-scope, HALT, post-ECBs, 16-byte
 * form for a real return code (GC26-3830-4; see header). Built byte-wise,
 * not as a struct -- same reasoning as addr24 above. ---- */
static void build_purge_tcb_halt(UCHAR pl[16], UINT *pirl_out)
{
    UINT pirl_addr = (UINT)pirl_out;

    memset(pl, 0, 16);
    /* byte0: postECB(.1..)=1 | HALT(..1.)=1 | TCB-purge(....  ..1.)=1 |
     * 16-byte-list(.......1)=1 -> 0x40|0x20|0x02|0x01 */
    pl[0]  = 0x63u;
    pl[9]  = (UCHAR)(pirl_addr >> 16);
    pl[10] = (UCHAR)(pirl_addr >> 8);
    pl[11] = (UCHAR)pirl_addr;
    /* bytes 1-3 (DEB addr), 5-7 (TCB addr), 12-15 (extra options / ASID)
     * all stay zero: not a dataset purge, TCB = 0 = our own job-step TCB
     * (the only form documented as available from problem state), no
     * address-space purge (that needs supervisor state), no ASID. */
}

/* ---- bounded wait: poll the ECB's POSTED bit (never non-zeroness, per
 * the project's ECB-wait rule) between short task-synchronous naps. ---- */
static int wait_ecb_posted(ECB *ecb, UINT max_cs, UINT poll_cs)
{
    UINT waited;

    for (waited = 0u; waited < max_cs; waited += poll_cs) {
        if (*ecb & ECB_POSTED_BIT) {
            return 1;
        }
        stimer_wait(poll_cs);
    }
    return (*ecb & ECB_POSTED_BIT) ? 1 : 0;
}

/* ---- one probe cycle shared by both mechanisms: EXCP a READ, prove a
 * quiet link does NOT complete it in ~1s, invoke `stop` to try to end it,
 * observe the completion signature, then prove a fresh READ still works
 * (the subchannel is not wedged) once a real frame arrives. ---- */
static void run_probe(void *scb, UCHAR *rbuf, UINT bufsize, USHORT cuu,
                      const char *label, void (*stop)(void *scb, USHORT cuu))
{
    ECB  ecb = 0u;
    UINT postcode = 0u, residual = 0u, xferred;
    int  drained;

    printf("--- %s probe: CUU %03X ---\n", label, (unsigned)cuu);

    /* Drain any frame Hercules already queued before this job even started
     * (it buffers inbound frames whether or not a READ is outstanding,
     * ADR-0025) -- keep re-arming until one READ does NOT complete within
     * ~1s. That still-outstanding READ, on a queue now known empty, is the
     * one the halt attempt targets; a completed drain read is NOT evidence
     * about the halt mechanism either way. */
    for (drained = 0; drained < 8; drained++) {
        ecb = 0u;
        CHECK(ctci_read(scb, rbuf, bufsize, &ecb) == 0, "EXCP READ started");
        if (!wait_ecb_posted(&ecb, 100u, 50u)) {
            break;                       /* still outstanding: queue was empty */
        }
        ctci_status(scb, &postcode, &residual);
        printf("%s: drained a queued frame (%u bytes) -- not evidence, "
               "re-arming\n", label, (unsigned)(bufsize - residual));
    }
    CHECK(drained < 8, "the queue drained within 8 READs");

    CHECK((ecb & ECB_POSTED_BIT) == 0u,
          "READ has NOT completed after ~1s on a quiet, drained link");

    stop(scb, cuu);

    CHECK(wait_ecb_posted(&ecb, 500u, 50u),
          "READ ECB posted within 5s of the halt attempt");
    ctci_status(scb, &postcode, &residual);
    xferred = bufsize - residual;
    printf("%s: post=%02X residual=%u xferred=%u\n", label,
           (unsigned)(postcode & 0xFFu), (unsigned)residual, (unsigned)xferred);
    /* The discriminator is the POST CODE, not the residual/xferred count:
     * GC26-3830-4 Figure 13 documents completion code 48000000 ("the
     * request queue element... made available after it has been purged")
     * as DISTINCT from 7F000000 (normal) -- and explicitly notes the CSW
     * (hence the residual count) "does not apply" on a purge completion.
     * Empirically (this probe) BOTH IOHALT and PURGE surface post=48 --
     * IOS reclassifies a problem-state halt this way even via the plain
     * HIO path, not the raw hardware CE+DE/full-residual signature a
     * literal reading of Hercules ctc_ctci.c would predict. */
    CHECK_EQ((long)postcode, 0x48L,
             "post code 48 (purged), not 7F (a real data completion)");

    /* --- re-arm proof: the subchannel must still take a fresh READ, and
     * that READ must complete WITH data once a real frame arrives. The
     * runbook performs one host ping now (bounded ~60s budget). --- */
    ecb = 0u;
    CHECK(ctci_read(scb, rbuf, bufsize, &ecb) == 0,
          "fresh READ re-armed after the halt attempt");
    printf("%s: waiting up to 60s for an inbound frame "
           "(ping the guest HOME now)...\n", label);
    CHECK(wait_ecb_posted(&ecb, 6000u, 100u),
          "post-halt READ completed (subchannel not wedged)");
    ctci_status(scb, &postcode, &residual);
    xferred = bufsize - residual;
    printf("%s: post-halt re-arm post=%02X residual=%u xferred=%u\n", label,
           (unsigned)(postcode & 0xFFu), (unsigned)residual, (unsigned)xferred);
    CHECK(xferred > 0u,
          "post-halt READ received real data (subchannel re-armed cleanly)");
}

static void stop_via_iohalt(void *scb, USHORT cuu)
{
    UINT deb, ucb;
    int  ok;

    deb = dcb_to_deb(scb);
    ucb = deb_to_ucb(deb);
    ok  = ucb_name_matches(ucb, cuu);
    printf("iohalt: DEB=%06X UCB=%06X UCBNAME-match=%d\n",
           (unsigned)deb, (unsigned)ucb, ok);
    CHECK(ok, "DCB->DEB->UCB pointer chase lands on the expected CUU");
    if (ok) {
        tsthio_halt(ucb);
        printf("iohalt: SVC 33 issued (IOHALT documents no return code)\n");
    } else {
        printf("iohalt: SKIPPED issuing SVC 33 (UCB sanity check failed)\n");
    }
}

static void stop_via_purge(void *scb, USHORT cuu)
{
    UCHAR pl[16];
    UINT  pirl = 0u;
    int   rc;

    (void)scb;
    (void)cuu;
    build_purge_tcb_halt(pl, &pirl);
    rc = tsthio_purge(pl);
    printf("purge: SVC 16 rc=%d byte4=%02X pirl=%06X\n",
           rc, (unsigned)pl[4], (unsigned)pirl);
    CHECK_EQ((long)rc, 0L, "PURGE (TCB-scope, HALT) returns rc 0 from problem state");
}

int main(void)
{
    void  *scb;
    UCHAR *rbuf;
    MMPOOL *scbpool, *bufpool;
    char    ddn[9];
    char    unit[5];
    short   s99err = 0, s99info = 0;

    printf("=== TSTHIO: M3-0a Stage 0 -- problem-state halt of a CTCI READ ===\n");

    if (mm_init(NULL) != 0) {
        printf("mm_init failed\n");
        return 1;
    }
    scbpool = mm_pool_create("HIOSCB  ", (USHORT)ctci_scb_size(), 1u);
    bufpool = mm_pool_create("HIOBUF  ", (USHORT)CTCI_BUF_DEFAULT, 1u);
    mm_init_complete();
    if (scbpool == NULL || bufpool == NULL) {
        printf("pool create failed\n");
        mm_shutdown();
        return 1;
    }

    scb  = mm_alloc(scbpool);
    rbuf = (UCHAR *)mm_alloc(bufpool);
    CHECK(scb != NULL && rbuf != NULL, "scb + read buffer allocated");
    if (scb == NULL || rbuf == NULL) {
        mm_shutdown();
        return mbt_test_summary("TSTHIO");
    }

    memset(ddn, 0, sizeof ddn);
    nsf_snprintf(unit, sizeof(unit), "%03X", (unsigned)NSFCTCI_CUU);
    CHECK(ctci_alloc_unit(unit, ddn, &s99err, &s99info) == 0,
          "SVC 99 allocate of the read subchannel");
    printf("alloc: unit=%s ddn=%s s99err=%04X s99info=%04X\n",
           unit, ddn, (unsigned)(s99err & 0xFFFF), (unsigned)(s99info & 0xFFFF));
    CHECK(ctci_open_sub(scb, 0u, ddn) == 0, "OPEN the read subchannel");

#ifdef TSTHIO_PURGE
    run_probe(scb, rbuf, CTCI_BUF_DEFAULT, (USHORT)NSFCTCI_CUU,
              "purge", stop_via_purge);
#else
    run_probe(scb, rbuf, CTCI_BUF_DEFAULT, (USHORT)NSFCTCI_CUU,
              "iohalt", stop_via_iohalt);
#endif

    (void)ctci_close_sub(scb);
    (void)ctci_free_ddn(ddn);
    mm_shutdown();
    return mbt_test_summary("TSTHIO");
}
