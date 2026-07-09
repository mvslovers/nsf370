/*
 * nsfctci.c -- CTCI device driver, C bottom half / lifecycle (spec 9.3;
 *              ADR-0019). MVS-ONLY: it uses the libc370 SVC 99 dynamic-
 *              allocation seam and the HLASM EXCP primitives in
 *              asm/nsfctcio.asm, so it never compiles on the host (there is no
 *              [host].replace shim -- the whole driver is in the host=false
 *              TSTCTCM test only, like nsfmain.c is MVS-only).
 *
 * M1-3 owns: reserve NSFMM region storage, SVC 99 allocate the CUU pair, open
 * both subchannels, and start/decode/close ONE raw EXCP each way. It does NOT
 * yet build/parse CTCIHDR/CTCISEG, convert PBUFs, drain a sendq, or implement
 * DEVOPS -- all M1-4. See nsfctci.h for the interface.
 *
 * ============  DEFERRED SEAM (blocked on hardware)  ============
 * The EXCP READ/WRITE path (ctci_dev_open .. ctci_dev_close) is UNVALIDATED on
 * MVS: there is no CTCI device on the Hercules side and the CUU pair is in no
 * UCB yet, so the channel program cannot be driven. This TU cross-links clean;
 * its on-MVS runtime is owed a live run (PR runbook). Proven under test-mvs
 * today: the SVC 99 seam (ctci_alloc_unit) with a deliberately invalid unit,
 * which fails with a decoded S99 error and touches no real device.
 *
 * MEMORY NOTE. The libc370 SVC 99 text-unit builders (NewTXT99 / arrayadd,
 * inside __txrddn / __txunit / __txshr / __txddn) malloc transiently and
 * FreeTXT99Array releases it on every path. This is init-time allocation
 * through the sanctioned libc370 seam (ADR-0015 precedent, ADR-0018 pattern),
 * confined to a single call -- NOT protocol-path storage, which stays with
 * NSFMM (§3). All DEVICE storage (CTCIDEV, the subchannel control blocks, the
 * I/O buffers) is NSFMM pool storage reserved once in the init window.
 * ==============================================================
 */

#include "nsfctci.h"
#include "nsfmm.h"
#include "nsfmsg.h"

#include <string.h>
#include <stdio.h>

#include <svc99.h>             /* RB99, TXT99, __tx* builders, S99* verbs    */
#include <clibary.h>          /* arraycount / FreeTXT99Array array helper   */

/* __svc99 issues the SVC 99. The sysroot mvssupa.h declares it only under
 * #ifdef MUSIC and svc99.h omits it, so declare it here with the prototype
 * libc370 defines (plain cc370 linkage). */
extern int __svc99(void *rb);

/* Init-window pools (spec 9.3): one for the CTCIDEV blocks, one for the
 * per-subchannel control blocks (CTCISC, sized by the asm), one for the I/O
 * buffers. All reserved once by ctci_reserve, before mm_init_complete. */
static MMPOOL *g_devpool;
static MMPOOL *g_scbpool;
static MMPOOL *g_bufpool;
static UINT    g_bufsize;         /* bytes per I/O buffer (pool objsize)      */

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

int ctci_alloc_unit(const char *unit4, char *ddn8, short *s99err, short *s99info)
{
    TXT99 **txt99 = NULL;
    int     err;

    if (s99err)  *s99err  = 0;
    if (s99info) *s99info = 0;

    /* Ask the system to return a generated DDNAME, allocate the device named
     * by UNIT=<unit4>, DISP=SHR (DALSTATS). __tx* builders arrayadd into
     * txt99; each returns non-zero on failure. */
    if (__txrddn(&txt99, NULL) ||
        __txunit(&txt99, unit4) ||
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

/* ---- Region reservation (init window) ----------------------------------- */

int ctci_reserve(UINT ndev, UINT bufsize)
{
    UINT scbsz;

    if (ndev == 0) {
        ndev = 1;
    }
    if (bufsize == 0) {
        bufsize = CTCI_BUF_DEFAULT;
    }
    if (bufsize > CTCI_BUF_MAX) {
        bufsize = CTCI_BUF_MAX;   /* CCW count-field width is the hard cap     */
    }
    g_bufsize = bufsize;
    scbsz     = ctci_scb_size();  /* the asm reports its control-block size    */

    g_devpool = mm_pool_create("CTCIDEV ", (USHORT)sizeof(CTCIDEV), (USHORT)ndev);
    g_scbpool = mm_pool_create("CTCISCB ", (USHORT)scbsz,   (USHORT)(2u * ndev));
    g_bufpool = mm_pool_create("CTCIBUF ", (USHORT)bufsize, (USHORT)(3u * ndev));

    if (g_devpool == NULL || g_scbpool == NULL || g_bufpool == NULL) {
        nsfmsg("NSF203E CTCI RESERVE FAILED (%u DEV, %u BYTE BUF)",
               (unsigned)ndev, (unsigned)bufsize);
        return 1;
    }
    return 0;
}

/* Release every pool object a partially-built device holds (leak-safe cleanup
 * on a failed open, and the teardown path). Frees only what is non-NULL. */
static void ctci_dev_release(CTCIDEV *d)
{
    if (d == NULL) {
        return;
    }
    if (d->rbuf0) mm_free(g_bufpool, d->rbuf0);
    if (d->rbuf1) mm_free(g_bufpool, d->rbuf1);
    if (d->wbuf)  mm_free(g_bufpool, d->wbuf);
    if (d->rscb)  mm_free(g_scbpool, d->rscb);
    if (d->wscb)  mm_free(g_scbpool, d->wscb);
    mm_free(g_devpool, d);
}

/* ---- Device lifecycle --------------------------------------------------- */

CTCIDEV *ctci_dev_open(USHORT cuu, USHORT mtu)
{
    CTCIDEV *d;
    char     unit[5];
    short    s99err, s99info;

    if (g_devpool == NULL) {
        nsfmsg("NSF201E CTCI %04X NOT RESERVED", (unsigned)cuu);
        return NULL;
    }

    /* MTU cap (spec 9.3): the frame must fit one block AND Hercules' limit. */
    if (mtu != 0 &&
        ((UINT)mtu > CTCI_MAX_FRAME(g_bufsize) || (UINT)mtu > CTCI_MTU_MAX)) {
        nsfmsg("NSF200E CTCI %04X MTU %u EXCEEDS CAP", (unsigned)cuu,
               (unsigned)mtu);
        return NULL;
    }

    d = (CTCIDEV *)mm_alloc(g_devpool);
    if (d == NULL) {
        nsfmsg("NSF201E CTCI %04X NO DEVICE STORAGE", (unsigned)cuu);
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    d->cuu     = cuu;
    d->wcuu    = (USHORT)(cuu + 1u);
    d->mtu     = mtu;
    d->bufsize = g_bufsize;
    d->state   = CTCI_S_DOWN;

    d->rscb  = mm_alloc(g_scbpool);
    d->wscb  = mm_alloc(g_scbpool);
    d->rbuf0 = (UCHAR *)mm_alloc(g_bufpool);
    d->rbuf1 = (UCHAR *)mm_alloc(g_bufpool);
    d->wbuf  = (UCHAR *)mm_alloc(g_bufpool);
    if (d->rscb == NULL || d->wscb == NULL ||
        d->rbuf0 == NULL || d->rbuf1 == NULL || d->wbuf == NULL) {
        nsfmsg("NSF201E CTCI %04X NO BUFFER STORAGE", (unsigned)cuu);
        ctci_dev_release(d);
        return NULL;
    }

    /* SVC 99 allocate both subchannels (read = cuu, write = cuu+1). */
    snprintf(unit, sizeof(unit), "%04X", (unsigned)d->cuu);
    if (ctci_alloc_unit(unit, d->rddn, &s99err, &s99info)) {
        nsfmsg("NSF202E CTCI %04X ALLOC FAILED S99 ERR %04X INFO %04X",
               (unsigned)d->cuu, (unsigned)(s99err & 0xFFFF),
               (unsigned)(s99info & 0xFFFF));
        ctci_dev_release(d);
        return NULL;
    }
    snprintf(unit, sizeof(unit), "%04X", (unsigned)d->wcuu);
    if (ctci_alloc_unit(unit, d->wddn, &s99err, &s99info)) {
        nsfmsg("NSF202E CTCI %04X ALLOC FAILED S99 ERR %04X INFO %04X",
               (unsigned)d->wcuu, (unsigned)(s99err & 0xFFFF),
               (unsigned)(s99info & 0xFFFF));
        ctci_free_ddn(d->rddn);
        ctci_dev_release(d);
        return NULL;
    }

    /* OPEN read subchannel INPUT, write subchannel OUTPUT. */
    if (ctci_open_sub(d->rscb, 0u, d->rddn)) {
        nsfmsg("NSF204E CTCI %04X READ OPEN FAILED", (unsigned)d->cuu);
        ctci_free_ddn(d->rddn);
        ctci_free_ddn(d->wddn);
        ctci_dev_release(d);
        return NULL;
    }
    if (ctci_open_sub(d->wscb, 1u, d->wddn)) {
        nsfmsg("NSF204E CTCI %04X WRITE OPEN FAILED", (unsigned)d->wcuu);
        ctci_close_sub(d->rscb);
        ctci_free_ddn(d->rddn);
        ctci_free_ddn(d->wddn);
        ctci_dev_release(d);
        return NULL;
    }

    d->state = CTCI_S_UP;
    nsfmsg("NSF210I CTCI %04X/%04X UP DD %s/%s MTU %u", (unsigned)d->cuu,
           (unsigned)d->wcuu, d->rddn, d->wddn, (unsigned)d->mtu);
    return d;
}

int ctci_dev_read(CTCIDEV *d)
{
    /* Read into ping-pong buffer A (d->rbuf0). The bottom half's
     * re-drive-before-parse alternation between rbuf0/rbuf1 is M1-4; M1-3
     * drives a single explicit READ, so it always uses rbuf0 (d->cur == 0). */
    return ctci_read(d->rscb, d->rbuf0, d->bufsize, &d->recb);
}

int ctci_dev_write(CTCIDEV *d, const void *buf, UINT len)
{
    if (len > d->bufsize) {
        return 1;                 /* would overrun the write buffer            */
    }
    /* ctci_write does not modify the buffer; the const is dropped only to fit
     * the shared CCW-data-address argument (the WRITE CCW reads from it). */
    return ctci_write(d->wscb, (void *)buf, len, &d->wecb);
}

int ctci_dev_status(CTCIDEV *d, int iswrite, UINT reqlen, UINT *len, UCHAR *post)
{
    void *scb = iswrite ? d->wscb : d->rscb;
    UINT  pc  = 0;
    UINT  res = 0;

    ctci_status(scb, &pc, &res);
    if (post) {
        *post = (UCHAR)pc;
    }
    if (len) {
        *len = (res <= reqlen) ? (reqlen - res) : 0u;
    }
    return (pc == 0x7Fu) ? 0 : 1;    /* 0x7F = normal completion (spec 9.3)   */
}

int ctci_dev_close(CTCIDEV *d)
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
    /* Return all region storage to its pools (leak gate: pools to baseline). */
    ctci_dev_release(d);
    return 0;
}
