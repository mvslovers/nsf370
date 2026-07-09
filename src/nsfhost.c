/*
 * nsfhost.c -- the HOST device driver (see nsfhost.h, spec ch. 9.4).
 *
 * HOST-ONLY. Never compiled by cc370: the project.toml [host].replace map swaps
 * the MVS placeholder src/nsfhost_plat.c for this file on the native test build.
 * pthread is sanctioned here because this is host-native test scaffolding, not
 * target code -- exactly as src/nsfevt_plat_host.c / src/nsfxq_host.c are.
 *
 * The value it delivers: a DEVOPS driver whose inbound path is the host analog
 * of the MVS CTCI I/O-completion exit. A reader THREAD -- not a synchronous
 * call -- is the async producer: it takes a received PBUF, hands it to the
 * device doneq (xq_push, lock-free) and POSTs the device ECB. The executive
 * loop drains the doneq up to EV_PACKET_RECEIVED (nsfdev_poll_input). So the
 * doneq -> EV_PACKET_RECEIVED handoff is exercised across a real thread
 * boundary before any Hercules device exists; M1-3 swaps only the producer.
 *
 * OWNERSHIP / THREAD SAFETY. NSFMM is touched only on the executive task: the
 * outbound PBUF the send side hands over becomes the inbound PBUF the reader
 * relays (loopback is copy-free -- the wire moves the same raw IP bytes), and
 * it is freed by the EV_PACKET_RECEIVED handler back on the executive task. The
 * reader NEVER calls a storage service (it only relays a pre-existing buffer),
 * so it cannot race the pools -- the same single-task storage rule the MVS
 * design relies on (the CTCI exit likewise does not allocate; §9.2 has the
 * executive bottom half allocate). The only cross-thread contacts are the
 * lock-free xq_push onto doneq and the ECB POST.
 */
#include "nsfhost.h"
#include "nsfdev.h"
#include "nsfbuf.h"             /* buf_free */
#include "nsfxq.h"              /* xq_push onto the device doneq */
#include "nsfevtp.h"            /* nsfevt_plat_post (wake the loop) */
#include "nsfsts.h"             /* STS_INC */

#include <pthread.h>
#include <string.h>

#ifdef NSFHOST_TUN
#include <unistd.h>            /* read, write, close */
#include <fcntl.h>
#endif

/* Internal loopback "wire" depth. >= NSFDEV_SENDQ_MAX so a full outbound queue
 * can drain into the wire in one kick without back-pressuring mid-drain (real
 * write-completion back-pressure is an M1-4 concern). */
#define HOST_WIRE_MAX   64

/* Host devices sharing the static driver-private pool. Host tests use one or
 * two; a small fixed pool keeps nsfhost allocation-free like the rest of NSF. */
#define HOST_DEV_MAX    4

/* Driver private block, one per host device (dev->priv points here). */
typedef struct hostpriv {
    int             used;               /* slot claimed                     */
    UCHAR           mode;               /* NSFHOST_LOOPBACK / _TUN / _PCAP   */
    volatile int    running;            /* reader thread should keep going   */
    NETDEV         *dev;                /* back-pointer for the reader thread */

    /* The loopback wire: a bounded ring of PBUF*, guarded by wmtx/wcv. The send
     * side (executive) enqueues; the reader thread dequeues. Holds PBUF* only --
     * the PBUF's own q linkage stays free for the doneq push. */
    PBUF           *wire[HOST_WIRE_MAX];
    int             whead, wtail, wcount;
    pthread_mutex_t wmtx;
    pthread_cond_t  wcv;

    pthread_t       reader;
    int             reader_started;

#ifdef NSFHOST_TUN
    int             tunfd;              /* -1 until start opens it            */
#endif
} HOSTPRIV;

static HOSTPRIV g_priv[HOST_DEV_MAX];

/* --- private-block pool (executive task only, no locking) ----------------- */

static HOSTPRIV *priv_alloc(void)
{
    int i;

    for (i = 0; i < HOST_DEV_MAX; i++) {
        if (!g_priv[i].used) {
            memset(&g_priv[i], 0, sizeof(g_priv[i]));
            g_priv[i].used = 1;
            return &g_priv[i];
        }
    }
    return NULL;
}

static void priv_free(HOSTPRIV *p)
{
    if (p != NULL) {
        p->used = 0;
    }
}

/* NULL-safe counter bump (a per-device counter may be NULL if the stats
 * registry filled at registration). */
static void ctr(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

/* Deliver one received PBUF the way the CTCI exit will: push it onto the device
 * doneq and POST the device ECB. No storage service is called here. */
static void host_deliver(HOSTPRIV *p, PBUF *b)
{
    xq_push(&p->dev->doneq, &b->q);
    nsfevt_plat_post((NSFECB *)&p->dev->ecb);
}

/* --- reader thread -------------------------------------------------------- */

/* Loopback source: relay each frame the send side placed on the wire back
 * inbound. Drains any remaining frames after `running` clears so shutdown loses
 * nothing, then exits. */
static void host_reader_loopback(HOSTPRIV *p)
{
    for (;;) {
        PBUF *b;

        pthread_mutex_lock(&p->wmtx);
        while (p->running && p->wcount == 0) {
            pthread_cond_wait(&p->wcv, &p->wmtx);
        }
        if (!p->running && p->wcount == 0) {
            pthread_mutex_unlock(&p->wmtx);
            return;                     /* stopped and drained */
        }
        b = p->wire[p->whead];
        p->wire[p->whead] = NULL;
        p->whead = (p->whead + 1) % HOST_WIRE_MAX;
        p->wcount--;
        pthread_mutex_unlock(&p->wmtx);

        if (b != NULL) {
            host_deliver(p, b);         /* same PBUF loops back inbound */
        }
    }
}

#ifdef NSFHOST_TUN
/* TUN source: read raw IP frames from the tun fd. Unlike loopback there is no
 * pre-existing PBUF, so the reader must allocate one here. This mode is not part
 * of the CI leak-gate test; it is the live-traffic path (compiled with
 * -DNSFHOST_TUN). */
static void host_reader_tun(HOSTPRIV *p)
{
    while (p->running) {
        UCHAR   frame[2048];
        ssize_t n = read(p->tunfd, frame, sizeof(frame));
        PBUF   *b;

        if (n <= 0) {
            continue;                   /* EINTR / no data: retry */
        }
        b = buf_alloc((USHORT)n);       /* executive-independent RX buffer */
        if (b == NULL) {
            ctr(p->dev->ctr_ierr);      /* pool exhausted: drop + count */
            continue;
        }
        buf_reset_rx(b);
        buf_copyin(b, frame, (USHORT)n);
        host_deliver(p, b);
    }
}
#endif

static void *host_reader(void *arg)
{
    HOSTPRIV *p = (HOSTPRIV *)arg;

#ifdef NSFHOST_TUN
    if (p->mode == NSFHOST_TUN) {
        host_reader_tun(p);
        return NULL;
    }
#endif
    host_reader_loopback(p);
    return NULL;
}

/* --- DEVOPS ---------------------------------------------------------------- */

static int host_init(NETDEV *dev, const DEVCFG *cfg)
{
    const HOSTCFG *hc = (cfg != NULL) ? (const HOSTCFG *)cfg->drvcfg : NULL;
    HOSTPRIV      *p  = priv_alloc();

    if (p == NULL) {
        return -1;                      /* host device pool full */
    }
    p->mode    = (hc != NULL) ? hc->mode : (UCHAR)NSFHOST_LOOPBACK;
    p->dev     = dev;
    p->running = 0;
    p->whead   = 0;
    p->wtail   = 0;
    p->wcount  = 0;
    p->reader_started = 0;
    pthread_mutex_init(&p->wmtx, NULL);
    pthread_cond_init(&p->wcv, NULL);

    /* Reject a mode not compiled into this build (leaving loopback always
     * available). */
    if (p->mode == NSFHOST_LOOPBACK) {
        /* always available */
    }
#ifdef NSFHOST_TUN
    else if (p->mode == NSFHOST_TUN) {
        p->tunfd = -1;
    }
#endif
    else {
        pthread_mutex_destroy(&p->wmtx);
        pthread_cond_destroy(&p->wcv);
        priv_free(p);
        return -1;                      /* unsupported mode in this build */
    }

    dev->priv = p;
    return 0;
}

static int host_start(NETDEV *dev)
{
    HOSTPRIV *p = (HOSTPRIV *)dev->priv;

    if (p == NULL) {
        return -1;
    }
#ifdef NSFHOST_TUN
    if (p->mode == NSFHOST_TUN) {
        p->tunfd = open("/dev/net/tun", O_RDWR);     /* Linux; macOS differs */
        if (p->tunfd < 0) {
            return -1;
        }
    }
#endif
    p->running = 1;
    if (pthread_create(&p->reader, NULL, host_reader, p) != 0) {
        p->running = 0;
        return -1;
    }
    p->reader_started = 1;
    return 0;
}

static int host_send(NETDEV *dev, PBUF *b)
{
    HOSTPRIV *p = (HOSTPRIV *)dev->priv;

    /* send() takes ownership of b unconditionally (spec 9.2). */
    if (p == NULL) {
        buf_free(b);
        ctr(dev->ctr_oerr);
        return -1;
    }

#ifdef NSFHOST_TUN
    if (p->mode == NSFHOST_TUN) {
        UCHAR  frame[2048];
        USHORT n = buf_copyout(b, frame, (USHORT)sizeof(frame));
        buf_free(b);                    /* transmitted (or dropped): we own it */
        if (write(p->tunfd, frame, n) != (ssize_t)n) {
            ctr(dev->ctr_oerr);
            return -1;
        }
        return 0;
    }
#endif

    /* Loopback: hand the frame to the wire; the reader thread relays it back
     * inbound. A full wire is back-pressure: drop + count (never grow, §3). */
    pthread_mutex_lock(&p->wmtx);
    if (p->wcount >= HOST_WIRE_MAX) {
        pthread_mutex_unlock(&p->wmtx);
        buf_free(b);
        ctr(dev->ctr_oerr);
        return -1;
    }
    p->wire[p->wtail] = b;
    p->wtail = (p->wtail + 1) % HOST_WIRE_MAX;
    p->wcount++;
    pthread_cond_signal(&p->wcv);
    pthread_mutex_unlock(&p->wmtx);
    return 0;
}

static int host_shutdown(NETDEV *dev)
{
    HOSTPRIV *p = (HOSTPRIV *)dev->priv;
    int       k;

    if (p == NULL) {
        return 0;
    }
    /* Stop the reader thread (it drains the wire onto the doneq first, so no
     * frame is lost), then join. */
    if (p->reader_started) {
        pthread_mutex_lock(&p->wmtx);
        p->running = 0;
        pthread_cond_broadcast(&p->wcv);
        pthread_mutex_unlock(&p->wmtx);
        pthread_join(p->reader, NULL);
        p->reader_started = 0;
    }
    /* Free any frame still held by the wire (driver-owned). Normally empty: the
     * reader drains the wire during stop; this is the safety net. The doneq is
     * NETDEV-owned and drained by dev_shutdown after this returns. */
    for (k = 0; k < HOST_WIRE_MAX; k++) {
        if (p->wire[k] != NULL) {
            buf_free(p->wire[k]);
            p->wire[k] = NULL;
        }
    }
    p->whead = p->wtail = p->wcount = 0;

#ifdef NSFHOST_TUN
    if (p->mode == NSFHOST_TUN && p->tunfd >= 0) {
        close(p->tunfd);
        p->tunfd = -1;
    }
#endif

    pthread_mutex_destroy(&p->wmtx);
    pthread_cond_destroy(&p->wcv);
    priv_free(p);
    dev->priv = NULL;
    return 0;
}

static DEVOPS g_host_ops = {
    host_init,
    host_start,
    host_send,
    host_shutdown
};

DEVOPS *nsfhost_ops(void)
{
    return &g_host_ops;
}
