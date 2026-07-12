/*
 * tstctci.c -- CTCI driver bottom-half tests over the host channel + thread
 * shims (spec 9.3; ADR-0019/0020/0022). It drives the REAL DEVOPS path
 * (register -> start -> send -> shutdown) with the two I/O subtasks running as
 * genuine pthreads (src/nsfthr_host.c) against the shim's model of the 3088
 * (src/nsfctcio_host.c). So the SAME subtask logic (read_sub / write_sub) and the
 * single-block-synchronous handoff run here and on MVS -- exactly as M1-2's
 * NSFHOST reader thread validated the doneq handoff across a real thread boundary.
 * It proves the driver's LOGIC; a wrong CCW flag or IOB field is caught only by
 * the real device (TSTCTCM on MVSCE).
 *
 * The flow under test (ADR-0022): a frame injected on the read subchannel
 * completes the read subtask's outstanding EXCP READ (shim posts recb); the
 * subtask stores len/post, POSTs dev->ecb, and waits returnecb; the executive
 * (evt_mainloop) wakes on dev->ecb, DECODES the buffer into PBUFs on its own task
 * (EV_PACKET_RECEIVED), and POSTs returnecb so the subtask reads again. On the
 * send side the executive encodes into wbuf and POSTs txgoecb; the write subtask
 * EXCPs the WRITE (shim captures) and POSTs dev->ecb; the executive reaps and
 * frees the PBUF once.
 *
 * Coverage:
 *   1. one frame -> exactly one EV_PACKET_RECEIVED with the exact IP payload;
 *   2. two frames -> both delivered, in order (the returnecb handshake cycles);
 *   3. an error post code counts ctr_ierr, does NOT decode, and the subtask keeps
 *      reading (a following good frame is still delivered);
 *   4. sendq -> WRITE: the captured block carries the CTCIHDR/CTCISEG framing with
 *      the 0x0000 terminator, ctr_out counts, and the PBUF is freed exactly once;
 *   5. a completion whose dev->ecb wake was destroyed (the executive's
 *      reset-before-service racing the subtask's POST, issue #21) is still
 *      reaped on the loop's next pass via the work_pending recheck -- with NO
 *      timer ticking on the host, so the reap cannot be a heartbeat rescue;
 *   6. the loop consults the DEVIO pending probe before committing to WAIT
 *      (a fake device with pending work but a cleared ECB must be serviced,
 *      not parked); plus the default-model probe (doneq non-empty);
 *   7. leak gate: every pool (PBUF, EVT, CTCI device storage) back to baseline
 *      after the subtasks are joined at shutdown.
 */
#include "nsfctci.h"
#include "nsfctcio_host.h"
#include "nsfdev.h"
#include "nsfevt.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include "nsfthr.h"             /* nsfthr_timed_wait (drive the send path)     */
#include "nsfevtp.h"            /* NSFECB_POSTED */
#include <mbtcheck.h>
#include <string.h>
#include <stdio.h>

/* HOST-ONLY. This suite drives the channel + thread MODELS, which have no MVS
 * counterpart. On the MVS cross-build it links to a trivial skip; the CTCI bottom
 * half is validated on real MVS by TSTCTCM. */
#ifndef __MVS__

/* A 20-byte IPv4 packet (version nibble 4); byte 4 is a tag so a test can tell
 * which frame a delivery came from. */
static void make_ip(UCHAR *ip, UCHAR tag)
{
    memset(ip, 0, 20);
    ip[0] = 0x45;                       /* version 4, IHL 5 */
    ip[3] = 20;                         /* total length low byte */
    ip[4] = tag;                        /* identifier high = our tag */
    ip[9] = 1;                          /* protocol ICMP (cosmetic) */
}

/* Build the READ form of a one-segment block for `ip`: exactly what Hercules
 * delivers -- [CTCIHDR hwOffset=end][CTCISEG 0x0800][IP] with NO 0x0000
 * terminator. Reuse the encoder (which appends the terminator) and drop its last
 * 2 bytes. Returns the delivered length. */
static UINT build_read_block(UCHAR *blk, UINT blksize, const UCHAR *ip, UINT iplen)
{
    UINT n = ctcif_encode(blk, blksize, ip, iplen);
    return (n >= 2u) ? (n - 2u) : 0u;   /* the guest never receives the term */
}

/* ---- EV_PACKET_RECEIVED capture handler ---- */
#define RX_MAX  8
static UINT   g_rx_count;
static UINT   g_rx_stop_at;
static UCHAR  g_rx_tag[RX_MAX];
static USHORT g_rx_len[RX_MAX];
static int    g_rx_devok;
static NETDEV *g_rx_dev;

static void h_rx(EVT *ev)
{
    PBUF   *b = (PBUF *)ev->p1;
    NETDEV *d = dev_by_index(ev->u1);
    UCHAR   buf[64];
    USHORT  n;

    if (d != g_rx_dev) {
        g_rx_devok = 0;
    }
    n = buf_copyout(b, buf, sizeof(buf));
    if (g_rx_count < RX_MAX) {
        g_rx_tag[g_rx_count] = (n >= 5u) ? buf[4] : 0u;
        g_rx_len[g_rx_count] = n;
    }
    g_rx_count++;
    buf_free(b);                        /* the handler owns the inbound PBUF */
    if (g_rx_count >= g_rx_stop_at) {
        nsfevt_stop();
    }
}

/* Build + register a fresh CTCI device for a scenario (fresh loop state too). */
static NETDEV *fresh_dev(const char *name, USHORT cuu)
{
    DEVCFG  cfg;
    NETDEV *dev;

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, name, strlen(name));
    cfg.cuu    = cuu;
    cfg.type   = NSFDEV_T_CTCI;
    cfg.ipaddr = 0x0A000002u;
    cfg.mtu    = 1500;

    nsfevt_init();
    dev_init();
    dev = dev_register(&cfg, ctci_devops());
    return dev;
}

/* ---- scenario 1: one frame -> one EV_PACKET_RECEIVED ---- */
static void scenario_receive_one(void)
{
    static UCHAR blk[64];
    UCHAR   ip[20];
    UINT    blklen;
    NETDEV *dev;
    CTCIDEV *d;

    dev = fresh_dev("CTCI0", 0x0500);
    CHECK(dev != NULL, "recv1: CTCI device registered");
    CHECK(dev->io != NULL, "recv1: DEVIO seam attached");

    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 1; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "recv1: dev_start created + OPENed the subtasks");
    d = (CTCIDEV *)dev->priv;
    CHECK(d != NULL, "recv1: CTCIDEV attached after start");

    make_ip(ip, 0xA1);
    blklen = build_read_block(blk, sizeof(blk), ip, 20u);
    ctcio_host_inject(d->rscb, blk, blklen);   /* completes the subtask's READ */

    evt_mainloop();                            /* executive decodes -> EV_PACKET */

    CHECK_EQ((long)g_rx_count, 1, "recv1: exactly one EV_PACKET_RECEIVED");
    CHECK(g_rx_devok, "recv1: u1 resolved to the device");
    CHECK_EQ((long)g_rx_len[0], 20, "recv1: delivered payload length is 20");
    CHECK_EQ((long)g_rx_tag[0], 0xA1, "recv1: delivered the exact IP payload");
    CHECK_EQ((long)dev->ctr_in->value, 1, "recv1: ctr_in counted the receive");

    CHECK(dev_shutdown(dev) == 0, "recv1: dev_shutdown joined the subtasks");
}

/* ---- scenario 2: two frames -> both delivered, in order ---- */
static void scenario_receive_two(void)
{
    static UCHAR blk1[64], blk2[64];
    UCHAR   ip1[20], ip2[20];
    UINT    l1, l2;
    NETDEV *dev;
    CTCIDEV *d;

    dev = fresh_dev("CTCI1", 0x0502);
    CHECK(dev != NULL, "recv2: device registered");

    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 2; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "recv2: dev_start");
    d = (CTCIDEV *)dev->priv;

    make_ip(ip1, 0x11);
    make_ip(ip2, 0x22);
    l1 = build_read_block(blk1, sizeof(blk1), ip1, 20u);
    l2 = build_read_block(blk2, sizeof(blk2), ip2, 20u);

    /* frame1 completes the outstanding READ; frame2 queues in the shim and is
     * picked up when the subtask reads again after the returnecb handshake. The
     * single read buffer is safe: the subtask does not re-read until the
     * executive has finished decoding (posted returnecb). */
    ctcio_host_inject(d->rscb, blk1, l1);
    ctcio_host_inject(d->rscb, blk2, l2);

    evt_mainloop();

    CHECK_EQ((long)g_rx_count, 2, "recv2: both frames delivered");
    CHECK_EQ((long)g_rx_tag[0], 0x11, "recv2: first delivery is frame1 (FIFO)");
    CHECK_EQ((long)g_rx_tag[1], 0x22, "recv2: second delivery is frame2 (FIFO)");
    CHECK_EQ((long)dev->ctr_in->value, 2, "recv2: ctr_in counted both");

    CHECK(dev_shutdown(dev) == 0, "recv2: dev_shutdown");
}

/* ---- scenario 3: an error post code -> ctr_ierr, no decode, keeps reading ---- */
static void scenario_error_then_good(void)
{
    static UCHAR blk[64], blk2[64];
    UCHAR   ip[20], ip2[20];
    UINT    blklen, l2;
    NETDEV *dev;
    CTCIDEV *d;

    dev = fresh_dev("CTCI3", 0x0506);
    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 1; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "errpost: dev_start");
    d = (CTCIDEV *)dev->priv;

    /* Force the FIRST READ completion to carry a non-normal post code; a good
     * frame follows. The bad one must be counted+dropped and the subtask must
     * keep reading, so the good one is still delivered. */
    ctcio_host_force(d->rscb, 0x41, 0u);
    make_ip(ip, 0x44);
    blklen = build_read_block(blk, sizeof(blk), ip, 20u);
    ctcio_host_inject(d->rscb, blk, blklen);

    make_ip(ip2, 0x55);
    l2 = build_read_block(blk2, sizeof(blk2), ip2, 20u);
    ctcio_host_inject(d->rscb, blk2, l2);

    evt_mainloop();

    CHECK_EQ((long)dev->ctr_ierr->value, 1, "errpost: the bad completion counted ctr_ierr");
    CHECK_EQ((long)dev->ctr_in->value, 1, "errpost: only the good frame decoded");
    CHECK_EQ((long)g_rx_count, 1, "errpost: exactly one delivery (the good frame)");
    CHECK_EQ((long)g_rx_tag[0], 0x55, "errpost: the delivered frame is the good one");

    CHECK(dev_shutdown(dev) == 0, "errpost: dev_shutdown");
}

/* ---- scenario 4: sendq -> WRITE framing + PBUF freed once ---- */
static void scenario_send_write(void)
{
    UCHAR        ip[28];
    NETDEV      *dev;
    CTCIDEV     *d;
    PBUF        *b;
    const UCHAR *cap;
    UINT         caplen = 0;
    UINT         i;

    dev = fresh_dev("CTCI4", 0x0508);
    CHECK(dev_start(dev) == 0, "send: dev_start");
    d = (CTCIDEV *)dev->priv;

    memset(ip, 0, sizeof(ip));
    ip[0] = 0x45;
    ip[3] = 28;
    for (i = 8u; i < 28u; i++) {
        ip[i] = (UCHAR)i;               /* recognizable payload */
    }
    b = buf_alloc((USHORT)sizeof(ip));
    CHECK(b != NULL, "send: buf_alloc for the outbound frame");
    (void)buf_copyin(b, ip, (USHORT)sizeof(ip));

    CHECK(dev_send(dev, b) == 0, "send: dev_send queued the frame");
    nsfdev_kick_output();               /* io->kick encodes + hands to write_sub */

    /* The write subtask EXCPs the WRITE (shim captures) and POSTs dev->ecb; wait
     * for it, then reap via service. Bounded so a stuck write cannot hang. */
    for (i = 0u; i < 200u && dev->ctr_out->value == 0; i++) {
        nsfthr_timed_wait(&dev->ecb, 1u);   /* <= 0.1 s per spin */
        dev->ecb = 0u;
        nsfdev_poll_input();                /* io->service reaps the WRITE */
    }

    CHECK_EQ((long)dev->ctr_out->value, 1, "send: ctr_out counted the transmit");
    CHECK(d->txpbuf == NULL, "send: the in-flight PBUF was freed on completion");

    cap = ctcio_host_captured(d->wscb, &caplen);
    CHECK_EQ((long)caplen, (long)(8u + 28u + 2u), "send: captured block length");
    CHECK((UINT)((cap[0] << 8) | cap[1]) == 8u + 28u,
          "send: leading hwOffset = end-of-data");
    CHECK((UINT)((cap[2] << 8) | cap[3]) == 6u + 28u, "send: CTCISEG hwLength");
    CHECK((UINT)((cap[4] << 8) | cap[5]) == CTCI_TYPE_IPV4, "send: CTCISEG hwType 0x0800");
    CHECK(cap[8] == 0x45, "send: IP payload present at offset 8");
    CHECK(cap[8 + 28] == 0x00 && cap[8 + 28 + 1] == 0x00,
          "send: trailing CTCIHDR terminator 0x0000");

    CHECK(dev_shutdown(dev) == 0, "send: dev_shutdown");
}

/* ---- scenario 5: sustained back-to-back WRITEs must drain ----
 * Regression for the write handshake: the write subtask owns wecb and hands the
 * completion up via wpost/wready; the executive reap keys off wready (never wecb)
 * and re-arms for the next WRITE. If the reap ever reads/clears wecb again, it
 * steals the subtask's completion and the pipeline stalls after ONE WRITE (the
 * live 999/1000-loss bug). Driving N > sendq depth writes end-to-end and asserting
 * every one transmits (ctr_out == N, txbusy always clears) guards that handshake.
 * The host shim completes synchronously so it cannot reproduce the MVS scheduler
 * race itself -- the live TSTCTCM / ping gate is that proof -- but it locks the
 * ping-pong so a future "stuck after one" fails here. */
static void scenario_send_many(void)
{
    UCHAR    ip[28];
    NETDEV  *dev;
    CTCIDEV *d;
    UINT     n, i;
    const UINT N = 40u;                 /* > NSFDEV_SENDQ_MAX so the queue cycles */

    dev = fresh_dev("CTCI5", 0x050A);
    CHECK(dev_start(dev) == 0, "many: dev_start");
    d = (CTCIDEV *)dev->priv;

    memset(ip, 0, sizeof(ip));
    ip[0] = 0x45;
    ip[3] = 28;

    for (n = 0u; n < N; n++) {
        PBUF *b = buf_alloc((USHORT)sizeof(ip));

        CHECK(b != NULL, "many: buf_alloc");
        ip[9] = (UCHAR)n;               /* vary a byte per frame */
        (void)buf_copyin(b, ip, (USHORT)sizeof(ip));
        CHECK(dev_send(dev, b) == 0, "many: dev_send queued");
        nsfdev_kick_output();           /* start this WRITE */

        /* Complete THIS write before the next, so a "stuck after one" regression
         * leaves txbusy set and fails the CHECK below rather than hanging. */
        for (i = 0u; i < 200u && d->txbusy; i++) {
            nsfthr_timed_wait(&dev->ecb, 1u);
            dev->ecb = 0u;
            nsfdev_poll_input();        /* reap via wready */
            nsfdev_kick_output();
        }
        CHECK(d->txbusy == 0u, "many: WRITE completed (not stuck)");
    }

    CHECK_EQ((long)dev->ctr_out->value, (long)N, "many: every WRITE transmitted");
    CHECK(d->txpbuf == NULL, "many: no in-flight PBUF left");
    CHECK(dev_shutdown(dev) == 0, "many: dev_shutdown");
}

/* ---- scenario 6: destroyed dev->ecb wake -> reaped via work_pending ----
 * The issue #21 executive-side loss, made deterministic: a WRITE completes
 * (wready set, dev->ecb posted), then the wake is destroyed (the executive's
 * reset-before-service racing the POST -- simulated by clearing dev->ecb before
 * the loop sees it). The loop's WAIT-commit recheck (nsfdev_work_pending,
 * ADR-0025) must reap it on the SAME pass. The host build never ticks timers
 * inside evt_mainloop (nsfstim is a no-op recorder), so if this reap happens at
 * all it happened through the probe -- a heartbeat rescue is impossible here,
 * which is exactly what makes the scenario conclusive. A checker thread bounds
 * the failure mode (a probe-less loop parks in WAIT forever). */
static NSFECB g_ck_nap;                  /* checker naps on this (never posted) */
static int    g_ck_reaped;               /* checker saw ctr_out advance         */
static STSCTR *g_ck_ctr;                 /* the ctr_out the checker watches     */

static int checker_fn(void *arg)
{
    UINT i;

    (void)arg;
    for (i = 0u; i < 30u; i++) {
        if (g_ck_ctr->value != 0u) {
            g_ck_reaped = 1;
            break;
        }
        nsfthr_timed_wait(&g_ck_nap, 1u);        /* ~100 ms nap per probe */
    }
    nsfevt_stop();                               /* end the loop either way */
    return 0;
}

static void scenario_lost_wake_reap(void)
{
    UCHAR    ip[28];
    NETDEV  *dev;
    CTCIDEV *d;
    PBUF    *b;
    NSFTHR  *ck;
    UINT     i;

    dev = fresh_dev("CTCI6", 0x050C);
    CHECK(dev_start(dev) == 0, "lostwake: dev_start");
    d = (CTCIDEV *)dev->priv;

    memset(ip, 0, sizeof(ip));
    ip[0] = 0x45;
    ip[3] = 28;
    b = buf_alloc((USHORT)sizeof(ip));
    CHECK(b != NULL, "lostwake: buf_alloc");
    (void)buf_copyin(b, ip, (USHORT)sizeof(ip));
    CHECK(dev_send(dev, b) == 0, "lostwake: dev_send queued");
    nsfdev_kick_output();                        /* start the WRITE */

    /* Wait for the subtask's completion handover (wready set, dev->ecb posted)
     * WITHOUT servicing it. */
    for (i = 0u; i < 200u && d->wready == 0u; i++) {
        nsfthr_timed_wait(&dev->ecb, 1u);
    }
    CHECK(d->wready != 0u, "lostwake: WRITE completed and handed up");

    dev->ecb = 0u;                               /* destroy the wake (#21 race) */
    CHECK(nsfdev_work_pending() != 0, "lostwake: probe sees the unreaped WRITE");

    g_ck_nap = 0u; g_ck_reaped = 0; g_ck_ctr = dev->ctr_out;
    ck = nsfthr_create(checker_fn, NULL);
    CHECK(ck != NULL, "lostwake: checker thread created");

    evt_mainloop();                              /* must reap WITHOUT any wake */

    CHECK(g_ck_reaped, "lostwake: reaped with dev->ecb destroyed (probe path)");
    CHECK_EQ((long)dev->ctr_out->value, 1, "lostwake: ctr_out counted the WRITE");
    CHECK(d->txpbuf == NULL && d->txbusy == 0u, "lostwake: PBUF freed, tx idle");
    CHECK(nsfdev_work_pending() == 0, "lostwake: probe idle after the reap");

    nsfthr_post(&g_ck_nap, 1u);                  /* release a napping checker */
    CHECK_EQ((long)nsfthr_join(ck, 20u), 0, "lostwake: checker joined");
    CHECK(dev_shutdown(dev) == 0, "lostwake: dev_shutdown");
}

/* ---- scenario 7: the loop consults the pending probe before WAIT ----
 * A minimal fake DEVIO device: pending work flagged, ECB deliberately clear.
 * With the ADR-0025 recheck the loop's first pass skips the WAIT and services
 * it; without it the loop parks forever (no ECB, no host timer) -- the watchdog
 * turns that hang into a bounded CHECK failure. Also unit-checks the
 * default-model probe (io == NULL: doneq non-empty). */
static int    g_fk_pending;
static int    g_fk_serviced;
static int    g_fk_late;
static NSFECB g_wd_nap;

static int  fk_collect(NETDEV *dev, NSFECB **list, int max)
{
    if (max > 0) {
        list[0] = (NSFECB *)&dev->ecb;
        return 1;
    }
    return 0;
}
static void fk_service(NETDEV *dev)
{
    (void)dev;
    if (g_fk_pending) {
        g_fk_pending  = 0;                       /* consume, exactly once */
        g_fk_serviced = 1;
        nsfevt_stop();
    }
}
static void fk_kick(NETDEV *dev)    { (void)dev; }
static int  fk_pending(NETDEV *dev) { (void)dev; return g_fk_pending; }
static DEVIO g_fk_io = { fk_collect, fk_service, fk_kick, fk_pending };

static int fk_init(NETDEV *dev, const DEVCFG *cfg)
{
    (void)cfg;
    dev_set_io(dev, &g_fk_io);
    return 0;
}
static DEVOPS g_fk_ops   = { fk_init, NULL, NULL, NULL };
static DEVOPS g_null_ops = { NULL, NULL, NULL, NULL };

static int watchdog_fn(void *arg)
{
    (void)arg;
    nsfthr_timed_wait(&g_wd_nap, 15u);           /* ~1.5 s bound */
    if (!g_fk_serviced) {
        g_fk_late = 1;
        nsfevt_stop();                           /* unpark a probe-less loop */
    }
    return 0;
}

static void scenario_loop_pending_probe(void)
{
    DEVCFG  cfg;
    NETDEV *dev;
    NSFTHR *wd;
    QELEM   qe;

    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.name, "FAKE0", 5);
    cfg.type = NSFDEV_T_HOST;

    nsfevt_init();
    dev_init();
    dev = dev_register(&cfg, &g_fk_ops);
    CHECK(dev != NULL, "probe: fake DEVIO device registered");

    g_fk_pending = 1; g_fk_serviced = 0; g_fk_late = 0; g_wd_nap = 0u;
    CHECK(nsfdev_work_pending() != 0, "probe: DEVIO pending reported");

    wd = nsfthr_create(watchdog_fn, NULL);
    CHECK(wd != NULL, "probe: watchdog created");

    evt_mainloop();                              /* dev->ecb is 0 the whole time */

    CHECK(g_fk_serviced, "probe: loop serviced flagged work without any wake");
    CHECK(!g_fk_late, "probe: serviced on its own pass, not by the watchdog");

    nsfthr_post(&g_wd_nap, 1u);                  /* release a napping watchdog */
    CHECK_EQ((long)nsfthr_join(wd, 20u), 0, "probe: watchdog joined");

    /* Default model (io == NULL): pending == doneq non-empty. */
    memcpy(cfg.name, "NULL0", 5);
    nsfevt_init();
    dev_init();
    dev = dev_register(&cfg, &g_null_ops);
    CHECK(dev != NULL, "probe: default-model device registered");
    CHECK(nsfdev_work_pending() == 0, "probe: default model idle when empty");
    xq_push(&dev->doneq, &qe);
    CHECK(nsfdev_work_pending() != 0, "probe: default model sees doneq work");
    (void)xq_drain(&dev->doneq);                 /* take the stack QELEM back */
    CHECK(nsfdev_work_pending() == 0, "probe: idle again after the drain");
}
#endif /* !__MVS__ */

int main(void)
{
    printf("=== nsf370 NSFCTCI bottom-half tests (subtask model, host shims) ===\n");
#ifdef __MVS__
    printf("host-only suite (drives the channel + thread shims); MVS validates via TSTCTCM\n");
    return 0;
#else

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    CHECK(nsfevt_init() == 0, "nsfevt_init created the EVT pool");
    CHECK(buf_init() == 0, "buf_init created the buffer pools");
    CHECK(ctci_reserve(1u, CTCI_BUF_DEFAULT) == 0, "ctci_reserve created the CTCI pools");
    mm_init_complete();

    scenario_receive_one();
    scenario_receive_two();
    scenario_error_then_good();
    scenario_send_write();
    scenario_send_many();
    scenario_lost_wake_reap();
    scenario_loop_pending_probe();

    /* Leak gate: every per-packet pool AND the CTCI device storage back to
     * baseline after all scenarios have joined their subtasks and shut down. */
#if NSF_DEBUG
    {
        MMSTATS s;

        mm_stats(buf_debug_pool(NSFBUF_CLASS_SMALL), &s);
        CHECK_EQ((long)s.inuse, 0, "leak gate: BUFSMALL pool at baseline");
        mm_stats(buf_debug_pool(NSFBUF_CLASS_LARGE), &s);
        CHECK_EQ((long)s.inuse, 0, "leak gate: BUFLARGE pool at baseline");
        mm_stats(ctci_debug_pool(0), &s);
        CHECK_EQ((long)s.inuse, 0, "leak gate: CTCIDEV pool at baseline");
        mm_stats(ctci_debug_pool(1), &s);
        CHECK_EQ((long)s.inuse, 0, "leak gate: CTCI subchannel pool at baseline");
        mm_stats(ctci_debug_pool(2), &s);
        CHECK_EQ((long)s.inuse, 0, "leak gate: CTCI I/O buffer pool at baseline");
    }
#endif
    CHECK_EQ((long)nsfevt_inuse(), 0, "leak gate: EVT pool at baseline");

    mm_shutdown();
    return mbt_test_summary("TSTCTCI");
#endif /* __MVS__ */
}
