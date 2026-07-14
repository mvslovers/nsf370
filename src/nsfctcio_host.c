/*
 * nsfctcio_host.c -- host stand-in for the CTCI top half (asm/nsfctcio.asm),
 * swapped in by the project.toml [host].replace map (exactly as nsfxq.asm ->
 * nsfxq_host.c). Host test builds only; never compiled by cc370.
 *
 * WHAT IT PROVES, AND WHAT IT CANNOT. It lets test/tstctci.c drive the portable
 * CTCI bottom half (ping-pong READ, completion demux, decode->PBUF, sendq->WRITE,
 * ownership, MIH idle) against a MODEL of the channel. It validates the driver's
 * LOGIC. It CANNOT catch a wrong CCW flag, a bad IOB field, or an EXCP linkage
 * fault -- those are caught only by the real 3088 device (TSTCTCM on MVSCE, the
 * runbook), never here.
 *
 * The model is faithful to Hercules ctc_ctci.c as corrected by ADR-0020:
 *   - A READ delivers ONE block (CTCIHDR + N segments, leading hwOffset =
 *     end-of-data, NO 0x0000 terminator to the guest) -- the tests inject such
 *     blocks byte-for-byte.
 *   - ctci_read CLEARS the ECB on arm (models the ASM CISTART XC) BEFORE it
 *     may post: without this the bottom half's stale-ECB phantom guard would be
 *     vacuous.
 *   - If no frame is queued, a READ is left OUTSTANDING (not posted): Hercules
 *     blocks the guest until a frame arrives, so ctcio_host_inject completes an
 *     outstanding READ immediately (a frame "arriving"), else queues the block
 *     for the next READ.
 *   - A forced post code / short residual models an I/O error for the driver's
 *     error-path test.
 */
#include "nsfctci.h"
#include "nsfthr.h"             /* NSFECB, nsfthr_post (wake the waiting subtask) */

#include <string.h>
#include <pthread.h>

#define HSCB_INJECT_MAX  8      /* queued inbound blocks awaiting a READ        */
#define HSCB_CAP_MAX     2048   /* captured outbound block (test frames small)  */

/* THREAD SAFETY (test-only). On MVS the channel subsystem serializes the read
 * subtask's EXCP-arm against IOS frame delivery; the host models both as plain
 * struct writes, so the read subtask thread (ctci_read/ctci_write on re-arm) and
 * the executive/test thread (ctcio_host_inject / ctci_halt_read / the ctcio_host_*
 * probes) would race on one HOSTSCB with no serialization -- a data race that
 * corrupts the arm/deliver handshake (e.g. inject queues a block while ctci_read
 * arms, and neither delivers it) and is the sole cause of the sequencing/lostwake
 * flake. One mutex restores that serialization. It guards HOSTSCB state only;
 * every wake (nsfthr_post, which takes the thread-seam mutex) is issued AFTER the
 * unlock, so this lock nests strictly ABOVE the thread-seam lock and can never
 * deadlock. Production is unaffected -- it uses asm/nsfctcio.asm, never this shim. */
static pthread_mutex_t g_hscb = PTHREAD_MUTEX_INITIALIZER;

/* The per-subchannel control block the C bottom half mm_allocs (ctci_scb_size
 * bytes). On the host it is this model; on MVS it is a DCB copy + IOB + CCW. */
typedef struct hostscb {
    int          forwrite;                 /* 0 = read subchannel, else write    */

    /* inbound (read subchannel): frames queued while no READ is outstanding, and
     * the parameters of an outstanding READ awaiting a frame. */
    const UCHAR *inq[HSCB_INJECT_MAX];
    UINT         inqlen[HSCB_INJECT_MAX];
    int          inhead, intail, incount;
    int          outstanding;              /* a READ is armed with no frame yet  */
    UCHAR       *pend_buf;                 /* the outstanding READ's target       */
    UINT         pend_len;                 /* its requested length                */
    UINT        *pend_ecb;                 /* its ECB                             */

    /* last completion status (read back by ctci_status). */
    UINT         post;
    UINT         residual;

    /* forced completion for the next READ/WRITE (error-path test). */
    int          force;
    UCHAR        force_post;
    UINT         force_resid;

    /* outbound (write subchannel): the last block written, for the test to
     * inspect the framing the driver produced. */
    UCHAR        cap[HSCB_CAP_MAX];
    UINT         caplen;
} HOSTSCB;

/* Prepare a READ completion UNDER g_hscb: copy the block into `buf` and set the
 * completion status so the bottom half computes (requested - residual) == block
 * length. Does NOT post -- the caller posts the ECB after releasing g_hscb (so the
 * wake, which takes the thread-seam mutex, never nests under g_hscb). */
static void hscb_prep_read(HOSTSCB *h, UCHAR *buf, UINT buflen,
                           const UCHAR *blk, UINT blklen)
{
    UINT n = (blklen <= buflen) ? blklen : buflen;

    memcpy(buf, blk, n);
    if (h->force) {
        h->post     = (UINT)h->force_post;
        h->residual = h->force_resid;
        h->force    = 0;
    } else {
        h->post     = (UINT)CTCI_POST_NORMAL;
        h->residual = (blklen <= buflen) ? (buflen - blklen) : 0u;
    }
}

/* ---- top-half primitives (the asm() interface) --------------------------- */

UINT ctci_scb_size(void)
{
    return (UINT)sizeof(HOSTSCB);
}

int ctci_open_sub(void *scb, UINT forwrite, const char *ddname8)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    (void)ddname8;
    pthread_mutex_lock(&g_hscb);
    memset(h, 0, sizeof(*h));               /* a clean, opened subchannel        */
    h->forwrite = (forwrite != 0u);
    pthread_mutex_unlock(&g_hscb);
    return 0;
}

int ctci_read(void *scb, void *buf, UINT len, UINT *ecb)
{
    HOSTSCB *h        = (HOSTSCB *)scb;
    UINT    *to_post  = NULL;

    pthread_mutex_lock(&g_hscb);
    *ecb = 0u;                              /* clear on arm (models CISTART XC)   */

    if (h->incount > 0) {                   /* a frame is already waiting          */
        const UCHAR *blk    = h->inq[h->inhead];
        UINT         blklen = h->inqlen[h->inhead];
        h->inhead = (h->inhead + 1) % HSCB_INJECT_MAX;
        h->incount--;
        h->outstanding = 0;
        hscb_prep_read(h, (UCHAR *)buf, len, blk, blklen);
        to_post = ecb;                      /* completed now: post after unlock    */
    } else {                                /* leave the READ outstanding          */
        h->outstanding = 1;
        h->pend_buf    = (UCHAR *)buf;
        h->pend_len    = len;
        h->pend_ecb    = ecb;
    }
    pthread_mutex_unlock(&g_hscb);
    if (to_post != NULL) {
        nsfthr_post((NSFECB *)to_post, 0u); /* wake the subtask's nsfthr_wait      */
    }
    return 0;
}

int ctci_write(void *scb, void *buf, UINT len, UINT *ecb)
{
    HOSTSCB *h = (HOSTSCB *)scb;
    UINT     n = (len <= (UINT)sizeof(h->cap)) ? len : (UINT)sizeof(h->cap);

    pthread_mutex_lock(&g_hscb);
    memcpy(h->cap, buf, n);
    h->caplen = len;
    *ecb = 0u;                              /* clear on arm (models CISTART XC)   */
    if (h->force) {
        h->post     = (UINT)h->force_post;
        h->residual = h->force_resid;
        h->force    = 0;
    } else {
        h->post     = (UINT)CTCI_POST_NORMAL;
        h->residual = 0u;                   /* whole block written                 */
    }
    pthread_mutex_unlock(&g_hscb);
    nsfthr_post((NSFECB *)ecb, 0u);         /* wake the subtask's nsfthr_wait      */
    return 0;
}

void ctci_status(void *scb, UINT *postcode, UINT *residual)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    pthread_mutex_lock(&g_hscb);
    if (postcode) {
        *postcode = h->post;
    }
    if (residual) {
        *residual = h->residual;
    }
    pthread_mutex_unlock(&g_hscb);
}

int ctci_close_sub(void *scb)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    pthread_mutex_lock(&g_hscb);
    h->outstanding = 0;
    h->incount = h->inhead = h->intail = 0;
    pthread_mutex_unlock(&g_hscb);
    return 0;
}

/* Host analog of the MVS IOHALT (asm ctci_halt_read): actively park the read by
 * completing its outstanding EXCP with post X'48' (purged) and NO data -- exactly
 * as IOS reclassifies a halted read (ADR-0027). If NO read is outstanding (a
 * frame already completed it -- the halt/data race), this is a harmless no-op and
 * the data completion flows normally, which is precisely the race the driver must
 * tolerate. rucb models the MVS UCB target and is unused here; the halt reaches
 * the read purely through rscb. */
void ctci_halt_read(void *rscb, UINT rucb)
{
    HOSTSCB *h       = (HOSTSCB *)rscb;
    UINT    *to_post = NULL;

    (void)rucb;
    pthread_mutex_lock(&g_hscb);
    if (h->outstanding) {
        h->outstanding = 0;
        h->post        = (UINT)CTCI_POST_PURGED;   /* X'48' -- purged           */
        h->residual    = h->pend_len;              /* no data transferred       */
        to_post        = h->pend_ecb;              /* post after unlock          */
    }
    pthread_mutex_unlock(&g_hscb);
    if (to_post != NULL) {
        nsfthr_post((NSFECB *)to_post, 0u);        /* wake the read subtask      */
    }
}

/* ---- test hooks (host only) ---------------------------------------------- */

/* A frame "arrives" at the read subchannel: complete an outstanding READ at
 * once (Hercules unblocks the guest), else queue it for the next READ. The block
 * memory is the caller's -- it must outlive the READ that consumes it. */
void ctcio_host_inject(void *rscb, const UCHAR *blk, UINT len)
{
    HOSTSCB *h       = (HOSTSCB *)rscb;
    UINT    *to_post = NULL;

    pthread_mutex_lock(&g_hscb);
    if (h->outstanding) {
        h->outstanding = 0;
        hscb_prep_read(h, h->pend_buf, h->pend_len, blk, len);
        to_post = h->pend_ecb;                 /* completed now: post after unlock */
    } else if (h->incount < HSCB_INJECT_MAX) {
        h->inq[h->intail]    = blk;
        h->inqlen[h->intail] = len;
        h->intail = (h->intail + 1) % HSCB_INJECT_MAX;
        h->incount++;
    }
    pthread_mutex_unlock(&g_hscb);
    if (to_post != NULL) {
        nsfthr_post((NSFECB *)to_post, 0u);    /* wake the read subtask's wait     */
    }
}

/* Read back the last block the driver WROTE (for framing assertions). Called
 * after the WRITE has completed (ctr_out advanced), so the buffer content is
 * stable; the lock just publishes caplen safely. */
const UCHAR *ctcio_host_captured(void *wscb, UINT *len)
{
    HOSTSCB *h = (HOSTSCB *)wscb;

    pthread_mutex_lock(&g_hscb);
    if (len) {
        *len = h->caplen;
    }
    pthread_mutex_unlock(&g_hscb);
    return h->cap;
}

/* Force the next completion on this subchannel to carry `post` / `residual`
 * (models an I/O error or a short read for the driver's error path). */
void ctcio_host_force(void *scb, UCHAR post, UINT residual)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    pthread_mutex_lock(&g_hscb);
    h->force       = 1;
    h->force_post  = post;
    h->force_resid = residual;
    pthread_mutex_unlock(&g_hscb);
}

/* Is a READ currently outstanding (armed, no frame yet)? A test barrier waits on
 * this before triggering a halt, so the halt reliably purges an armed READ. */
int ctcio_host_outstanding(void *rscb)
{
    HOSTSCB *h = (HOSTSCB *)rscb;
    int      r;

    pthread_mutex_lock(&g_hscb);
    r = h->outstanding;
    pthread_mutex_unlock(&g_hscb);
    return r;
}
