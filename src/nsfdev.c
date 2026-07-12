/*
 * nsfdev.c -- the Device Abstraction (see nsfdev.h, spec ch. 09).
 *
 * Portable C: the device table is a fixed BSS array (no runtime allocation,
 * §3), dispatch runs through dev->ops, and the three loop-integration hooks
 * (collect_ecbs / poll_input / kick_output) are the only coupling to NSFEVT --
 * registered lazily via evt_set_devices so NSFEVT never names NSFDEV. Concrete
 * drivers (NSFHOST here at M1-2, NSFCTCI at M1-3/M1-4) implement DEVOPS; this
 * file is driver-agnostic.
 *
 * THREADING NOTE (host validation). On the host the inbound producer is the
 * NSFHOST reader thread; on MVS it is the CTCI I/O-completion exit. The only
 * cross-context contact points are: (1) the producer's xq_push onto dev->doneq
 * -- lock-free by construction (NSFXQ: one consumer swaps the whole chain,
 * ABA-safe, see nsfxq.h); and (2) the POST of dev->ecb. NSFMM is touched only
 * on the executive task here (poll_input frees on drop; the handler frees after
 * dispatch), never on the producer -- exactly the single-task storage model the
 * MVS design relies on, so the host test does not race the pools.
 */
#include "nsfdev.h"
#include "nsfevt.h"             /* evt_post, evt_set_devices, nsfevt_wake     */

#include <string.h>            /* memset, memcpy */

/* The device table. g_used[i] marks a claimed slot (distinct from state: a
 * shut-down device stays registered in state DOWN until dev_init resets the
 * table). Both are BSS -- zero at load, so dev_by_index of an unused slot is
 * safely NULL and a zeroed slot reads as state DOWN. */
static NETDEV g_devtab[NSFDEV_MAX];
static UCHAR  g_used[NSFDEV_MAX];

/* Set once the NSFEVT loop seam has been wired (first dev_register). The seam
 * functions tolerate an empty table, so this need never be un-wired. */
static int    g_wired;

/* Guarded counter bump: a per-device counter may be NULL if the statistics
 * registry was full at registration (a build-time miscount, never fatal), so
 * counting must stay safe. */
static void ctr(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

/* Copy an interface name into an 8-char field, NUL-padding the remainder. The
 * caller's DEVCFG.name is a NUL-terminated (or exactly-8) field. */
static void dev_copyname(char dst[8], const char *src)
{
    int i;

    for (i = 0; i < 8 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < 8; i++) {
        dst[i] = '\0';
    }
}

void dev_init(void)
{
    int i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        memset(&g_devtab[i], 0, sizeof(g_devtab[i]));
        g_used[i] = 0;
    }
    /* Force a re-wire on the next dev_register: nsfevt_init clears the loop's
     * device hooks (like it clears the operator seam), so a fresh table must
     * re-register them. dev_register must therefore run after nsfevt_init. */
    g_wired = 0;
}

NETDEV *dev_register(const DEVCFG *cfg, DEVOPS *ops)
{
    NETDEV *dev = NULL;
    int     i, slot = -1;

    if (cfg == NULL || ops == NULL) {
        return NULL;
    }
    for (i = 0; i < NSFDEV_MAX; i++) {
        if (!g_used[i]) {
            slot = i;
            dev  = &g_devtab[i];
            break;
        }
    }
    if (dev == NULL) {
        return NULL;                    /* table full */
    }

    memset(dev, 0, sizeof(*dev));
    dev_copyname(dev->name, cfg->name);
    dev->ops    = ops;
    dev->cuu    = cfg->cuu;
    dev->type   = cfg->type;
    dev->state  = NSFDEV_S_DOWN;
    dev->ipaddr = cfg->ipaddr;
    dev->mtu    = cfg->mtu;
    dev->flags  = cfg->flags;
    q_init(&dev->sendq, NSFDEV_SENDQ_MAX);
    xq_init(&dev->doneq);
    dev->ecb    = 0u;
    dev->priv   = NULL;
    dev->io     = NULL;                  /* default doneq/ecb model until set   */

    /* Per-device counters (component = LINK name). A NULL return only means the
     * registry filled (a build-time miscount); ctr() stays NULL-safe. */
    dev->ctr_in   = sts_register(dev->name, "in");
    dev->ctr_out  = sts_register(dev->name, "out");
    dev->ctr_ierr = sts_register(dev->name, "ierr");
    dev->ctr_oerr = sts_register(dev->name, "oerr");

    if (ops->init != NULL && ops->init(dev, cfg) != 0) {
        memset(dev, 0, sizeof(*dev));   /* release the slot on init failure */
        return NULL;
    }
    g_used[slot] = 1;

    /* Wire the device seam into the loop on the first device: the loop begins
     * WAITing on device ECBs, running poll_input / kick_output each pass, and
     * rechecking work_pending before it commits to WAIT. */
    if (!g_wired) {
        evt_set_devices(nsfdev_collect_ecbs, nsfdev_poll_input,
                        nsfdev_kick_output, nsfdev_work_pending);
        g_wired = 1;
    }
    return dev;
}

NETDEV *dev_find(const char *name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < NSFDEV_MAX; i++) {
        if (g_used[i] && strncmp(g_devtab[i].name, name, 8) == 0) {
            return &g_devtab[i];
        }
    }
    return NULL;
}

NETDEV *dev_find_cuu(USHORT cuu)
{
    int i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        if (g_used[i] && g_devtab[i].cuu == cuu) {
            return &g_devtab[i];
        }
    }
    return NULL;
}

NETDEV *dev_by_index(UINT idx)
{
    if (idx >= (UINT)NSFDEV_MAX || !g_used[idx]) {
        return NULL;
    }
    return &g_devtab[idx];
}

UINT dev_index(const NETDEV *dev)
{
    int i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        if (&g_devtab[i] == dev) {
            return (UINT)i;
        }
    }
    return (UINT)NSFDEV_MAX;             /* not a registered slot */
}

void dev_foreach(void (*fn)(NETDEV *dev, void *arg), void *arg)
{
    int i;

    if (fn == NULL) {
        return;
    }
    for (i = 0; i < NSFDEV_MAX; i++) {
        if (g_used[i]) {
            fn(&g_devtab[i], arg);
        }
    }
}

UINT dev_count(void)
{
    UINT n = 0;
    int  i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        if (g_used[i]) {
            n++;
        }
    }
    return n;
}

int dev_start(NETDEV *dev)
{
    if (dev == NULL) {
        return -1;
    }
    if (dev->state == NSFDEV_S_UP) {
        return 0;                       /* idempotent */
    }
    dev->state = NSFDEV_S_STARTING;
    if (dev->ops->start != NULL && dev->ops->start(dev) != 0) {
        dev->state = NSFDEV_S_DOWN;     /* failed to come up */
        return -1;
    }
    dev->state = NSFDEV_S_UP;
    return 0;
}

int dev_send(NETDEV *dev, PBUF *b)
{
    /* dev_send owns b unconditionally: every path below either queues it or
     * frees it, never both, never neither (§3 single-owner). */
    if (b == NULL) {
        return -1;
    }
    if (dev == NULL || dev->state != NSFDEV_S_UP) {
        buf_free(b);
        if (dev != NULL) {
            ctr(dev->ctr_oerr);
        }
        return -1;
    }
    if (q_enq(&dev->sendq, &b->q) != 0) {   /* bounded: reject when full */
        buf_free(b);
        ctr(dev->ctr_oerr);
        return -1;
    }
    /* Queued. Wake the loop so kick_output runs even for a send issued from
     * outside the loop (a test, or later the request path). */
    nsfevt_wake();
    return 0;
}

int dev_shutdown(NETDEV *dev)
{
    QELEM *qe;
    QELEM *chain;

    if (dev == NULL) {
        return -1;
    }
    if (dev->state == NSFDEV_S_DOWN) {
        return 0;                       /* idempotent */
    }
    dev->state = NSFDEV_S_QUIESCING;

    /* Stop the producer first (reader thread / EXCP), so no further push lands
     * on doneq while we drain it. */
    if (dev->ops->shutdown != NULL) {
        dev->ops->shutdown(dev);
    }

    /* Free every PBUF still held by the NETDEV-owned queues (leak gate). */
    while ((qe = q_deq(&dev->sendq)) != NULL) {
        buf_free(Q_ENTRY(qe, PBUF, q));
    }
    chain = xq_drain(&dev->doneq);
    while (chain != NULL) {
        QELEM *nx = chain->next;
        buf_free(Q_ENTRY(chain, PBUF, q));
        chain = nx;
    }

    dev->ecb   = 0u;
    dev->state = NSFDEV_S_DOWN;
    return 0;
}

void dev_set_io(NETDEV *dev, DEVIO *io)
{
    if (dev != NULL) {
        dev->io = io;
    }
}

/* --- NSFEVT main-loop integration ----------------------------------------- */
/* Each of the three hooks below runs the DEFAULT doneq/ecb model for a device
 * with io == NULL (NSFHOST), and delegates to the device's DEVIO seam when it is
 * set (CTCI, ADR-0021) -- so NSFDEV stays driver-agnostic (it never names CTCI;
 * it calls through the pointers the driver installed). */

int nsfdev_collect_ecbs(NSFECB **list, int max)
{
    int i, n = 0;

    for (i = 0; i < NSFDEV_MAX; i++) {
        if (!g_used[i]) {
            continue;
        }
        if (g_devtab[i].io != NULL && g_devtab[i].io->collect != NULL) {
            /* Driver contributes its own completion ECB(s) (CTCI: read+write). */
            if (n < max) {
                n += g_devtab[i].io->collect(&g_devtab[i], &list[n], max - n);
            }
        } else if (n < max) {
            list[n++] = (NSFECB *)&g_devtab[i].ecb;   /* default single ECB */
        }
    }
    return n;
}

void nsfdev_poll_input(void)
{
    int i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        NETDEV *dev;
        QELEM  *chain;
        QELEM  *prev;
        QELEM  *cur;

        if (!g_used[i]) {
            continue;
        }
        dev = &g_devtab[i];

        /* ECB-completion driver (CTCI, ADR-0022): the DEVIO service decodes the
         * subtask-delivered block and reaps a completed WRITE. Clear dev->ecb
         * BEFORE servicing -- exactly as the default doneq path below clears it
         * before draining -- so a stale posted dev->ecb never lingers in the
         * executive's WAIT ECBLIST. A lingering posted ECB corrupts the multi-ECB
         * WAIT so a later operator/stop POST no longer wakes it (the #18 hazard,
         * re-introduced if this clear is missing). Lost-wakeup safe: the read path
         * is serialized by the returnecb handshake (the subtask cannot re-post
         * dev->ecb until service posts returnecb), and a write-completion post
         * landing after the clear simply leaves it set for the next WAIT. */
        if (dev->io != NULL && dev->io->service != NULL) {
            dev->ecb = 0u;
            dev->io->service(dev);
            continue;
        }

        /* Default model: clear the device ECB BEFORE draining (lost-wakeup safe:
         * a producer that pushes+posts in the window between drain and the next
         * clear leaves the ECB set, so the next WAIT returns and picks it up). */
        dev->ecb = 0u;

        chain = xq_drain(&dev->doneq);  /* LIFO */
        prev  = NULL;
        while (chain != NULL) {         /* reverse to FIFO (arrival order) */
            QELEM *nx = chain->next;
            chain->next = prev;
            prev  = chain;
            chain = nx;
        }
        for (cur = prev; cur != NULL; ) {
            QELEM *nx = cur->next;      /* capture before evt_post reuses none */
            PBUF  *b  = Q_ENTRY(cur, PBUF, q);

            if (evt_post(EV_PACKET_RECEIVED, b, (UINT)i) == 0) {
                ctr(dev->ctr_in);
            } else {
                /* EVT pool exhausted: drop + count, never an ABEND (§3). The
                 * PBUF is ours here, so free it. */
                buf_free(b);
                ctr(dev->ctr_ierr);
            }
            cur = nx;
        }
    }
}

int nsfdev_work_pending(void)
{
    int i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        NETDEV *dev;

        if (!g_used[i]) {
            continue;
        }
        dev = &g_devtab[i];
        if (dev->io != NULL) {
            /* DEVIO driver: its probe mirrors its service's consume conditions
             * (CTCI: a filled read block / an unreaped WRITE). */
            if (dev->io->pending != NULL && dev->io->pending(dev) != 0) {
                return 1;
            }
        } else if (dev->doneq.head != NULL) {
            /* Default model: completed inbound I/O awaiting the doneq drain. */
            return 1;
        }
    }
    return 0;
}

void nsfdev_kick_output(void)
{
    int i;

    for (i = 0; i < NSFDEV_MAX; i++) {
        NETDEV *dev;
        QELEM  *qe;

        if (!g_used[i]) {
            continue;
        }
        dev = &g_devtab[i];
        if (dev->state != NSFDEV_S_UP) {
            continue;                   /* only an up device transmits */
        }

        /* ECB-completion driver (CTCI): the DEVIO kick starts at most one
         * outstanding I/O (one WRITE), counting ctr_out itself when it does.
         * It must not be drained through the generic ops->send loop below,
         * which would issue a WRITE per queued PBUF. */
        if (dev->io != NULL && dev->io->kick != NULL) {
            dev->io->kick(dev);
            continue;
        }

        while ((qe = q_deq(&dev->sendq)) != NULL) {
            PBUF *b = Q_ENTRY(qe, PBUF, q);

            /* ops->send takes ownership of b: on success it is transmitted (and
             * for loopback loops back inbound); on immediate error the driver
             * has already freed b and counted ctr_oerr. */
            if (dev->ops->send(dev, b) == 0) {
                ctr(dev->ctr_out);
            }
        }
    }
}
