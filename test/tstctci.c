/*
 * tstctci.c -- CTCI driver bottom-half tests over the host channel shim
 * (spec 9.3; ADR-0019/0020/0021). It drives the REAL DEVOPS path (register ->
 * start -> send -> shutdown) and the DEVIO completion seam against the shim's
 * model of the 3088 (src/nsfctcio_host.c). It proves the driver's LOGIC; a wrong
 * CCW flag or IOB field is caught only by the real device (TSTCTCM on MVSCE).
 *
 * Coverage:
 *   1. inject a block -> EV_PACKET_RECEIVED with the exact IP payload;
 *   2. ping-pong: two queued blocks -> the re-drive targets the OTHER buffer and
 *      happens BEFORE the parse, so the first block is decoded uncorrupted and
 *      both are delivered in order;
 *   3. stale-ECB phantom-completion guard: after a completion with nothing more
 *      queued, recb is left CLEARED, so a further service pass delivers nothing;
 *   4. an error post code counts ctr_ierr, does NOT decode, and STILL re-drives
 *      (a READ stays outstanding);
 *   5. sendq -> WRITE: the captured block carries the CTCIHDR/CTCISEG framing
 *      with the 0x0000 terminator, and the in-flight PBUF is freed exactly once;
 *   6. leak gate: every pool (PBUF, EVT, CTCI device storage) back to baseline.
 */
#include "nsfctci.h"
#include "nsfctcif.h"
#include "nsfctcio_host.h"
#include "nsfdev.h"
#include "nsfevt.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include "nsfevtp.h"            /* NSFECB_POSTED */
#include <mbtcheck.h>
#include <string.h>

/* HOST-ONLY. This suite drives the channel MODEL (src/nsfctcio_host.c) through
 * its inject/capture/force hooks, which have no MVS counterpart -- the real
 * device is driven by traffic, not injection. On the MVS cross-build it links to
 * a trivial skip (main below); the CTCI bottom half is validated on real MVS by
 * TSTCTCM. Same pattern as TSTDEV skipping where nsfhost_ops() is NULL. */
#ifndef __MVS__

/* Two distinguishable 20-byte IPv4 packets (version nibble 4). Byte 4 is a tag
 * so a test can tell which block a delivered payload came from. */
static UCHAR ip_a(void)  { return 0xA1; }
static void make_ip(UCHAR *ip, UCHAR tag)
{
    memset(ip, 0, 20);
    ip[0] = 0x45;                       /* version 4, IHL 5 */
    ip[3] = 20;                         /* total length low byte */
    ip[4] = tag;                        /* identifier high = our tag */
    ip[9] = 1;                          /* protocol ICMP (cosmetic) */
}

/* Build the READ form of a one-segment block for `ip`/`iplen`: exactly what
 * Hercules delivers to the guest -- [CTCIHDR hwOffset=end][CTCISEG 0x0800][IP]
 * with NO 0x0000 terminator. We reuse the encoder (which appends the terminator)
 * and drop its last 2 bytes, so the leading hwOffset = end-of-data is identical
 * to the wire. Returns the delivered length. */
static UINT build_read_block(UCHAR *blk, UINT blksize, const UCHAR *ip, UINT iplen)
{
    UINT n = ctcif_encode(blk, blksize, ip, iplen);
    return (n >= 2u) ? (n - 2u) : 0u;   /* the guest never receives the term */
}

/* ---- EV_PACKET_RECEIVED capture handler ---- */
#define RX_MAX  8
static UINT   g_rx_count;
static UINT   g_rx_stop_at;
static UCHAR  g_rx_tag[RX_MAX];         /* payload byte 4 (our tag) per delivery */
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

/* Build + register + start a fresh CTCI device for a scenario. */
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

/* ---- scenario 1: inject one block -> EV_PACKET_RECEIVED ---- */
static void scenario_receive_one(void)
{
    static UCHAR blk[64];
    UCHAR   ip[20];
    UINT    blklen;
    NETDEV *dev;
    CTCIDEV *d;

    dev = fresh_dev("CTCI0", 0x0500);
    CHECK(dev != NULL, "recv1: CTCI device registered");
    CHECK(dev->io != NULL, "recv1: DEVIO seam attached (ADR-0021)");

    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 1; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "recv1: dev_start opened the channel + drove READ");
    d = (CTCIDEV *)dev->priv;
    CHECK(d != NULL, "recv1: CTCIDEV attached after start");
    CHECK(ctcio_host_outstanding(d->rscb) == 1, "recv1: a READ is outstanding after start");

    make_ip(ip, ip_a());
    blklen = build_read_block(blk, sizeof(blk), ip, 20u);
    ctcio_host_inject(d->rscb, blk, blklen);   /* a frame "arrives" */

    evt_mainloop();

    CHECK_EQ((long)g_rx_count, 1, "recv1: exactly one EV_PACKET_RECEIVED");
    CHECK(g_rx_devok, "recv1: u1 resolved to the device");
    CHECK_EQ((long)g_rx_len[0], 20, "recv1: delivered payload length is 20");
    CHECK_EQ((long)g_rx_tag[0], (long)ip_a(), "recv1: delivered the exact IP payload");
    CHECK_EQ((long)dev->ctr_in->value, 1, "recv1: ctr_in counted the receive");
    CHECK((d->recb & NSFECB_POSTED) == 0u,
          "recv1: recb left cleared after the re-drive (no stale posted bit)");

    CHECK(dev_shutdown(dev) == 0, "recv1: dev_shutdown quiesced the device");
}

/* ---- scenario 2: ping-pong re-drive targets the OTHER buffer, before parse ---- */
static void scenario_pingpong(void)
{
    static UCHAR blk1[64], blk2[64];
    UCHAR   ip1[20], ip2[20];
    UINT    l1, l2;
    NETDEV *dev;
    CTCIDEV *d;

    dev = fresh_dev("CTCI1", 0x0502);
    CHECK(dev != NULL, "pingpong: device registered");

    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 2; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "pingpong: dev_start");
    d = (CTCIDEV *)dev->priv;

    make_ip(ip1, 0x11);
    make_ip(ip2, 0x22);
    l1 = build_read_block(blk1, sizeof(blk1), ip1, 20u);
    l2 = build_read_block(blk2, sizeof(blk2), ip2, 20u);

    /* block1 completes the outstanding READ (lands in rbuf0); block2 is queued.
     * On the completion, the correct driver re-drives into rbuf1 (popping block2
     * there) BEFORE parsing rbuf0 -- so block1 in rbuf0 is NOT clobbered. A
     * buggy re-drive into rbuf0 would overwrite block1 with block2 before parse,
     * and the first delivery would carry tag 0x22 instead of 0x11. */
    ctcio_host_inject(d->rscb, blk1, l1);
    ctcio_host_inject(d->rscb, blk2, l2);

    evt_mainloop();

    CHECK_EQ((long)g_rx_count, 2, "pingpong: both blocks delivered");
    CHECK_EQ((long)g_rx_tag[0], 0x11,
             "pingpong: first delivery is block1 (re-drive hit the OTHER buffer, before parse)");
    CHECK_EQ((long)g_rx_tag[1], 0x22, "pingpong: second delivery is block2 (FIFO order)");
    CHECK_EQ((long)dev->ctr_in->value, 2, "pingpong: ctr_in counted both");

    CHECK(dev_shutdown(dev) == 0, "pingpong: dev_shutdown");
}

/* ---- scenario 3: stale-ECB phantom-completion guard ---- */
static void scenario_phantom_guard(void)
{
    static UCHAR blk[64];
    UCHAR   ip[20];
    UINT    blklen;
    NETDEV *dev;
    CTCIDEV *d;
    UINT    in_before;

    dev = fresh_dev("CTCI2", 0x0504);
    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 99; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "phantom: dev_start");
    d = (CTCIDEV *)dev->priv;

    make_ip(ip, 0x33);
    blklen = build_read_block(blk, sizeof(blk), ip, 20u);
    ctcio_host_inject(d->rscb, blk, blklen);

    /* One service pass consumes the completion (manual pump -- no loop, so no
     * phantom can be hidden by an early stop). */
    nsfdev_poll_input();
    in_before = dev->ctr_in->value;
    CHECK_EQ((long)in_before, 1, "phantom: the injected block was delivered once");
    CHECK((d->recb & NSFECB_POSTED) == 0u,
          "phantom: recb cleared by the re-drive (empty queue -> not re-posted)");

    /* A second service pass must find NOTHING to do: recb is clear, so no
     * phantom re-decode of the same buffer. Non-vacuous: if the driver had not
     * re-driven (thus not cleared recb), this pass would re-decode and bump
     * ctr_in to 2. */
    nsfdev_poll_input();
    CHECK_EQ((long)dev->ctr_in->value, (long)in_before,
             "phantom: no second delivery from a stale posted bit");

    /* Flush the delivered event (dispatch + free the PBUF) so the leak gate is
     * clean: set stop, run the loop once to drain the evq. */
    nsfevt_stop();
    evt_mainloop();
    CHECK_EQ((long)g_rx_count, 1, "phantom: exactly one event dispatched");

    CHECK(dev_shutdown(dev) == 0, "phantom: dev_shutdown");
}

/* ---- scenario 4: an error post code counts ctr_ierr, still re-drives ---- */
static void scenario_error_post(void)
{
    static UCHAR blk[64];
    UCHAR   ip[20];
    UINT    blklen;
    NETDEV *dev;
    CTCIDEV *d;

    dev = fresh_dev("CTCI3", 0x0506);
    evt_register(EV_PACKET_RECEIVED, h_rx);
    g_rx_count = 0; g_rx_stop_at = 99; g_rx_devok = 1; g_rx_dev = dev;

    CHECK(dev_start(dev) == 0, "errpost: dev_start");
    d = (CTCIDEV *)dev->priv;

    /* Force the next READ completion to carry a non-normal post code. */
    ctcio_host_force(d->rscb, 0x41, 0u);
    make_ip(ip, 0x44);
    blklen = build_read_block(blk, sizeof(blk), ip, 20u);
    ctcio_host_inject(d->rscb, blk, blklen);

    nsfdev_poll_input();

    CHECK_EQ((long)dev->ctr_in->value, 0, "errpost: a bad completion is not decoded");
    CHECK_EQ((long)dev->ctr_ierr->value, 1, "errpost: the I/O error counted ctr_ierr");
    CHECK(ctcio_host_outstanding(d->rscb) == 1,
          "errpost: a READ is STILL outstanding after the error (never left without one)");

    CHECK(dev_shutdown(dev) == 0, "errpost: dev_shutdown");
}

/* ---- scenario 5: sendq -> WRITE framing + PBUF freed once ---- */
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

    /* A 28-byte IPv4 packet to transmit. */
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
    nsfdev_kick_output();               /* io->kick encodes + issues the WRITE */
    CHECK(d->txflight != NULL, "send: the in-flight PBUF is held during the WRITE");

    nsfdev_poll_input();                /* io->service handles WRITE completion */
    CHECK_EQ((long)dev->ctr_out->value, 1, "send: ctr_out counted the transmit");
    CHECK(d->txflight == NULL, "send: the in-flight PBUF was freed on completion");

    /* The captured block carries the validated WRITE framing (ADR-0020):
     * [CTCIHDR hwOffset=end][CTCISEG len/0x0800/0][IP][CTCIHDR 0x0000]. */
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
    printf("=== nsf370 NSFCTCI bottom-half tests (over the host shim) ===\n");
#ifdef __MVS__
    /* Host-only: the channel shim + its hooks are not present on MVS. The CTCI
     * bottom half is validated on real 3.8j by TSTCTCM. */
    printf("host-only suite (drives the channel shim); MVS validates via TSTCTCM\n");
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
    scenario_pingpong();
    scenario_phantom_guard();
    scenario_error_post();
    scenario_send_write();

    /* Leak gate: every per-packet pool AND the CTCI device storage back to
     * baseline after all scenarios shut down. */
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
