/*
 * nsfctcib.c -- the CTCI driver bottom half: DEVOPS + the (repurposed) DEVIO
 * completion seam + the two I/O subtask functions (spec 9.3; ADR-0019/0020/0022).
 * PORTABLE C: it drives the channel only through the top-half primitives
 * (asm/nsfctcio.asm, shimmed by src/nsfctcio_host.c), the channel/SVC 99 seam
 * (src/nsfctci.c, shimmed by src/nsfctci_host.c), the codec (src/nsfctcif.c) and
 * the threading seam (src/nsfthr.c, shimmed by src/nsfthr_host.c). So the SAME
 * subtask logic runs host + MVS (test/tstctci.c drives it over the shims).
 *
 * COMPLETION MODEL (ADR-0022, M1-4b -- supersedes ADR-0019's WAIT premise). The
 * executive never WAITs on the raw IOB ECB (issue #18). Each subchannel is owned
 * by its own subtask (nsfthr):
 *   - read_sub OWNS the read subchannel: OPEN, then loop { EXCP READ into the ONE
 *     read buffer; single-ECB wait recb; store len/post; POST dev->ecb; wait
 *     returnecb }, CLOSE at stop. Single-block-synchronous: no READ is
 *     outstanding only for the microseconds the executive spends decoding --
 *     lossless (Hercules buffers with no READ outstanding, §9.3).
 *   - write_sub OWNS the write subchannel: OPEN, then loop { wait txgoecb; EXCP
 *     WRITE the executive-encoded buffer; single-ECB wait wecb; POST dev->ecb },
 *     CLOSE at stop. One WRITE outstanding at a time.
 * The executive's DEVIO seam: collect -> &dev->ecb (the ONE ECB it waits on);
 * service -> decode the read block into PBUFs ON THE EXECUTIVE (§9.2/§3) + reap a
 * completed WRITE + free its PBUF once; kick -> encode a queued PBUF into wbuf and
 * hand it to write_sub. Every cross-task field in CTCIDEV is written by one side
 * per phase and handed over by a POST (no lock, §3).
 *
 * OWNERSHIP (§3, single-owner PBUFs). Inbound: the decoder copies each IPv4
 * segment into a fresh PBUF (EV_PACKET_RECEIVED); the read buffer is never handed
 * up. Outbound: the PBUF NEVER crosses to the write subtask -- the executive
 * encodes it into wbuf, holds it in d->txpbuf, and frees it exactly once on WRITE
 * completion (or at shutdown if still in flight). Every path queues or frees.
 *
 * MEMORY (§3). No storage service on the hot path: the CTCIDEV, the two
 * subchannel control blocks and the two I/O buffers come from pools reserved once
 * in the init window (ctci_reserve). An inbound PBUF that cannot be allocated is a
 * drop + count, never an ABEND.
 */
#include "nsfctci.h"
#include "nsfctcif.h"
#include "nsfdev.h"
#include "nsfmm.h"
#include "nsfbuf.h"
#include "nsfevt.h"             /* evt_post, EV_PACKET_RECEIVED */
#include "nsfthr.h"             /* nsfthr_* (the I/O subtask seam) */
#include "nsfsts.h"             /* STS_INC / STS_ADD */
#include "nsftrc.h"             /* TRC(DRIVER, ...) */
#include "nsfmsg.h"             /* nsfmsg (NSF2xxE) */

#include <string.h>

/* Init-window pools (spec 9.3): the CTCIDEV blocks, the per-subchannel control
 * blocks (CTCISC, sized by the asm), and the I/O buffers (2 per device: one read,
 * one write). All reserved once by ctci_reserve, before mm_init_complete. */
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

    /* IDENTIFY CTHREAD once (unauthorized; ADR-0022) so the I/O subtasks can be
     * ATTACHed. Host: a no-op. Refuse to reserve if it fails -- no subtasks means
     * no CTCI. */
    if (nsfthr_setup() != 0) {
        nsfmsg("NSF205E CTCI THREAD SEAM SETUP FAILED (IDENTIFY CTHREAD)");
        return 1;
    }

    g_devpool = mm_pool_create("CTCIDEV ", (USHORT)sizeof(CTCIDEV), (USHORT)ndev);
    g_scbpool = mm_pool_create("CTCISCB ", (USHORT)scbsz,   (USHORT)(2u * ndev));
    g_bufpool = mm_pool_create("CTCIBUF ", (USHORT)bufsize, (USHORT)(2u * ndev));

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
    if (d->rbuf) mm_free(g_bufpool, d->rbuf);
    if (d->wbuf) mm_free(g_bufpool, d->wbuf);
    if (d->rscb) mm_free(g_scbpool, d->rscb);
    if (d->wscb) mm_free(g_scbpool, d->wscb);
    mm_free(g_devpool, d);
}

/* Allocate + initialize a CTCIDEV (no channel I/O yet). Validates the MTU cap,
 * mm_allocs the two subchannel control blocks and the two I/O buffers. Returns
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

    d->rscb = mm_alloc(g_scbpool);
    d->wscb = mm_alloc(g_scbpool);
    d->rbuf = (UCHAR *)mm_alloc(g_bufpool);
    d->wbuf = (UCHAR *)mm_alloc(g_bufpool);
    if (d->rscb == NULL || d->wscb == NULL ||
        d->rbuf == NULL || d->wbuf == NULL) {
        nsfmsg("NSF201E CTCI %04X NO BUFFER STORAGE", (unsigned)cuu);
        ctci_dev_release(d);
        return NULL;
    }
    return d;
}

/* ---- inbound: decode one received block into PBUFs ---------------------- */

/* Walk the block just received in `buf` (`len` bytes moved) and hand each IPv4
 * segment up as EV_PACKET_RECEIVED. Copies OUT of the I/O buffer into a right-
 * sized PBUF -- never hands the shared read buffer up. Drop + count (never ABEND)
 * on pool exhaustion, a would-truncate payload, or a codec-flagged bad /
 * non-IPv4 segment. Runs ON THE EXECUTIVE TASK (§9.2/§3). */
static void ctci_decode_block(NETDEV *dev, CTCIDEV *d, const UCHAR *buf, UINT len)
{
    CTCIFDEC     dec;
    const UCHAR *ip;
    USHORT       iplen;
    UINT         idx = dev_index(dev);

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

/* ---- I/O subtasks (ADR-0022): each owns OPEN+EXCP+CLOSE of its subchannel -- */

/* Block until *ecb is posted OR the device is stopping. Returns 1 if posted, 0 if
 * a stop was requested first. Every subtask wait goes through this, so a stop is
 * noticed within one CTCI_POLL_TICKS interval -- which is also the MIH-on-idle
 * tolerance for an outstanding READ. */
static int wait_or_stop(CTCIDEV *d, NSFECB *ecb)
{
    for (;;) {
        /* stop wins: at teardown ctci_stop_and_join POSTs this ECB to wake us
         * promptly, and we abandon any pending completion (the CLOSE that follows
         * purges the outstanding EXCP). In normal operation stop == 0, so this is
         * identical to checking the posted bit first. */
        if (d->stop) {
            return 0;
        }
        if (*ecb & NSFECB_POSTED) {
            return 1;
        }
        nsfthr_timed_wait(ecb, CTCI_POLL_TICKS);
    }
}

/* The read subtask. arg = NETDEV*; d = dev->priv (set before create). */
static int read_sub(void *arg)
{
    NETDEV  *dev = (NETDEV *)arg;
    CTCIDEV *d   = (CTCIDEV *)dev->priv;

    /* OPEN on THIS TCB so the eventual CLOSE purges the EXCP this task issues. */
    d->rstartrc = ctci_open_sub(d->rscb, 0u, d->rddn);
    nsfthr_post(&d->rstartecb, 0u);         /* report the OPEN result to op_start */
    if (d->rstartrc != 0) {
        return d->rstartrc;                 /* OPEN failed: no loop, terminate */
    }

    while (!d->stop) {
        UINT post = 0u;
        UINT res  = 0u;

        d->recb = 0u;
        ctci_read(d->rscb, d->rbuf, d->bufsize, &d->recb);  /* one READ */
        if (!wait_or_stop(d, &d->recb)) {
            break;                          /* stop while a READ is outstanding */
        }
        ctci_status(d->rscb, &post, &res);
        d->rpost = post;
        d->rlen  = (res <= d->bufsize) ? (d->bufsize - res) : 0u;

        d->returnecb = 0u;                  /* arm before asking for the decode */
        d->rready    = 1u;                  /* buffer filled; hand it up */
        nsfthr_post(&dev->ecb, 0u);         /* wake the executive to decode */
        if (!wait_or_stop(d, &d->returnecb)) {
            break;                          /* stop while awaiting the decode */
        }
    }
    ctci_close_sub(d->rscb);                /* same-TCB CLOSE purges the READ */
    return 0;
}

/* The write subtask. arg = NETDEV*; d = dev->priv. */
static int write_sub(void *arg)
{
    NETDEV  *dev = (NETDEV *)arg;
    CTCIDEV *d   = (CTCIDEV *)dev->priv;

    d->wstartrc = ctci_open_sub(d->wscb, 1u, d->wddn);
    nsfthr_post(&d->wstartecb, 0u);
    if (d->wstartrc != 0) {
        return d->wstartrc;
    }

    while (!d->stop) {
        if (!wait_or_stop(d, &d->txgoecb)) {  /* wait for the executive's WRITE */
            break;
        }
        d->txgoecb = 0u;                    /* consume the go signal */
        if (d->stop) {
            break;
        }
        d->wecb = 0u;
        ctci_write(d->wscb, d->wbuf, d->txlen, &d->wecb);   /* one WRITE */
        if (!wait_or_stop(d, &d->wecb)) {
            break;                          /* stop while a WRITE is outstanding */
        }
        /* This subtask OWNS wscb/wecb (ADR-0023): read the completion status HERE
         * and hand only wpost/wready up -- exactly as read_sub does for the read.
         * The executive must never touch wecb, or it would clear the POSTED bit
         * out from under this wait and steal the completion (the live #2-write
         * stall). */
        {
            UINT wpost = 0u;
            UINT wres  = 0u;

            ctci_status(d->wscb, &wpost, &wres);
            d->wpost  = wpost;
            d->wready = 1u;                 /* hand the completion to the executive */
        }
        nsfthr_post(&dev->ecb, 0u);         /* wake the executive to reap */
    }
    ctci_close_sub(d->wscb);
    return 0;
}

/* ---- DEVIO seam (ADR-0021 shape, ADR-0022 semantics) -------------------- */

/* The executive WAITs only on the ONE device ECB it owns (never recb/wecb). */
static int ctci_io_collect(NETDEV *dev, NSFECB **list, int max)
{
    if (max > 0) {
        list[0] = (NSFECB *)&dev->ecb;
        return 1;
    }
    return 0;
}

/* Once per pass, on the executive: decode a completed READ block into PBUFs and
 * release the read subtask to read again; reap a completed WRITE and free its
 * in-flight PBUF exactly once. Both a read and a write can complete in one wake,
 * so check both (not else-if). */
static void ctci_io_service(NETDEV *dev)
{
    CTCIDEV *d = (CTCIDEV *)dev->priv;

    if (d == NULL) {
        return;
    }

    /* read completion: the subtask filled the buffer and posted dev->ecb. */
    if (d->rready) {
        if (d->rpost == CTCI_POST_NORMAL) {
            ctci_decode_block(dev, d, d->rbuf, d->rlen);
        } else {
            ctr(dev->ctr_ierr);
            TRC(DRIVER, "CTCI %04X READ post=%02X (not X'7F')", (unsigned)d->cuu,
                (unsigned)(d->rpost & 0xFFu));
        }
        d->rready = 0u;                     /* consumed */
        nsfthr_post(&d->returnecb, 0u);     /* let the read subtask read again */
    }

    /* write completion: the write subtask read its own status, set wpost, then
     * set wready and posted dev->ecb. The reap keys off wready (a subtask-set
     * flag) and NEVER reads or clears wecb -- wecb belongs to the write subtask's
     * wait (ADR-0023). Reaping on wecb directly would clear the POSTED bit before
     * the subtask observed it, stealing the completion and hanging the subtask
     * after one WRITE. This mirrors the read reap keying off rready, not recb. */
    if (d->wready && d->txbusy) {
        if (d->wpost == CTCI_POST_NORMAL) {
            ctr(dev->ctr_out);
        } else {
            ctr(dev->ctr_oerr);
            TRC(DRIVER, "CTCI %04X WRITE post=%02X (not X'7F')", (unsigned)d->cuu,
                (unsigned)(d->wpost & 0xFFu));
        }
        if (d->txpbuf != NULL) {            /* §3: only the executive frees */
            buf_free(d->txpbuf);
            d->txpbuf = NULL;
        }
        d->wready = 0u;                     /* consumed; ping-ponged like rready */
        d->txbusy = 0u;
    }
}

/* Start at most ONE WRITE from the sendq (one outstanding at a time). The
 * executive encodes the PBUF into the write buffer here -- the PBUF never crosses
 * to the write subtask -- and hands only wbuf/txlen over via txgoecb. */
static void ctci_io_kick(NETDEV *dev)
{
    CTCIDEV *d = (CTCIDEV *)dev->priv;
    QELEM   *qe;
    PBUF    *b;
    UINT     iplen;
    UINT     blklen;
    USHORT   got;

    if (d == NULL || d->txbusy) {
        return;                             /* a WRITE is already outstanding */
    }
    qe = q_deq(&dev->sendq);
    if (qe == NULL) {
        return;                             /* nothing queued */
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
     * then frame in place (ctcif_encode_hdr). buf_copyout stays on the executive. */
    got = buf_copyout(b, d->wbuf + (UINT)sizeof(CTCIHDR) + (UINT)sizeof(CTCISEG),
                      (USHORT)iplen);
    if ((UINT)got != iplen) {               /* short copy (should not happen): drop */
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

    d->txpbuf = b;                          /* executive-owned until reap */
    d->txlen  = blklen;
    d->txbusy = 1u;
    nsfthr_post(&d->txgoecb, 0u);           /* wake the write subtask */
    TRC(DRIVER, "CTCI %04X WRITE %u bytes (ip %u)", (unsigned)d->cuu,
        (unsigned)blklen, (unsigned)iplen);
}

static DEVIO g_ctci_io = {
    ctci_io_collect,
    ctci_io_service,
    ctci_io_kick
};

/* ---- subtask lifecycle helper ------------------------------------------- */

/* Signal stop, wake any subtask parked on an exec->subtask ECB, and join both
 * (bounded). Returns 1 if BOTH joined (safe to free storage), 0 otherwise. */
static int ctci_stop_and_join(CTCIDEV *d)
{
    int joined = 1;

    d->stop = 1u;
    /* Wake the subtask whatever it is waiting on -- an exec->subtask ECB
     * (returnecb / txgoecb) or its own IOB ECB (recb / wecb) idle-blocked with no
     * completion. wait_or_stop checks stop first, so a posted IOB ECB here does
     * NOT decode a phantom block; the subtask just sees stop and CLOSEs (purging
     * its EXCP). Prompt shutdown without waiting out the poll interval. */
    nsfthr_post(&d->recb, 0u);
    nsfthr_post(&d->wecb, 0u);
    nsfthr_post(&d->returnecb, 0u);
    nsfthr_post(&d->txgoecb, 0u);

    if (d->rsub != NULL) {
        if (nsfthr_join(d->rsub, CTCI_JOIN_TICKS) != 0) {
            joined = 0;
        } else {
            d->rsub = NULL;
        }
    }
    if (d->wsub != NULL) {
        if (nsfthr_join(d->wsub, CTCI_JOIN_TICKS) != 0) {
            joined = 0;
        } else {
            d->wsub = NULL;
        }
    }
    return joined;
}

/* ---- DEVOPS ------------------------------------------------------------- */

/* init only attaches the DEVIO seam; the CTCIDEV + subtasks are created at
 * start, so a device that is registered but never started (or whose start fails)
 * holds no pool storage. */
static int ctci_op_init(NETDEV *dev, const DEVCFG *cfg)
{
    (void)cfg;                              /* cuu/mtu come from the NETDEV at start */
    dev_set_io(dev, &g_ctci_io);
    return 0;
}

static int ctci_op_start(NETDEV *dev)
{
    CTCIDEV *d = ctci_dev_alloc(dev->cuu, dev->mtu);

    if (d == NULL) {
        return -1;                          /* no storage / MTU cap: refuse to start */
    }
    if (ctci_chan_alloc(d) != 0) {          /* SVC 99 allocate the CUU pair (MVS) */
        ctci_dev_release(d);
        return -1;
    }
    dev->ecb  = 0u;
    dev->priv = d;                          /* subtasks read dev->priv */

    /* Create the read subtask; it OPENs its subchannel and reports the result. */
    d->rsub = nsfthr_create(read_sub, dev);
    if (d->rsub != NULL) {
        nsfthr_timed_wait(&d->rstartecb, CTCI_START_TICKS);
    }
    if (d->rsub == NULL || !(d->rstartecb & NSFECB_POSTED) || d->rstartrc != 0) {
        nsfmsg("NSF204E CTCI %04X READ SUBCHANNEL OPEN FAILED", (unsigned)d->cuu);
        goto fail;
    }

    d->wsub = nsfthr_create(write_sub, dev);
    if (d->wsub != NULL) {
        nsfthr_timed_wait(&d->wstartecb, CTCI_START_TICKS);
    }
    if (d->wsub == NULL || !(d->wstartecb & NSFECB_POSTED) || d->wstartrc != 0) {
        nsfmsg("NSF204E CTCI %04X WRITE SUBCHANNEL OPEN FAILED", (unsigned)d->wcuu);
        goto fail;
    }

    d->state = CTCI_S_UP;
    nsfmsg("NSF210I CTCI %04X/%04X UP DD %s/%s MTU %u", (unsigned)d->cuu,
           (unsigned)d->wcuu, d->rddn, d->wddn, (unsigned)d->mtu);
    return 0;

fail:
    (void)ctci_stop_and_join(d);            /* stop + join whatever was created */
    ctci_chan_unalloc(d);
    ctci_dev_release(d);
    dev->priv = NULL;
    dev_set_io(dev, NULL);
    return -1;
}

/* send() just hands the PBUF to the bounded sendq via the generic dev_send. For
 * the CTCI DEVIO model, io->kick drains the sendq, so this op is never the
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
    /* Stop + join the subtasks (each CLOSEs its subchannel, purging its own
     * outstanding EXCP -- same TCB). If a subtask will not stop in time, RETAIN
     * the device storage rather than free memory a live subtask still touches
     * (§5.4: a hung device cannot hang shutdown, but must not corrupt). */
    if (!ctci_stop_and_join(d)) {
        nsfmsg("NSF206W CTCI %04X SUBTASK DID NOT STOP -- STORAGE RETAINED",
               (unsigned)d->cuu);
        return 0;                           /* dev->priv left set: not re-freed */
    }
    /* Free the in-flight WRITE PBUF if a WRITE was outstanding at stop (§3). */
    if (d->txpbuf != NULL) {
        buf_free(d->txpbuf);
        d->txpbuf = NULL;
    }
    ctci_chan_unalloc(d);                   /* SVC 99 unallocate both CUUs */
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
 * returns to baseline after shutdown (which=0 dev, 1 scb, 2 buf). */
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
