/*
 * nsfctci.c -- CTCI channel + SVC 99 seam (spec 9.3; ADR-0019/0020). MVS-ONLY:
 * it uses the libc370 SVC 99 dynamic-allocation seam and the HLASM EXCP OPEN
 * primitives, so it never compiles on the host -- the [host].replace map swaps
 * it for the success-no-op src/nsfctci_host.c, and the portable bottom half
 * (src/nsfctcib.c) brings a device up identically on both.
 *
 * This file owns exactly the pieces that touch real MVS device services:
 *   - the SVC 99 wrapper (svc99_call) and its allocate/unallocate helpers
 *     (ctci_alloc_unit / ctci_free_ddn), validated on TK5/MVSCE (issue #16);
 *   - ctci_chan_alloc / ctci_chan_unalloc: SVC 99 allocate / free the CUU pair
 *     (address-space scoped, so it stays on the executive). The OPEN/CLOSE of each
 *     subchannel moved to its OWNING SUBTASK (ADR-0022): a subchannel is OPENed
 *     and EXCP'd on one TCB, so its CLOSE at stop purges its own outstanding EXCP.
 * The region storage, CTCIDEV allocation, the read/write subtasks, completion
 * demux, codec and DEVOPS all live in the portable src/nsfctcib.c.
 *
 * The SVC 99 unit name is 3 hex digits ("%03X"): 3.8j device numbers are 3
 * digits, so a 4-digit name is an undefined unit (S99ERROR 021C) even for a
 * defined+online device (ADR-0020).
 *
 * MEMORY NOTE. The libc370 SVC 99 text-unit builders malloc transiently and
 * FreeTXT99Array releases on every path -- init-time allocation through the
 * sanctioned libc370 seam (ADR-0015/0018), confined to one call, NOT protocol-
 * path storage (which stays with NSFMM, §3).
 */

#include "nsfctci.h"
#include "nsfmsg.h"
#include "nsffmt.h"            /* nsf_snprintf (libc370's own snprintf does not
                                * NUL-terminate on truncation, issue #25.2)  */

#include <string.h>

#include <svc99.h>             /* RB99, TXT99, __tx* builders, S99* verbs    */
#include <clibary.h>          /* arraycount / FreeTXT99Array array helper   */

/* __svc99 issues the SVC 99. The sysroot mvssupa.h declares it only under
 * #ifdef MUSIC and svc99.h omits it, so declare it here with the prototype
 * libc370 defines (plain cc370 linkage). */
extern int __svc99(void *rb);

/* ---- SVC 99 seam (libc370) ---------------------------------------------- */

/* Mark the last text unit (high-order bit) and drive SVC 99 for `request`.
 * On failure copies S99ERROR/S99INFO out. Does NOT free the txt99 array (the
 * caller owns it). ddn8 (may be NULL) receives the generated DDNAME on a
 * successful allocate. Exported (nsfctci.h, NSFCISVC) so a test can drive both
 * the failure and the success paths over our request-block construction. */
int svc99_call(void *txt99v, unsigned char request,
               char *ddn8, short *s99err, short *s99info)
{
    TXT99  **txt99 = (TXT99 **)txt99v;
    RB99     rb99;
    unsigned count;
    int      err;

    memset(&rb99, 0, sizeof rb99);
    count = arraycount(&txt99);
    if (count == 0) {
        return 1;                 /* no text units built -- caller error      */
    }
    /* the last text-unit pointer carries the end-of-list bit */
    count--;
    txt99[count] = (TXT99 *)((unsigned)txt99[count] | 0x80000000u);

    rb99.len     = sizeof(RB99);
    rb99.request = request;
    rb99.flag1   = S99NOCNV;
    rb99.txtptr  = txt99;

    err = __svc99(&rb99);
    if (err) {
        if (s99err)  *s99err  = rb99.error;
        if (s99info) *s99info = rb99.info;
    } else if (ddn8) {
        memcpy(ddn8, txt99[0]->text, 8);
        ddn8[8] = '\0';
    }
    return err;
}

int ctci_alloc_unit(const char *unit, char *ddn8, short *s99err, short *s99info)
{
    TXT99 **txt99 = NULL;
    int     err;

    if (s99err)  *s99err  = 0;
    if (s99info) *s99info = 0;

    /* Ask the system to return a generated DDNAME, allocate the device named
     * by UNIT=<unit>, DISP=SHR (DALSTATS). __tx* builders arrayadd into txt99;
     * each returns non-zero on failure. */
    if (__txrddn(&txt99, NULL) ||
        __txunit(&txt99, unit) ||
        __txshr(&txt99, NULL)) {
        if (txt99) FreeTXT99Array(&txt99);
        return 1;
    }

    err = svc99_call(txt99, S99VRBAL, ddn8, s99err, s99info);
    FreeTXT99Array(&txt99);
    return err;
}

int ctci_free_ddn(const char *ddn8)
{
    TXT99 **txt99 = NULL;
    int     err;

    if (ddn8 == NULL || ddn8[0] == '\0') {
        return 0;                 /* nothing allocated                        */
    }
    if (__txddn(&txt99, ddn8)) {
        if (txt99) FreeTXT99Array(&txt99);
        return 1;
    }
    err = svc99_call(txt99, S99VRBUN, NULL, NULL, NULL);
    FreeTXT99Array(&txt99);
    return err;
}

/* ---- channel allocate / unallocate (SVC 99; executive, AS-scoped) -------- *
 * The OPEN/CLOSE moved to the owning subtask (ADR-0022): a subchannel is OPENed
 * and EXCP'd on ONE TCB, so its CLOSE at stop purges its own outstanding EXCP.
 * SVC 99 allocate/unallocate is address-space scoped (not TCB), so it stays here
 * on the executive: alloc fills d->rddn/d->wddn before the subtasks are created;
 * unalloc runs after they have CLOSEd + joined. */

int ctci_chan_alloc(CTCIDEV *d)
{
    char  unit[5];
    short s99err  = 0;
    short s99info = 0;

    /* MVS 3.8j device numbers are 3 hex digits (CUU); a 4-digit unit name is
     * UNDEFINED to SVC 99 (S99ERROR 021C), so format as 3 digits (ADR-0020). */
    nsf_snprintf(unit, sizeof(unit), "%03X", (unsigned)d->cuu);
    if (ctci_alloc_unit(unit, d->rddn, &s99err, &s99info)) {
        nsfmsg("NSF202E CTCI %04X ALLOC FAILED S99 ERR %04X INFO %04X",
               (unsigned)d->cuu, (unsigned)(s99err & 0xFFFF),
               (unsigned)(s99info & 0xFFFF));
        d->rddn[0] = '\0';
        return 1;
    }
    nsf_snprintf(unit, sizeof(unit), "%03X", (unsigned)d->wcuu);
    if (ctci_alloc_unit(unit, d->wddn, &s99err, &s99info)) {
        nsfmsg("NSF202E CTCI %04X ALLOC FAILED S99 ERR %04X INFO %04X",
               (unsigned)d->wcuu, (unsigned)(s99err & 0xFFFF),
               (unsigned)(s99info & 0xFFFF));
        ctci_free_ddn(d->rddn);
        d->rddn[0] = '\0';
        d->wddn[0] = '\0';
        return 1;
    }
    return 0;
}

int ctci_chan_unalloc(CTCIDEV *d)
{
    if (d == NULL) {
        return 0;
    }
    ctci_free_ddn(d->rddn);
    ctci_free_ddn(d->wddn);
    d->rddn[0] = '\0';
    d->wddn[0] = '\0';
    return 0;
}

/* ---- read-subchannel UCB chase (IOHALT target; ADR-0027) ---------------- *
 * DCB+44 (DCBDEBAD) -> DEB+32 (DEBSUCBA), byte-wise (no struct overlay on a
 * control block NSF does not own -- ADR-0024, the Stage-0 probe's discipline),
 * with a UCBNAME sanity check before an SVC ever fires at the computed address.
 * Offsets pinned from the OS/VS2 Debugging Handbook (GC28-0709/0710) and read
 * back from the running system by test/mvs/tsthio.c; the DCB in the scb sits at
 * offset 0 (SCDCB), so rscb IS the DCB address. */

/* A 3-byte 24-bit address field at base+off: the byte at off-1 in the enclosing
 * fullword is always a flags/modifier byte (DCBIFLGS before DCBDEBA, DEBSDVM
 * before DEBSUCBB), never part of the address. */
static UINT ctci_addr24(const UCHAR *base, UINT off)
{
    return ((UINT)base[off] << 16) | ((UINT)base[off + 1] << 8) |
           (UINT)base[off + 2];
}

int ctci_read_ucb(const void *rscb, USHORT cuu, UINT *ucb_out)
{
    const UCHAR *dcb = (const UCHAR *)rscb;
    UINT         deb;
    UINT         ucb;
    const UCHAR *u;
    char         want[5];

    if (rscb == NULL || ucb_out == NULL) {
        return 1;
    }
    deb = ctci_addr24(dcb, 45u);              /* DCB+44 DCBDEBAD (byte 44 = flags) */
    ucb = ctci_addr24((const UCHAR *)deb, 33u); /* DEB+32 DEBSUCBA (byte 32 = mod) */

    /* UCBNAME (UCB+13, 3 EBCDIC chars) must match the device's own "%03X" text --
     * a wrong pointer chase is caught here as a clean refusal, never an SVC 33
     * fired at garbage. */
    u = (const UCHAR *)ucb;
    nsf_snprintf(want, sizeof(want), "%03X", (unsigned)cuu);
    if (u[13] != (UCHAR)want[0] || u[14] != (UCHAR)want[1] ||
        u[15] != (UCHAR)want[2]) {
        nsfmsg("NSF207E CTCI %04X UCB CHASE MISMATCH (DEB %06X UCB %06X)",
               (unsigned)cuu, (unsigned)deb, (unsigned)ucb);
        return 1;
    }
    *ucb_out = ucb;
    return 0;
}
