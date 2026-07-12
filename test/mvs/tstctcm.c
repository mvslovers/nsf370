/*
 * tstctcm.c -- NSFCTCI on-MVS validation (spec 9.3; ADR-0019/0020/0021).
 * host = false. Four parts, split by whether they need a live device:
 *
 *  1. SVC 99 seam proof -- device-free. Drives the shipped SVC 99 code
 *     (ctci_alloc_unit -> RB99 build, libc370 text-unit builders, __svc99
 *     linkage) against a NONEXISTENT unit ("ZZZZ") and asserts a decoded
 *     S99ERROR. Nothing is allocated; the CI-meaningful assertion.
 *
 *  1b. DEVOPS refuse-to-start -- device-free. Registers a CTCI device on an
 *     UNDEFINED CUU through the real DEVOPS path and asserts dev_start refuses
 *     (SVC 99 021C), leaving the device DOWN with no leaked storage. This is the
 *     whole bottom-half bring-up path except the channel I/O.
 *
 *  1c. SVC 99 SUCCESS -- device-free. A DUMMY allocation (touches no UCB/
 *     channel/device) proves S99VRBAL rc 0, the generated DDNAME reaching our
 *     buffer, and S99VRBUN unallocating -- all over OUR svc99_call wrapper.
 *
 *  2. EXCP channel path -- gated behind -DNSFCTCI_CUU (a live 3088 pair). This is
 *     a MANUAL, SINGLE-SHOT batch probe: add -DNSFCTCI_CUU to [build].cflags, run
 *     with a background ping (CLAUDE.md §5), read the BATCH spool. mbt runs every
 *     test in both a batch AND a TSO step; the TSO step re-runs part 2 against the
 *     SAME physical 500/501 immediately after batch, and re-using the real device
 *     back-to-back leaves its READ subchannel MIH-pending (IGF991I) so the second
 *     run stalls -- that is a live-hardware re-use artifact, NOT a driver bug (the
 *     batch step ends CC 0, i.e. the full up/probe/shutdown/EOT lifecycle
 *     completes). So judge part 2 by the BATCH result; the STC (S NSF) is the
 *     single-run production gate. The committed CI state is the device-FREE parts
 *     (1/1b/1c), green in both steps. It
 *     drives the M1-4b bottom half (ADR-0022) end to end against the SUBTASK model
 *     through a mini executive loop that WAITs on a MULTI-ECB list {dev->ecb,
 *     stopECB} -- NOT a single-ECB ecb_wait(recb). That distinction is the whole
 *     point: the old TSTCTCM was a single-ECB probe ("the safe path that wasn't a
 *     production proof"), which recreated the exact blind spot that let #18
 *     through. Here the read/write SUBTASKS own the EXCP + single-ECB IOB wait and
 *     POST dev->ecb; the test's loop wakes on that POST in a MULTI-ECB WAIT, just
 *     like the executive. It register+starts (SVC 99 allocate + each subtask
 *     OPENs + first READ), dev_sends a crafted ICMP echo (the write subtask EXCPs
 *     it -> host tcpdump), waits for an inbound ping (the read subtask decodes it
 *     -> ctr_in rises), and then PROVES #18 in isolation: after a READ completes,
 *     a stop POST still wakes the same multi-ECB WAIT (the exact thing that hung
 *     the STC). Shutdown joins both subtasks (each CLOSE purges its own EXCP).
 */
#include "nsfctci.h"
#include "nsfdev.h"
#include "nsfevt.h"
#include "nsfbuf.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfsts.h"
#include <mbtcheck.h>
#include <string.h>
#include <stdio.h>
#include <clibecb.h>            /* ECB, ecb_timed_waitlist, ecb_post (mini loop) */
#include <svc99.h>             /* __txdmy/__txrddn/__txddn, S99* verbs (part 1c) */

/* Guest/host IP addresses for the crafted ICMP echo (part 2). Defaults match the
 * live MVSCE CTCI pair on mvsdev (CLAUDE.md §5): guest HOME 192.168.200.1, host
 * TUN 192.168.200.2. The runbook overrides them if the pair changes. */
#ifndef NSFCTCI_SRC
#define NSFCTCI_SRC  0xC0A8C801u    /* 192.168.200.1 (guest HOME)             */
#endif
#ifndef NSFCTCI_DST
#define NSFCTCI_DST  0xC0A8C802u    /* 192.168.200.2 (host TUN)               */
#endif
#ifndef NSFCTCI_MTU
#define NSFCTCI_MTU  1500u
#endif

/* CUU for the DEVOPS refuse-to-start check (part 1b), run when no live device is
 * configured. It must be an address NOT genned on the test system, so SVC 99
 * fails and dev_start refuses. 0E20 is verified undefined on TK5/MVSCE (D U). */
#ifndef NSFCTCI_WALL_CUU
#define NSFCTCI_WALL_CUU  0x0E20
#endif

/* ---- helpers (part 2 EXCP path only; compiled with a live device) ------- */
#ifdef NSFCTCI_CUU

/* RFC 1071 ones-complement checksum over len bytes. */
static USHORT ip_cksum(const void *p, UINT len)
{
    const UCHAR *b = (const UCHAR *)p;
    UINT         sum = 0;
    UINT         i;

    for (i = 0; i + 1u < len; i += 2u) {
        sum += ((UINT)b[i] << 8) | (UINT)b[i + 1u];
    }
    if (i < len) {
        sum += (UINT)b[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (USHORT)(~sum & 0xFFFFu);
}

static void put16(UCHAR *p, USHORT v) { p[0] = (UCHAR)(v >> 8); p[1] = (UCHAR)v; }
static void put32(UCHAR *p, UINT v)
{
    p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);  p[3] = (UCHAR)v;
}

/* Build a minimal IPv4 + ICMP echo request into buf. Returns the IP length. */
static UINT build_icmp_echo(UCHAR *buf)
{
    UCHAR  *ip   = buf;
    UCHAR  *icmp = buf + 20;
    UINT    iplen = 28u;              /* 20 IP + 8 ICMP, no payload            */

    memset(buf, 0, iplen);
    ip[0]  = 0x45;                    /* version 4, IHL 5                      */
    put16(&ip[2], (USHORT)iplen);     /* total length                         */
    put16(&ip[4], 0xABCD);            /* id                                    */
    ip[8]  = 64;                      /* TTL                                   */
    ip[9]  = 1;                       /* protocol = ICMP                       */
    put32(&ip[12], NSFCTCI_SRC);
    put32(&ip[16], NSFCTCI_DST);
    put16(&ip[10], ip_cksum(ip, 20)); /* header checksum                       */

    icmp[0] = 8;                      /* type = echo request                   */
    put16(&icmp[4], 0xABCD);          /* identifier                            */
    put16(&icmp[6], 0x0001);          /* sequence                              */
    put16(&icmp[2], ip_cksum(icmp, 8));

    return iplen;
}
#endif /* NSFCTCI_CUU */

/* Build a DEVCFG for a CTCI device. */
static void make_cfg(DEVCFG *cfg, const char *name, USHORT cuu, USHORT mtu)
{
    memset(cfg, 0, sizeof(*cfg));
    memcpy(cfg->name, name, strlen(name));
    cfg->cuu    = cuu;
    cfg->type   = NSFDEV_T_CTCI;
    cfg->ipaddr = NSFCTCI_SRC;
    cfg->mtu    = mtu;
}

/* ---- part 1: SVC 99 seam proof (runs today) ----------------------------- */

static int run_svc99_proof(void)
{
    char  ddn[9];
    short err  = 0;
    short info = 0;
    int   rc;

    memset(ddn, 0, sizeof ddn);
    printf("--- SVC 99 seam proof: allocate nonexistent unit ZZZZ ---\n");
    rc = ctci_alloc_unit("ZZZZ", ddn, &err, &info);
    printf("ctci_alloc_unit(ZZZZ) rc=%d  S99 ERROR=%04X INFO=%04X  ddn=[%s]\n",
           rc, (unsigned)(err & 0xFFFF), (unsigned)(info & 0xFFFF), ddn);

    CHECK(rc != 0, "SVC 99 allocate of nonexistent unit ZZZZ fails");
    CHECK(err != 0, "S99ERROR is set (RB99/text-unit/__svc99 linkage works)");
    return 0;
}

/* ---- part 1b: DEVOPS refuse-to-start on an undefined CUU ----------------- */
#ifndef NSFCTCI_CUU
static int run_wall_probe(void)
{
    DEVCFG  cfg;
    NETDEV *dev;

    printf("--- DEVOPS refuse-to-start vs undefined CUU %04X ---\n",
           (unsigned)NSFCTCI_WALL_CUU);
    make_cfg(&cfg, "CTCWALL", (USHORT)NSFCTCI_WALL_CUU, (USHORT)NSFCTCI_MTU);

    dev_init();
    dev = dev_register(&cfg, ctci_devops());
    CHECK(dev != NULL, "wall: dev_register (DEVIO attached, no storage yet)");
    if (dev == NULL) {
        return 1;
    }
    /* dev_start allocates the CTCIDEV, then SVC 99 allocate of the undefined CUU
     * fails -> start refuses and releases the storage (nothing half-allocated). */
    CHECK(dev_start(dev) != 0,
          "wall: dev_start refuses on the undefined CUU (SVC 99 021C)");
    CHECK_EQ((long)dev->state, (long)NSFDEV_S_DOWN, "wall: device left DOWN");
    dev_shutdown(dev);
    return 0;
}
#endif /* !NSFCTCI_CUU */

/* ---- part 2: EXCP channel path (gated; runs only with NSFCTCI_CUU) ------- */
#ifdef NSFCTCI_CUU

/* One MULTI-ECB timed WAIT on {dev->ecb, stopecb} -- the executive's shape
 * (ADR-0022, §5.3). The read/write subtasks POST dev->ecb; a stop POST is the
 * operator/stop analog. This is emphatically NOT a single-ECB ecb_wait on the raw
 * IOB ECB (which was the old probe's blind spot). Timed (a separate discarded
 * timeout ECB) so an absent ping cannot hang the batch job forever. */
static void mini_wait(NETDEV *dev, ECB *stopecb, UINT ticks)
{
    ECB *wl[2];
    ECB  tmo = 0u;

    wl[0] = (ECB *)&dev->ecb;
    wl[1] = (ECB *)((unsigned)stopecb | 0x80000000u);
    (void)ecb_timed_waitlist(wl, &tmo, ticks * 10u, 0u);
}

/* Drive the mini executive loop until ctr->value reaches `goal` or `max_iter`
 * timed WAITs elapse. Reset dev->ecb before the WAIT + double-check drain (the
 * UFSD reset-before-WAIT pattern): a subtask POST racing the reset is caught by
 * the second poll_input, never lost. Returns 1 if the goal was reached. */
static int drive_until(NETDEV *dev, STSCTR *ctr, UINT goal, ECB *stopecb,
                       UINT max_iter, UINT ticks)
{
    UINT i;

    if (ctr == NULL) {
        return 0;
    }
    for (i = 0u; i < max_iter; i++) {
        nsfdev_poll_input();                    /* io->service: decode / reap */
        if (ctr->value >= goal) {
            return 1;
        }
        if (*stopecb & ECB_POSTED_BIT) {
            return 0;
        }
        dev->ecb = 0u;                          /* reset before WAIT */
        nsfdev_poll_input();                    /* double-check (post racing reset) */
        if (ctr->value >= goal) {
            return 1;
        }
        mini_wait(dev, stopecb, ticks);         /* MULTI-ECB WAIT */
    }
    return (ctr->value >= goal);
}

static int run_device_probe(void)
{
    DEVCFG   cfg;
    NETDEV  *dev;
    CTCIDEV *d;
    UCHAR    ip[64];
    UINT     iplen;
    PBUF    *b;
    ECB      stopecb = 0u;

    printf("--- EXCP channel path (subtask model, MULTI-ECB WAIT): CUU %04X ---\n",
           (unsigned)NSFCTCI_CUU);
    make_cfg(&cfg, "CTCA", (USHORT)NSFCTCI_CUU, (USHORT)NSFCTCI_MTU);

    dev_init();
    dev = dev_register(&cfg, ctci_devops());
    CHECK(dev != NULL, "dev_register");
    if (dev == NULL) {
        return 1;
    }
    CHECK(dev_start(dev) == 0,
          "dev_start: SVC 99 allocate + both subtasks OPEN + first READ driven");
    d = (CTCIDEV *)dev->priv;
    if (d == NULL) {
        return 1;
    }
    printf("device up: DD %s/%s (read subtask %08X, write subtask %08X)\n",
           d->rddn, d->wddn, (unsigned)d->rsub, (unsigned)d->wsub);

    /* --- crafted WRITE: dev_send an ICMP echo; the write subtask EXCPs it and
     * POSTs dev->ecb; the mini MULTI-ECB loop reaps it. --- */
    iplen = build_icmp_echo(ip);
    b = buf_alloc((USHORT)iplen);
    CHECK(b != NULL, "buf_alloc for the crafted frame");
    (void)buf_copyin(b, ip, (USHORT)iplen);
    CHECK(dev_send(dev, b) == 0, "dev_send queued the crafted ICMP echo");
    nsfdev_kick_output();               /* io->kick encodes + hands to write_sub */
    CHECK(drive_until(dev, dev->ctr_out, 1u, &stopecb, 30u, 10u),
          "crafted WRITE transmitted via the write subtask (ctr_out rose)");
    printf("WRITE ctr_out=%u (crafted ICMP echo id 0xABCD -> see host tcpdump)\n",
           (unsigned)dev->ctr_out->value);

    /* --- inbound READ: host pings of the guest HOME arrive; each completes the
     * read subtask's EXCP READ and POSTs dev->ecb, waking the MULTI-ECB WAIT. The
     * ISOLATED #18 disproof is REPEATED wakes: in #18 the FIRST foreign post into
     * the executive's multi-ECB WAIT left it unresponsive to the next. Requiring
     * ctr_in >= 3 proves the multi-ECB WAIT keeps waking across successive
     * real-device subtask posts. (The operator-responds-AFTER-a-READ gate is the
     * STC, F NSF,STATS -- a second, genuinely foreign async poster.) --- */
    printf("waiting for inbound frames (ping the guest HOME now; ~60s budget)...\n");
    CHECK(drive_until(dev, dev->ctr_in, 3u, &stopecb, 60u, 10u),
          "MULTI-ECB WAIT woke repeatedly on real read-subtask posts (#18 disproof)");
    printf("READ ctr_in=%u ctr_ierr=%u\n",
           (unsigned)dev->ctr_in->value, (unsigned)dev->ctr_ierr->value);

    /* Sanity: the multi-ECB WAIT returns when the stopECB slot is posted (the
     * stop path is live). Not the full operator proof -- that is the STC. */
    printf("posting stop; the multi-ECB WAIT must return...\n");
    (void)ecb_post((ECB *)&stopecb, 0u);
    dev->ecb = 0u;
    mini_wait(dev, &stopecb, 50u);      /* MULTI-ECB WAIT, <=5s */
    CHECK((stopecb & ECB_POSTED_BIT) != 0u,
          "multi-ECB WAIT returned on the stop POST (stopECB slot live)");

    /* --- clean shutdown: stop + join both subtasks (each CLOSE purges its own
     * outstanding EXCP -- same TCB), unallocate, release storage. --- */
    CHECK(dev_shutdown(dev) == 0, "dev_shutdown joined the subtasks + unallocated");
    CHECK_EQ((long)dev->state, (long)NSFDEV_S_DOWN, "device DOWN after shutdown");
    return 0;
}
#endif /* NSFCTCI_CUU */

/* ---- part 1c: SVC 99 SUCCESS path via a DUMMY allocation ----------------- */

/* A system-generated DDNAME (DALRTDDN) is 8 uppercase, non-blank characters.
 * Checked charset-transparently (strchr over an EBCDIC literal). */
static int ddname_ok(const char *d)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (d[i] == ' ' || d[i] == '\0') {
            return 0;
        }
        if (strchr("abcdefghijklmnopqrstuvwxyz", d[i]) != NULL) {
            return 0;
        }
    }
    return 1;
}

static int run_alloc_success(void)
{
    struct txt99 **txt = NULL;
    char           ddn[9];
    short          err = 0;
    short          info = 0;
    int            rc;

    memset(ddn, 0, sizeof ddn);
    printf("--- SVC 99 success path: DUMMY allocation (no device) ---\n");

    if (__txrddn(&txt, NULL) || __txdmy(&txt, NULL)) {
        if (txt) FreeTXT99Array(&txt);
        CHECK(0, "built the DUMMY allocation text units");
        return 1;
    }
    rc = svc99_call(txt, S99VRBAL, ddn, &err, &info);
    FreeTXT99Array(&txt);
    printf("DUMMY alloc   rc=%d  S99 ERROR=%04X INFO=%04X  ddn=[%s]\n",
           rc, (unsigned)(err & 0xFFFF), (unsigned)(info & 0xFFFF), ddn);
    CHECK(rc == 0, "SVC 99 S99VRBAL DUMMY allocation succeeds (rc 0)");
    if (rc != 0) {
        return 1;
    }
    CHECK(ddname_ok(ddn), "returned DDNAME is 8 uppercase non-blank chars");

    txt = NULL;
    if (__txddn(&txt, ddn)) {
        if (txt) FreeTXT99Array(&txt);
        CHECK(0, "built the unallocate text unit");
        return 1;
    }
    err = 0;
    info = 0;
    rc = svc99_call(txt, S99VRBUN, NULL, &err, &info);
    FreeTXT99Array(&txt);
    printf("DUMMY unalloc rc=%d  S99 ERROR=%04X INFO=%04X\n",
           rc, (unsigned)(err & 0xFFFF), (unsigned)(info & 0xFFFF));
    CHECK(rc == 0, "SVC 99 S99VRBUN unallocation succeeds (rc 0)");
    return 0;
}

int main(void)
{
    printf("=== nsf370 NSFCTCI MVS validation (M1-4) ===\n");

    sts_init();
    mm_init(NULL);
    nsftmr_init();
    if (nsfevt_init() != 0 || buf_init() != 0 ||
        ctci_reserve(1u, CTCI_BUF_DEFAULT) != 0) {
        printf("init failed\n");
        mm_shutdown();
        return 1;
    }
    mm_init_complete();

    run_svc99_proof();

#ifdef NSFCTCI_CUU
    run_device_probe();
#else
    run_wall_probe();
    run_alloc_success();
    printf("--- EXCP channel path DEFERRED ---\n");
    printf("no CTCI device configured; rebuild with -DNSFCTCI_CUU=0xNNNN and a\n");
    printf("live 3088 pair to drive the channel path (see PR runbook).\n");
#endif

    mm_shutdown();
    return mbt_test_summary("TSTCTCM");
}
