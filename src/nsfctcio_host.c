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
#include "nsfevtp.h"            /* NSFECB, nsfevt_plat_post (wake the loop)    */

#include <string.h>

#define HSCB_INJECT_MAX  8      /* queued inbound blocks awaiting a READ        */
#define HSCB_CAP_MAX     2048   /* captured outbound block (test frames small)  */

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

/* Complete a READ: copy the block into `buf`, set the completion status so the
 * bottom half computes (requested - residual) == block length, and post the ECB
 * (which also wakes a WAITing loop). */
static void hscb_complete_read(HOSTSCB *h, UCHAR *buf, UINT buflen, UINT *ecb,
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
    *ecb = NSFECB_POSTED;
    nsfevt_plat_post((NSFECB *)ecb);
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
    memset(h, 0, sizeof(*h));               /* a clean, opened subchannel        */
    h->forwrite = (forwrite != 0u);
    return 0;
}

int ctci_read(void *scb, void *buf, UINT len, UINT *ecb)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    *ecb = 0u;                              /* clear on arm (models CISTART XC)   */

    if (h->incount > 0) {                   /* a frame is already waiting          */
        const UCHAR *blk    = h->inq[h->inhead];
        UINT         blklen = h->inqlen[h->inhead];
        h->inhead = (h->inhead + 1) % HSCB_INJECT_MAX;
        h->incount--;
        h->outstanding = 0;
        hscb_complete_read(h, (UCHAR *)buf, len, ecb, blk, blklen);
    } else {                                /* leave the READ outstanding          */
        h->outstanding = 1;
        h->pend_buf    = (UCHAR *)buf;
        h->pend_len    = len;
        h->pend_ecb    = ecb;
    }
    return 0;
}

int ctci_write(void *scb, void *buf, UINT len, UINT *ecb)
{
    HOSTSCB *h = (HOSTSCB *)scb;
    UINT     n = (len <= (UINT)sizeof(h->cap)) ? len : (UINT)sizeof(h->cap);

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
    *ecb = NSFECB_POSTED;
    nsfevt_plat_post((NSFECB *)ecb);
    return 0;
}

void ctci_status(void *scb, UINT *postcode, UINT *residual)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    if (postcode) {
        *postcode = h->post;
    }
    if (residual) {
        *residual = h->residual;
    }
}

int ctci_close_sub(void *scb)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    h->outstanding = 0;
    h->incount = h->inhead = h->intail = 0;
    return 0;
}

/* ---- test hooks (host only) ---------------------------------------------- */

/* A frame "arrives" at the read subchannel: complete an outstanding READ at
 * once (Hercules unblocks the guest), else queue it for the next READ. The block
 * memory is the caller's -- it must outlive the READ that consumes it. */
void ctcio_host_inject(void *rscb, const UCHAR *blk, UINT len)
{
    HOSTSCB *h = (HOSTSCB *)rscb;

    if (h->outstanding) {
        h->outstanding = 0;
        hscb_complete_read(h, h->pend_buf, h->pend_len, h->pend_ecb, blk, len);
    } else if (h->incount < HSCB_INJECT_MAX) {
        h->inq[h->intail]    = blk;
        h->inqlen[h->intail] = len;
        h->intail = (h->intail + 1) % HSCB_INJECT_MAX;
        h->incount++;
    }
}

/* Read back the last block the driver WROTE (for framing assertions). */
const UCHAR *ctcio_host_captured(void *wscb, UINT *len)
{
    HOSTSCB *h = (HOSTSCB *)wscb;

    if (len) {
        *len = h->caplen;
    }
    return h->cap;
}

/* Force the next completion on this subchannel to carry `post` / `residual`
 * (models an I/O error or a short read for the driver's error path). */
void ctcio_host_force(void *scb, UCHAR post, UINT residual)
{
    HOSTSCB *h = (HOSTSCB *)scb;

    h->force       = 1;
    h->force_post  = post;
    h->force_resid = residual;
}

/* Is a READ currently outstanding (armed, no frame yet)? The MIH-idle test uses
 * this to confirm a valid READ remains after an idle period. */
int ctcio_host_outstanding(void *rscb)
{
    HOSTSCB *h = (HOSTSCB *)rscb;

    return h->outstanding;
}
