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
 *   5. leak gate: every pool (PBUF, EVT, CTCI device storage) back to baseline
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
