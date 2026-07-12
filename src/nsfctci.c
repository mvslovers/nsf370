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
 *   - ctci_chan_open / ctci_chan_close: SVC 99 allocate the CUU pair, OPEN both
 *     subchannels (read INPUT, write OUTPUT), and the reverse.
 * The region storage, CTCIDEV allocation, ping-pong READ, completion demux,
 * codec and DEVOPS all live in the portable src/nsfctcib.c.
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

#include <string.h>
#include <stdio.h>

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

/* ---- channel open/close ------------------------------------------------- */

int ctci_chan_open(CTCIDEV *d)
{
    char  unit[5];
    short s99err  = 0;
    short s99info = 0;

    /* SVC 99 allocate both subchannels (read = cuu, write = cuu+1). MVS 3.8j
     * device numbers are 3 hex digits (CUU); a 4-digit unit name is UNDEFINED to
     * SVC 99 (S99ERROR 021C), so format the address as 3 digits (ADR-0020). */
    snprintf(unit, sizeof(unit), "%03X", (unsigned)d->cuu);
    if (ctci_alloc_unit(unit, d->rddn, &s99err, &s99info)) {
        nsfmsg("NSF202E CTCI %04X ALLOC FAILED S99 ERR %04X INFO %04X",
               (unsigned)d->cuu, (unsigned)(s99err & 0xFFFF),
               (unsigned)(s99info & 0xFFFF));
        return 1;
    }
    snprintf(unit, sizeof(unit), "%03X", (unsigned)d->wcuu);
    if (ctci_alloc_unit(unit, d->wddn, &s99err, &s99info)) {
        nsfmsg("NSF202E CTCI %04X ALLOC FAILED S99 ERR %04X INFO %04X",
               (unsigned)d->wcuu, (unsigned)(s99err & 0xFFFF),
               (unsigned)(s99info & 0xFFFF));
        ctci_free_ddn(d->rddn);
        d->rddn[0] = '\0';
        return 1;
    }

    /* OPEN read subchannel INPUT, write subchannel OUTPUT. */
    if (ctci_open_sub(d->rscb, 0u, d->rddn)) {
        nsfmsg("NSF204E CTCI %04X READ OPEN FAILED", (unsigned)d->cuu);
        ctci_free_ddn(d->rddn);
        ctci_free_ddn(d->wddn);
        d->rddn[0] = '\0';
        d->wddn[0] = '\0';
        return 1;
    }
    if (ctci_open_sub(d->wscb, 1u, d->wddn)) {
        nsfmsg("NSF204E CTCI %04X WRITE OPEN FAILED", (unsigned)d->wcuu);
        ctci_close_sub(d->rscb);
        ctci_free_ddn(d->rddn);
        ctci_free_ddn(d->wddn);
        d->rddn[0] = '\0';
        d->wddn[0] = '\0';
        return 1;
    }

    d->state = CTCI_S_UP;
    nsfmsg("NSF210I CTCI %04X/%04X UP DD %s/%s MTU %u", (unsigned)d->cuu,
           (unsigned)d->wcuu, d->rddn, d->wddn, (unsigned)d->mtu);
    return 0;
}

int ctci_chan_close(CTCIDEV *d)
{
    if (d == NULL) {
        return 0;
    }
    if (d->state == CTCI_S_UP) {
        ctci_close_sub(d->rscb);
        ctci_close_sub(d->wscb);
        d->state = CTCI_S_DOWN;
    }
    ctci_free_ddn(d->rddn);
    ctci_free_ddn(d->wddn);
    d->rddn[0] = '\0';
    d->wddn[0] = '\0';
    return 0;
}
