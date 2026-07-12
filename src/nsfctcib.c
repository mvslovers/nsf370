/*
 * nsfctcib.c -- the CTCI driver bottom half: DEVOPS + the ECB-completion DEVIO
 * seam (spec 9.3; ADR-0019/0020/0021). PORTABLE C: it drives the channel only
 * through the top-half primitives (asm/nsfctcio.asm, shimmed by
 * src/nsfctcio_host.c on the host), the channel/SVC 99 seam (src/nsfctci.c,
 * shimmed by src/nsfctci_host.c), and the codec (src/nsfctcif.c). So the whole
 * bottom half -- ping-pong READ, completion demux, decode->PBUF, sendq->WRITE,
 * ownership -- is host-testable over the shim (test/tstctci.c) and the SAME code
 * runs on MVS.
 *
 * COMPLETION MODEL (ADR-0019). IOS posts the read/write IOB ECBs directly; there
 * is no async producer thread and doneq stays EMPTY (never pushed). The DEVIO
 * seam wires this device's own recb+wecb into the loop's ECBLIST (io->collect),
 * demuxes them each pass (io->service), and starts one WRITE at a time from the
 * sendq (io->kick) -- the per-device mirror of the loop's three global device
 * hooks, so NSFDEV/NSFEVT never name CTCI.
 *
 * OWNERSHIP (spec 9.2 / §3, single-owner PBUFs). Inbound: the decoder copies
 * each IPv4 segment into a fresh PBUF and hands it up as EV_PACKET_RECEIVED (the
 * handler owns it); the 20 KB I/O buffer is NEVER handed up. Outbound: dev_send
 * queued the PBUF; io->kick holds it in d->txflight across the WRITE and frees it
 * exactly once -- on WRITE completion normally, or at shutdown if still in
 * flight. Every path queues or frees, never both, never neither.
 *
 * MEMORY (§3). No storage service on the hot path: the CTCIDEV, the two
 * subchannel control blocks and the three I/O buffers all come from pools
 * reserved once in the init window (ctci_reserve, before mm_init_complete).
 * mm_alloc from those pools is fine after the seal (only mm_pool_create is
 * sealed); an inbound PBUF that cannot be allocated is a drop + count, never an
 * ABEND.
 */
#include "nsfctci.h"
#include "nsfctcif.h"
#include "nsfdev.h"
#include "nsfmm.h"
#include "nsfbuf.h"
#include "nsfevt.h"             /* evt_post, EV_PACKET_RECEIVED */
#include "nsfsts.h"             /* STS_INC / STS_ADD */
#include "nsftrc.h"             /* TRC(DRIVER, ...) */
#include "nsfmsg.h"             /* nsfmsg (NSF2xxE) */

#include <string.h>

/* Init-window pools (spec 9.3): the CTCIDEV blocks, the per-subchannel control
 * blocks (CTCISC, sized by the asm), and the I/O buffers. All reserved once by
 * ctci_reserve, before mm_init_complete. */
static MMPOOL *g_devpool;
static MMPOOL *g_scbpool;
static MMPOOL *g_bufpool;
static UINT    g_bufsize;         /* bytes per I/O buffer (pool objsize)        */

/* NULL-safe counter bumps (a per-device counter may be NULL if the stats
 * registry filled at registration -- a build-time miscount, never fatal). */
static void ctr(STSCTR *c)            { if (c != NULL) STS_INC(c); }
static void ctr_add(STSCTR *c, UINT n) { if (c != NULL && n != 0u) STS_ADD(c, n); }

/* ---- region reservation (init window) ----------------------------------- */

int ctci_reserve(UINT ndev, UINT bufsize)
{
    UINT scbsz;

    if (ndev == 0u) {
        ndev = 1u;
    }
    if (bufsize == 0u) {
        bufsize = CTCI_BUF_DEFAULT;
    }
    if (bufsize > CTCI_BUF_MAX) {
        bufsize = CTCI_BUF_MAX;       /* CCW count-field width is the hard cap   */
    }
    g_bufsize = bufsize;
    scbsz     = ctci_scb_size();      /* the top half reports its block size     */

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

/* ---- CTCIDEV allocation / release --------------------------------------- */

/* Release every pool object a (partially built) device holds -- leak-safe on a
 * failed init and on teardown. Frees only what is non-NULL. */
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

/* Allocate + initialize a CTCIDEV (no channel I/O yet). Validates the MTU cap,
 * mm_allocs the two subchannel control blocks and the three I/O buffers. Returns
 * the CTCIDEV*, or NULL (with an NSF2xxE) on any failure -- nothing half-built. */
static CTCIDEV *ctci_dev_alloc(USHORT cuu, USHORT mtu)
{
    CTCIDEV *d;

    if (g_devpool == NULL) {
        nsfmsg("NSF201E CTCI %04X NOT RESERVED", (unsigned)cuu);
        return NULL;
    }
    /* MTU cap (spec 9.3): the frame must fit one block AND Hercules' limit. */
    if (mtu != 0u &&
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
    return d;
}

/* ---- inbound: decode one received block into PBUFs ---------------------- */

/* Walk the block just received in `buf` (`len` bytes moved) and hand each IPv4
 * segment up as EV_PACKET_RECEIVED. Copies OUT of the I/O buffer into a right-
 * sized PBUF -- never hands the shared 20 KB buffer up. Drop + count (never
 * ABEND) on pool exhaustion, a would-truncate payload, or a codec-flagged bad /
 * non-IPv4 segment. */
static void ctci_decode_block(NETDEV *dev, CTCIDEV *d, const UCHAR *buf, UINT len)
{
    CTCIFDEC     dec;
    const UCHAR *ip;
    USHORT       iplen;
    UINT         idx = dev_index(dev);

    (void)d;
    ctcif_dec_init(&dec, buf, len);
    while ((ip = ctcif_dec_next(&dec, &iplen)) != NULL) {
        PBUF *b = buf_alloc(iplen);

        if (b == NULL) {                        /* pool exhausted: drop + count  */
            ctr(dev->ctr_ierr);
            continue;
        }
        buf_reset_rx(b);                        /* receive at start, full capacity */
        if (buf_copyin(b, ip, iplen) != iplen) { /* would truncate: drop + count  */
            buf_free(b);
            ctr(dev->ctr_ierr);
            continue;
        }
        if (evt_post(EV_PACKET_RECEIVED, b, idx) != 0) { /* EVT pool exhausted    */
            buf_free(b);
            ctr(dev->ctr_ierr);
        } else {
            ctr(dev->ctr_in);
        }
    }
    /* Segments the codec dropped (non-IPv4, or a malformed tail) are inbound
     * errors too. */
    ctr_add(dev->ctr_ierr, dec.nonipv4 + dec.malformed);
    if (dec.nonipv4 != 0u || dec.malformed != 0u) {
        TRC(DRIVER, "CTCI %04X rx drop nonip=%u bad=%u", (unsigned)d->cuu,
            (unsigned)dec.nonipv4, (unsigned)dec.malformed);
    }
}

/* ---- read completion: re-drive FIRST, then parse (ping-pong) ------------- */

static void ctci_read_complete(NETDEV *dev, CTCIDEV *d)
{
    UINT   post = 0u;
    UINT   res  = 0u;
    UINT   len;
    UCHAR *donebuf;
    UCHAR *nextbuf;

    ctci_status(d->rscb, &post, &res);
    len = (res <= d->bufsize) ? (d->bufsize - res) : 0u;

    /* The buffer this completion filled, and the OTHER one to re-drive into. */
    donebuf = (d->cur == 0u) ? d->rbuf0 : d->rbuf1;
    nextbuf = (d->cur == 0u) ? d->rbuf1 : d->rbuf0;

    /* Re-drive the READ into the OTHER buffer BEFORE parsing (normative, spec
     * 9.3): the window with no READ outstanding shrinks to a few instructions,
     * and if a frame is already queued it lands in nextbuf, not donebuf.
     * ctci_read clears recb on arm (CISTART XC), so no stale posted bit survives
     * to trigger a phantom completion next pass. The device must NEVER be left
     * without a READ -- so we re-drive even on a bad completion. */
    d->cur = (d->cur == 0u) ? 1u : 0u;
    ctci_read(d->rscb, nextbuf, d->bufsize, &d->recb);

    if (post != CTCI_POST_NORMAL) {
        /* An I/O error: count it and trace, but the re-drive above already kept
         * a READ outstanding. The data (if any) is not trustworthy -- do not
         * decode it. */
        ctr(dev->ctr_ierr);
        TRC(DRIVER, "CTCI %04X READ post=%02X (not X'7F')", (unsigned)d->cuu,
            (unsigned)(post & 0xFFu));
        return;
    }

    ctci_decode_block(dev, d, donebuf, len);
}

/* ---- write completion --------------------------------------------------- */

static void ctci_write_complete(NETDEV *dev, CTCIDEV *d)
{
    UINT post = 0u;
    UINT res  = 0u;

    ctci_status(d->wscb, &post, &res);
    d->wecb = 0u;                       /* clear so no phantom completion next pass */

    if (post == CTCI_POST_NORMAL) {
        ctr(dev->ctr_out);
    } else {
        ctr(dev->ctr_oerr);
        TRC(DRIVER, "CTCI %04X WRITE post=%02X (not X'7F')", (unsigned)d->cuu,
            (unsigned)(post & 0xFFu));
    }
    /* Free the in-flight PBUF exactly once (the WRITE is done, error or not). */
    if (d->txflight != NULL) {
        buf_free(d->txflight);
        d->txflight = NULL;
    }
}

/* ---- DEVIO seam (ADR-0021) ---------------------------------------------- */

/* Contribute both completion ECBs so the loop WAITs on read AND write. */
static int ctci_io_collect(NETDEV *dev, NSFECB **list, int max)
{
    CTCIDEV *d = (CTCIDEV *)dev->priv;
    int      n = 0;

    if (d == NULL) {
        return 0;
    }
    if (n < max) list[n++] = (NSFECB *)&d->recb;
    if (n < max) list[n++] = (NSFECB *)&d->wecb;
    return n;
}

/* Demux each pass: BOTH ECBs can be posted in one wake, so check both (not
 * else-if). doneq is untouched (ADR-0019). */
static void ctci_io_service(NETDEV *dev)
{
    CTCIDEV *d = (CTCIDEV *)dev->priv;

    if (d == NULL) {
        return;
    }
    if (d->recb & NSFECB_POSTED) {
        ctci_read_complete(dev, d);
    }
    if (d->wecb & NSFECB_POSTED) {
        ctci_write_complete(dev, d);
    }
}

/* Start at most ONE WRITE from the sendq (one outstanding at a time). Encodes
 * the PBUF into the write buffer -- copying the IP bytes straight in (no large
 * executive-stack temp) and framing in place -- then issues the WRITE and holds
 * the PBUF in d->txflight until the WRITE completes. */
static void ctci_io_kick(NETDEV *dev)
{
    CTCIDEV *d = (CTCIDEV *)dev->priv;
    QELEM   *qe;
    PBUF    *b;
    UINT     iplen;
    UINT     blklen;
    USHORT   got;

    if (d == NULL || d->txflight != NULL) {
        return;                         /* a WRITE is already outstanding */
    }
    qe = q_deq(&dev->sendq);
    if (qe == NULL) {
        return;                         /* nothing queued */
    }
    b = Q_ENTRY(qe, PBUF, q);

    iplen  = (UINT)buf_chain_len(b);
    blklen = (UINT)sizeof(CTCIHDR) + (UINT)sizeof(CTCISEG) + iplen
             + (UINT)sizeof(CTCIHDR);
    /* Reject a frame that will not fit the block (drop + count, never grow). */
    if (iplen == 0u || blklen > d->bufsize) {
        buf_free(b);
        ctr(dev->ctr_oerr);
        return;
    }
    /* Copy the IP straight into the write buffer at the segment payload offset,
     * then frame in place (ctcif_encode_hdr). */
    got = buf_copyout(b, d->wbuf + (UINT)sizeof(CTCIHDR) + (UINT)sizeof(CTCISEG),
                      (USHORT)iplen);
    if ((UINT)got != iplen) {           /* short copy (should not happen): drop  */
        buf_free(b);
        ctr(dev->ctr_oerr);
        return;
    }
    blklen = ctcif_encode_hdr(d->wbuf, d->bufsize, iplen);
    if (blklen == 0u) {
        buf_free(b);
        ctr(dev->ctr_oerr);
        return;
    }

    d->txflight = b;                    /* freed on WRITE completion */
    ctci_write(d->wscb, d->wbuf, blklen, &d->wecb);
    TRC(DRIVER, "CTCI %04X WRITE %u bytes (ip %u)", (unsigned)d->cuu,
        (unsigned)blklen, (unsigned)iplen);
}

static DEVIO g_ctci_io = {
    ctci_io_collect,
    ctci_io_service,
    ctci_io_kick
};

/* ---- DEVOPS ------------------------------------------------------------- */

/* init only attaches the DEVIO seam; the CTCIDEV + I/O buffers are allocated at
 * start, so a device that is registered but never started (or whose start fails)
 * holds no pool storage -- dev_shutdown short-circuits on a DOWN device and
 * would otherwise not run ops->shutdown to release it. */
static int ctci_op_init(NETDEV *dev, const DEVCFG *cfg)
{
    (void)cfg;                          /* cuu/mtu come from the NETDEV at start */
    dev_set_io(dev, &g_ctci_io);        /* ECB-completion model (ADR-0021) */
    return 0;
}

static int ctci_op_start(NETDEV *dev)
{
    CTCIDEV *d = ctci_dev_alloc(dev->cuu, dev->mtu);

    if (d == NULL) {
        return -1;                      /* no storage / MTU cap: refuse to start */
    }
    if (ctci_chan_open(d) != 0) {        /* SVC 99 allocate + OPEN (MVS) */
        ctci_dev_release(d);            /* leak-safe: release what init did not  */
        return -1;
    }
    /* Drive the first READ into buffer A. MIH after long idle is normal (spec
     * 9.3): a READ simply stays outstanding until a frame arrives. */
    d->cur      = 0u;
    d->recb     = 0u;
    d->wecb     = 0u;
    d->txflight = NULL;
    dev->priv   = d;                    /* now live: collect/service/kick see it */
    ctci_read(d->rscb, d->rbuf0, d->bufsize, &d->recb);
    TRC(DRIVER, "CTCI %04X started, first READ driven", (unsigned)d->cuu);
    return 0;
}

/* send() just hands the PBUF to the bounded sendq via the generic dev_send (it
 * calls this op only from nsfdev_kick_output for the DONEQ model). For the CTCI
 * DEVIO model, io->kick drains the sendq instead, so this op is never the
 * transmit path -- but the contract still requires it to TAKE OWNERSHIP. It
 * should not be reached; if it is, free + count rather than leak. */
static int ctci_op_send(NETDEV *dev, PBUF *b)
{
    buf_free(b);
    ctr(dev->ctr_oerr);
    return -1;
}

static int ctci_op_shutdown(NETDEV *dev)
{
    CTCIDEV *d = (CTCIDEV *)dev->priv;

    if (d == NULL) {
        return 0;
    }
    /* Halt I/O (CLOSE both subchannels; on MVS this quiesces any outstanding
     * READ), then free the in-flight WRITE PBUF if the WRITE never completed,
     * and return all region storage to its pools (leak gate: pools to baseline).
     * The sendq/doneq are drained by dev_shutdown after this returns. */
    ctci_chan_close(d);
    if (d->txflight != NULL) {
        buf_free(d->txflight);
        d->txflight = NULL;
    }
    dev_set_io(dev, NULL);
    ctci_dev_release(d);
    dev->priv = NULL;
    return 0;
}

static DEVOPS g_ctci_ops = {
    ctci_op_init,
    ctci_op_start,
    ctci_op_send,
    ctci_op_shutdown
};

DEVOPS *ctci_devops(void)
{
    return &g_ctci_ops;
}

#if NSF_DEBUG
/* The pool backing CTCI region storage, so a host test can prove device storage
 * returns to baseline after shutdown (which=0 dev, 1 scb, 2 buf). Outside the
 * production interface (mirrors buf_debug_pool / mm_debug_live_regions). */
MMPOOL *ctci_debug_pool(UCHAR which) asm("NSFCIDBP");
MMPOOL *ctci_debug_pool(UCHAR which)
{
    switch (which) {
    case 0:  return g_devpool;
    case 1:  return g_scbpool;
    default: return g_bufpool;
    }
}
#endif
