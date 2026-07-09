/*
 * tstdev.c -- NSFDEV + NSFHOST host unit tests (spec ch. 09, M1-2).
 *
 * Drives the device abstraction end-to-end over the HOST loopback driver, so
 * the async completion handoff (device doneq -> EV_PACKET_RECEIVED) is exercised
 * across a real thread boundary before any Hercules device exists:
 *
 *   1. full send -> receive cycle: register a HOST loopback device via DEVOPS,
 *      start it, send N PBUFs; each returns as EV_PACKET_RECEIVED through the
 *      device doneq + ECB (produced by the NSFHOST reader thread -- the host
 *      analog of the CTCI I/O exit), in order, with payload intact; counters
 *      ctr_out/ctr_in == N;
 *   2. bounded sendq: sends past NSFDEV_SENDQ_MAX are rejected (freed + counted
 *      ctr_oerr), never grown;
 *   3. shutdown drains cleanly: all pools return to baseline (the leak gate).
 *
 * The loop code (NSFEVT) never names HOST or CTCI -- it services the device only
 * through the NSFDEV seam (evt_set_devices), so this same test structure will
 * validate the CTCI driver unchanged (M1-3+).
 *
 * PORTABLE. nsfhost_ops() returns NULL where there is no host driver (the MVS
 * build, src/nsfhost_plat.c); the test then reports the host path is unavailable
 * on this platform and passes trivially (CTCI validates the abstraction on MVS).
 * The pthread reader lives inside src/nsfhost.c, so this test source itself is
 * plain portable C.
 */
#include "nsfdev.h"
#include "nsfhost.h"
#include "nsfevt.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include <mbtcheck.h>
#include <string.h>

#define TSTDEV_N    5           /* packets per flow scenario (< drain budget) */

/* ---- EV_PACKET_RECEIVED handler (scenario 1) ---- */
static UINT    g_rx_count;
static int     g_rx_seq[TSTDEV_N + 4];  /* payload tag order received */
static int     g_rx_devok;              /* every u1 resolved to the device */
static NETDEV *g_rx_expect;

static void h_rx(EVT *ev)
{
    PBUF  *b = (PBUF *)ev->p1;
    NETDEV *d = dev_by_index(ev->u1);
    UCHAR  got[4];

    if (d != g_rx_expect) {
        g_rx_devok = 0;
    }
    (void)buf_copyout(b, got, sizeof(got));
    if (got[0] == 'P' && got[1] == 'K') {
        if (g_rx_count < (UINT)(TSTDEV_N + 4)) {
            g_rx_seq[g_rx_count] = (int)got[2];
        }
        g_rx_count++;
    }
    buf_free(b);                        /* the handler owns the inbound PBUF */
    if (g_rx_count >= (UINT)TSTDEV_N) {
        nsfevt_stop();
    }
}

/* Build a DEVCFG for a host loopback device named `name`. */
static void make_cfg(DEVCFG *cfg, const char *name, USHORT cuu)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->name, name, strlen(name));   /* zeroed rest -> NUL-padded */
    cfg->cuu    = cuu;
    cfg->type   = NSFDEV_T_HOST;
    cfg->ipaddr = 0x0A000001u;               /* 10.0.0.1, network order      */
    cfg->mtu    = 1500;
    cfg->drvcfg = NULL;                      /* NULL -> loopback default     */
}

/* ---- scenario 1: send N -> receive N via loopback ---- */
static void scenario_flow(DEVOPS *ops)
{
    NETDEV *dev;
    DEVCFG  cfg;
    int     i, inorder;

    make_cfg(&cfg, "HOST0", 0x0E20);

    nsfevt_init();                      /* fresh loop state (clears dev hooks) */
    dev_init();                         /* fresh device table (forces re-wire) */
    dev = dev_register(&cfg, ops);
    CHECK(dev != NULL, "dev_register created the host loopback device");
    CHECK(dev_find("HOST0") == dev, "dev_find by LINK name");
    CHECK(dev_find_cuu(0x0E20) == dev, "dev_find_cuu by device address");
    CHECK(dev_by_index(0) == dev, "dev_by_index(0) resolves the device");
    CHECK_EQ((long)dev_count(), 1, "one device registered");
    CHECK_EQ((long)dev->state, (long)NSFDEV_S_DOWN, "registered device is DOWN");

    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count  = 0;
    g_rx_devok  = 1;
    g_rx_expect = dev;

    CHECK(dev_start(dev) == 0, "dev_start brought the device up");
    CHECK_EQ((long)dev->state, (long)NSFDEV_S_UP, "device state is UP");

    for (i = 0; i < TSTDEV_N; i++) {
        PBUF *b = buf_alloc(64);
        UCHAR payload[4];

        CHECK(b != NULL, "buf_alloc for an outbound frame");
        payload[0] = 'P';
        payload[1] = 'K';
        payload[2] = (UCHAR)i;          /* tag = send order */
        payload[3] = 0;
        (void)buf_copyin(b, payload, sizeof(payload));
        CHECK(dev_send(dev, b) == 0, "dev_send queued the frame");
    }

    evt_mainloop();                     /* runs until the handler stops it */

    CHECK_EQ((long)g_rx_count, TSTDEV_N,
             "every sent frame returned as EV_PACKET_RECEIVED");
    CHECK(g_rx_devok, "each EV_PACKET_RECEIVED u1 resolved to the device");
    inorder = 1;
    for (i = 0; i < TSTDEV_N; i++) {
        if (g_rx_seq[i] != i) {
            inorder = 0;
        }
    }
    CHECK(inorder, "frames were received in send order (FIFO preserved)");
    CHECK_EQ((long)dev->ctr_out->value, TSTDEV_N, "ctr_out counted N transmits");
    CHECK_EQ((long)dev->ctr_in->value, TSTDEV_N, "ctr_in counted N receives");
    CHECK_EQ((long)dev->ctr_oerr->value, 0, "no outbound errors");

    CHECK(dev_shutdown(dev) == 0, "dev_shutdown quiesced the device");
    CHECK_EQ((long)dev->state, (long)NSFDEV_S_DOWN, "device state is DOWN");
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline after the flow");
}

/* ---- scenario 2: bounded sendq (reject rather than grow) ---- */
static void scenario_bound(DEVOPS *ops)
{
    NETDEV *dev;
    DEVCFG  cfg;
    int     i, queued = 0, rejected = 0;
    const int over = 5;

    make_cfg(&cfg, "HOST1", 0x0E22);

    nsfevt_init();
    dev_init();
    dev = dev_register(&cfg, ops);
    CHECK(dev != NULL, "dev_register (bound scenario)");
    CHECK(dev_start(dev) == 0, "dev_start (bound scenario)");

    /* The loop is not running here, so nothing drains the sendq: the (bound+1)th
     * and later sends must be rejected. Each rejected send frees its PBUF. */
    for (i = 0; i < NSFDEV_SENDQ_MAX + over; i++) {
        PBUF *b = buf_alloc(64);

        CHECK(b != NULL, "buf_alloc (bound scenario)");
        if (dev_send(dev, b) == 0) {
            queued++;
        } else {
            rejected++;
        }
    }
    CHECK_EQ((long)queued, NSFDEV_SENDQ_MAX, "sendq accepted exactly its bound");
    CHECK_EQ((long)rejected, over, "sends past the bound were rejected");
    CHECK_EQ((long)dev->ctr_oerr->value, over, "ctr_oerr counted the rejects");

    /* Shutdown frees the queued (undrained) PBUFs -- the leak gate proves it. */
    CHECK(dev_shutdown(dev) == 0, "dev_shutdown drains the queued frames");
}

/* ---- scenario 3: send on a device that is not UP is rejected ---- */
static void scenario_notup(DEVOPS *ops)
{
    NETDEV *dev;
    DEVCFG  cfg;
    PBUF   *b;

    make_cfg(&cfg, "HOST2", 0x0E24);

    nsfevt_init();
    dev_init();
    dev = dev_register(&cfg, ops);       /* registered but never started (DOWN) */
    CHECK(dev != NULL, "dev_register (not-up scenario)");

    b = buf_alloc(64);
    CHECK(b != NULL, "buf_alloc (not-up scenario)");
    CHECK(dev_send(dev, b) != 0, "dev_send on a DOWN device is rejected");
    CHECK_EQ((long)dev->ctr_oerr->value, 1, "ctr_oerr counted the DOWN reject");
    /* b was freed by dev_send; the device was never started, so nothing to
     * quiesce -- but call shutdown for symmetry (idempotent on DOWN). */
    CHECK(dev_shutdown(dev) == 0, "dev_shutdown on a DOWN device is a no-op");
}

int main(void)
{
    DEVOPS *ops;

    printf("=== nsf370 NSFDEV / NSFHOST tests ===\n");

    ops = nsfhost_ops();
    if (ops == NULL) {
        /* No host driver on this platform (the MVS build). The device
         * abstraction is validated on MVS by the CTCI driver (M1-3+). */
        printf("host driver unavailable on this platform -- skipping "
               "(CTCI validates the abstraction on MVS)\n");
        return 0;
    }

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init created the EVT pool");
    CHECK(buf_init() == 0, "buf_init created the buffer pools");
    mm_init_complete();

    scenario_flow(ops);
    scenario_bound(ops);
    scenario_notup(ops);

    /* Leak gate: every buffer sent, looped back and freed; every queued frame
     * freed at shutdown. Both PBUF pools and the EVT pool back to baseline. */
#if NSF_DEBUG
    {
        MMSTATS s;

        mm_stats(buf_debug_pool(NSFBUF_CLASS_SMALL), &s);
        CHECK_EQ((long)s.inuse, 0, "BUFSMALL pool at baseline (no leak)");
        mm_stats(buf_debug_pool(NSFBUF_CLASS_LARGE), &s);
        CHECK_EQ((long)s.inuse, 0, "BUFLARGE pool at baseline (no leak)");
    }
#endif
    CHECK_EQ((long)nsfevt_inuse(), 0, "EVT pool at baseline (no leak)");

    mm_shutdown();
    return mbt_test_summary("TSTDEV");
}
