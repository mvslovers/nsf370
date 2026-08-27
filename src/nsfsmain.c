/*
 * nsfsmain.c -- the NSFS (Phase-2) STC main.
 *
 * GENERATED FROM src/nsfmain.c AND KEPT DELIBERATELY CLOSE TO IT.  Everything
 * below -- config load, pool sizing, device wiring, the IP/socket/transport
 * init order, ESTAE, the heartbeat, teardown -- is the Phase-1 sequence
 * verbatim.  Exactly three things differ, and they are the whole of Phase 2:
 *
 *   1. nsfsx_start() installs the cross-AS transport (CSA anchor + private
 *      SVC) before the loop, nsfsx_stop() takes it down after.
 *   2. evt_set_request() is wired to the CROSS-AS transport instead of the
 *      same-address-space one.  Same executive seam, different transport --
 *      spec 2's "the NSFRQE request block is the phase boundary; only the
 *      transport changes", in one call.
 *   3. NSFS runs AUTHORISED (ac=1): __super, CSA getmain and the SVCTABLE
 *      store require it.  NSF stays unauthorised, which is why this is a
 *      separate module rather than a mode of that one.
 *
 * The duplication is conscious and temporary: refactoring nsfmain.c in the
 * same step that adds Phase 2 would put the proven Phase-1 STC at risk for a
 * cosmetic gain, and re-validating `S NSF` live costs a cycle.  The two mains
 * converge when Phase 2 supersedes Phase 1.
 *
 * Original header follows.
 *
 * nsfmain.c -- the NSF started task: init -> loop -> MODIFY/STOP -> shutdown,
 * under ESTAE, config-driven (spec 5, 14, 17; the M0-8 deliverable). MVS-ONLY
 * (project.toml host = false): it uses the MVS console/ESTAE seams, so it is
 * never compiled on the host -- the sequencing it drives (nsf_init_from_cfg),
 * the dispatcher (nsfopr_dispatch) and the loop (evt_mainloop) are all
 * host-tested on their own.
 *
 * Startup order (spec 14.4):
 *   trace/stats init -> cfg_load PROFILE -> mm_init/nsftmr_init/nsfevt_init
 *   -> nsf_init_from_cfg (pools sized from NSFPOOL, trace flags, refs)
 *   -> mm_init_complete (seal) -> operator init -> ESTAE -> evt_mainloop.
 * A config error or a failed pool build REFUSES to start (no partial config).
 */
#include "nsf.h"
#include "nsfcfg.h"
#include "nsfstc.h"
#include "nsfopr.h"
#include "nsfswap.h"    /* nsfswap_op -- the SWAP verb (Phase 2 only) */
#include "nsfevt.h"
#include "nsfmsg.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsfstim.h"           /* nsftmr_plat_arm (the liveness heartbeat)    */
#include "nsftrc.h"
#include "nsfsts.h"
#include "nsfdev.h"            /* dev_register / dev_start / dev_shutdown     */
#include "nsfctci.h"          /* ctci_reserve / ctci_devops                  */
#include "nsfbuf.h"           /* PBUF, buf_free (the RX terminus)            */
#include "nsfip.h"            /* nsfip_input / nsfip_config (the IP layer)   */
#include "nsficmp.h"          /* nsficmp_init (ICMP counters)                */
#include "nsfsoc.h"           /* soc_reserve / soc_init (the socket layer)   */
#include "nsfreq.h"           /* nsfreq_init / _register_proto / _ecb / ...  */
#include "nsfsel.h"           /* nsfsel_init (the SELECT engine, M4-5)       */
#include "nsftcp.h"           /* nsftcp_reserve / _init / _protops (M4-5)    */
#include "nsfudp.h"           /* nsfudp_reserve / _init / _protops (M4-5)    */
#include "nsfsx.h"            /* the Phase-2 cross-AS request transport      */
#include <clibos.h>           /* clib_apf_setup (SVC 244 self-authorisation) */
#include <string.h>           /* memcpy / memset (device wiring)             */
#include <clibstae.h>          /* __estae, ESTAE_CREATE/DELETE */
#include <clibsdwa.h>          /* SDWA, SDWARCDE, SDWACWT */

/* The PROFILE.TCPIP member is read from the PROFILE DD in the PROC (spec 14.4;
 * libc370 fopen understands "DD:ddname"). */
#define NSF_PROFILE_DD  "DD:PROFILE"

/* The loaded config (1160 B) -- file scope, not a stack frame on the executive
 * task (spec 14.4 keeps large init buffers off the stack). Read-only after a
 * successful parse; the operator DISPLAY reads it via nsf_active_cfg. */
static NSFCFG g_cfg;

/*
 * ESTAE recovery (spec 17.1). MVS enters here on any unhandled abend on the
 * executive task. First-failure data is already captured for the dump: the
 * trace ring ("NSFTRACE"), the statistics registry ("NSFSTATS") and every pool
 * header carry eyecatchers (spec 17.2). We WTO a marker, attempt the SAME
 * orderly teardown the clean path uses (nsf_shutdown: disarm the timer exit,
 * free the pool regions), then percolate (SDWARCDE = SDWACWT == 0) so RTM
 * produces the dump and terminates the address space -- no Hercules restart is
 * ever needed to clean up (goal, ADR-0006). Minimal + defensive: it may run in
 * a damaged environment, so it does the least that is useful and never loops.
 */
static void nsf_recover(SDWA *sdwa)
{
    nsfmsg("NSF900E NSF ABEND INTERCEPTED -- CAPTURING AND PERCOLATING");
    nsf_shutdown();
    nsfmsg("NSF901I NSF EMERGENCY TEARDOWN COMPLETE");
    if (sdwa != NULL) {
        sdwa->SDWARCDE = SDWACWT;       /* continue with termination = percolate */
    }
}

/*
 * EV_PACKET_RECEIVED terminus. The device bottom half hands a raw IP packet up
 * as EV_PACKET_RECEIVED (p1 = PBUF*, u1 = device index); this handler traces it
 * and passes it to nsfip_input, which TAKES OWNERSHIP (validates, demuxes, and
 * frees or hands off the PBUF). nsfip_input frees every path, so this handler
 * must not free after the hand-off (single owner, §3).
 */
static void nsf_rx_packet(EVT *ev)
{
    PBUF   *b   = (PBUF *)ev->p1;
    NETDEV *dev = dev_by_index(ev->u1);

    if (b == NULL) {
        return;
    }
    TRC(DRIVER, "RX %u bytes on dev %u", (unsigned)buf_chain_len(b),
        (unsigned)ev->u1);
    /* Hexdump the packet head into the trace ring (flag-gated inside; F
     * NSF,TRACE DRIVER ON enables it) -- the M1 exit gate's "ping -> hexdump in
     * trace". Bounded: the IP header + a little payload tells the story. */
    {
        UCHAR  head[48];
        USHORT hn = buf_copyout(b, head, (USHORT)sizeof(head));
        nsftrc_hexdump(TRCF_DRIVER, "RX", head, hn);
    }
    nsfip_input(dev, b);                 /* takes ownership of b */
}

/* -- device wiring (PROFILE DEVICE/LINK/HOME -> NSFDEV) --------------------- *
 * Charset-transparent name compare over NUL-padded fixed fields (literals only,
 * no hardcoded byte values), matching nsfstc.c. */
static int nsf_name_eq(const char *a, const char *b, UINT len)
{
    UINT i;

    for (i = 0u; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;                   /* both padded from here on */
        }
    }
    return 1;
}

/* Number of DEVICE statements of the CTC type. */
static UINT nsf_ctc_count(const NSFCFG *cfg)
{
    UINT i, n = 0u;

    for (i = 0u; i < cfg->ndev; i++) {
        if (cfg->dev[i].type == NSFCFG_DEV_CTC) {
            n++;
        }
    }
    return n;
}

/* Build a DEVCFG for DEVICE index `di` by resolving the LINK bound to it (its
 * name is the interface name) and the HOME address bound to that LINK. Referential
 * integrity (LINK->DEVICE, HOME->LINK) was validated at init, but a DEVICE need
 * not have a LINK -- then the interface name falls back to the device name and
 * the HOME stays 0 (the device still comes up; IP binding is M2). */
static void nsf_devcfg(const NSFCFG *cfg, UINT di, DEVCFG *out)
{
    const NSFCFGDEV *dv = &cfg->dev[di];
    UINT             i, j;

    memset(out, 0, sizeof(*out));
    out->cuu  = dv->cuu;
    out->type = NSFDEV_T_CTCI;
    out->mtu  = 1500;                   /* v1 default (IP/MTU refinement is M2) */
    memcpy(out->name, dv->name, sizeof(out->name));   /* fallback: device name */

    for (i = 0u; i < cfg->nlink; i++) {
        if (!nsf_name_eq(cfg->link[i].devname, dv->name, (UINT)NSFCFG_NAMELEN)) {
            continue;
        }
        memcpy(out->name, cfg->link[i].name, sizeof(out->name)); /* LINK name */
        for (j = 0u; j < cfg->nhome; j++) {
            if (nsf_name_eq(cfg->home[j].link, cfg->link[i].name,
                            (UINT)NSFCFG_NAMELEN)) {
                out->ipaddr = cfg->home[j].ip;
                break;
            }
        }
        break;
    }
}

/* Register + start every configured CTCI interface (called after the pools are
 * sealed). A device that fails to register or start is reported and skipped --
 * one bad interface must not stop the rest (or the whole stack). */
static void nsf_start_devices(const NSFCFG *cfg)
{
    UINT i;

    dev_init();                         /* fresh table; forces the loop re-wire */
    for (i = 0u; i < cfg->ndev; i++) {
        DEVCFG  dc;
        NETDEV *nd;

        if (cfg->dev[i].type != NSFCFG_DEV_CTC) {
            continue;
        }
        nsf_devcfg(cfg, i, &dc);
        nd = dev_register(&dc, ctci_devops());
        if (nd == NULL) {
            nsfmsg("NSF213E CTCI %04X REGISTER FAILED", (unsigned)dc.cuu);
            continue;
        }
        if (dev_start(nd) != 0) {
            nsfmsg("NSF212E CTCI %04X FAILED TO START", (unsigned)dc.cuu);
            continue;
        }
        nsfmsg("NSF211I INTERFACE %.8s CUU %04X UP", dc.name, (unsigned)dc.cuu);
    }
}

/* Quiesce one device (dev_foreach callback): close the channel, free I/O
 * buffers, return pool storage -- so SVC 99 allocations release cleanly before
 * mm_shutdown. */
static void nsf_quiesce_device(NETDEV *dev, void *arg)
{
    (void)arg;
    dev_shutdown(dev);
}

int main(int argc, char **argv)
{
    char err[NSFCFG_MSGLEN];
    INT  rc;

    (void)argc;

    /* Self-authorise at runtime (clib_apf_setup -> SVC 244), BEFORE anything
     * touches CSA. The transport needs __super, a key-0 getmain and a store
     * into the SVCTABLE; this is what lets NSF.LINKLIB stay non-APF. The STC
     * authorises itself -- the CLIENT never does, which is the ADR-0038 red
     * line. Same call, same place, as the Stage-0 probe STC. */
    if (clib_apf_setup(argv[0]) != 0) {
        nsfmsg("NSF009E NSFS CANNOT SELF-AUTHORISE");
        return 8;
    }

    /* Foundation FFDC surfaces first (static storage, no pool), so even a very
     * early failure leaves a trace ring + eyecatchers in the dump. */
    nsftrc_init();
    sts_init();

    nsfmsg("NSF040I NSFS %s STARTING (PHASE 2)", NSF_VERSION);

    /* 1. Load + fully validate the PROFILE member (all-or-nothing, spec 14.1). */
    rc = cfg_load(NSF_PROFILE_DD, &g_cfg);
    if (rc != 0) {
        nsfmsg("%s", g_cfg.err.msg);            /* the rendered NSF7xxE + line */
        nsfmsg("NSF009E NSF INITIALIZATION FAILED, RC=%d", (int)rc);
        return 8;
    }
    if (g_cfg.nwarn > 0u) {
        nsfmsg("NSF719W %u CONFIG STATEMENT(S) IGNORED", (unsigned)g_cfg.nwarn);
    }

    /* 2. Open the init window; build the foundation pools. */
    if (mm_init(NULL) != 0) {
        nsfmsg("NSF008E NSF MEMORY INITIALIZATION FAILED");
        return 8;
    }
    nsftmr_init();
    if (nsfevt_init() != 0) {
        nsfmsg("NSF008E NSF EVENT POOL CREATE FAILED");
        mm_shutdown();
        return 8;
    }

    /* 3. Wire the config into the component inits (buffer pools sized from
     *    NSFPOOL, NSFTRACE flags, referential integrity). Refuse to start on any
     *    error -- no partial config survives (spec 14.1). */
    rc = nsf_init_from_cfg(&g_cfg, err, sizeof(err));
    if (rc != 0) {
        nsfmsg("%s", err);
        nsfmsg("NSF009E NSF INITIALIZATION FAILED, RC=%d", (int)rc);
        mm_shutdown();
        return 8;
    }

    /* 3b. Reserve device region storage (still in the init window). CTCI pools
     *     must exist before the seal; the CTCIDEV/buffers themselves are
     *     mm_alloc'd at dev_start, after the seal (only mm_pool_create is
     *     sealed). Refuse to start if the reserve fails. */
    {
        UINT nctc = nsf_ctc_count(&g_cfg);
        if (nctc > 0u && ctci_reserve(nctc, CTCI_BUF_DEFAULT) != 0) {
            nsfmsg("NSF009E NSF INITIALIZATION FAILED, RC=%d", 8);
            mm_shutdown();
            return 8;
        }
    }

    /* 3c. Reserve the socket-layer pools (M4-5): the SOCKET table, the TCP TCBs and
     *     the UDP PCBs. Still in the init window (mm_pool_create). This makes the
     *     STC a full stack -- an inbound SYN to a closed port now draws a correct
     *     RST (nsftcp_input) instead of an ICMP protocol-unreachable, and an
     *     inbound UDP datagram to an unbound port a port-unreachable -- rather than
     *     the IP layer's blanket proto-unreachable. Refuse to start on any failure. */
    if (soc_reserve(0u) != 0 || nsftcp_reserve(0u) != 0 || nsfudp_reserve(0u) != 0) {
        nsfmsg("NSF009E NSF INITIALIZATION FAILED, RC=%d", 8);
        mm_shutdown();
        return 8;
    }

    mm_init_complete();                          /* seal: mm_pool_create is closed */

    /* 4. Operator interface (CIB/QEDIT); add its console ECB to the ECBLIST. */
    if (nsfopr_init() != 0) {
        nsfmsg("NSF807E NSF CONSOLE INTERFACE UNAVAILABLE");
        nsf_shutdown();
        return 8;
    }
    evt_set_operator(nsfopr_ecb(), nsfopr_drain);

    /* 5. Register + start the configured interfaces (M1-4), then build the IP
     *    layer on top (M2). The inbound terminus handler passes each received
     *    PBUF to nsfip_input. nsfip_config resolves each route's device by LINK
     *    name, so it MUST run AFTER the interfaces are registered. */
    evt_register(EV_PACKET_RECEIVED, nsf_rx_packet);
    nsf_start_devices(&g_cfg);
    nsficmp_init();                              /* register ICMP counters */
    (void)nsfip_config(&g_cfg);                  /* routing table from HOME/GATEWAY */

    /* 5b. Socket layer + transports + the request/SELECT path (M4-5). The socket
     *     table + request transport + SELECT engine, then TCP and UDP register
     *     their inbound demux with NSFIP (AFTER nsfip_config) and their PROTOPS with
     *     NSFREQ, and the requestECB joins the ECBLIST. Inert until an application
     *     drives a request (Phase 1 has none in the STC), but the inbound demux is
     *     live: TCP answers a closed port with a RST, UDP an unbound port with a
     *     port-unreachable. */
    soc_init();
    nsfreq_init();
    nsfsel_init();
    nsftcp_init();
    nsfudp_init();
    (void)nsfreq_register_proto(6u,  nsftcp_protops());
    (void)nsfreq_register_proto(17u, nsfudp_protops());
    /* THE PHASE BOUNDARY (ADR-0041).  The same evt_set_request seam the
     * Phase-1 STC uses, wired to the cross-address-space transport: requests
     * now arrive from another address space through the CSA anchor instead of
     * from a local subtask through the CS-safe queue.  nsfreq_init() above
     * still runs -- it resets the app registry and the protocol table, which
     * the dispatcher needs -- but nsfreq_submit/_drain/_ecb have no job here:
     * nothing in this address space submits, and nsfsx_drain calls
     * nsfreq_dispatch() directly on the STC-private copy. */
    if (nsfsx_start() != 0) {
        nsfmsg("NSF009E NSFS TRANSPORT INITIALIZATION FAILED");
        nsf_shutdown();
        return 8;
    }
    evt_set_request(nsfsx_ecb(), nsfsx_drain, nsfsx_pending);

    /* The transport's own STATS supplement (issue #64, step 64-0): the raw
     * wake-ECB word with its POSTED bit decoded, next to the counters that
     * interpret it. Phase 2 only -- src/nsfmain.c registers nothing, so the
     * Phase-1 STATS reply is unchanged. Registered AFTER nsfsx_start so the
     * first STATS already reports a live anchor. */
    nsfopr_set_stats_extra(nsfsx_stats_extra);
    /* The SRM swappability verb (issue #64, step 64-3-1).  Phase 2 only:
     * SYSEVENT needs the APF authorisation, supervisor state and key 0 this
     * STC already holds and the Phase-1 module does not.  Registering it here
     * -- rather than on the startup path -- means a wrong answer cannot break
     * a normal `S NSFS`, and the action is operator-gated and reversible. */
    nsfopr_set_swap(nsfswap_op);

    /* 6. ESTAE from init onward (ADR-0006): recovery uses the same teardown. */
    __estae(ESTAE_CREATE, (void *)nsf_recover, NULL);

    nsfmsg("NSF001I NSFS INITIALIZATION COMPLETE");

    /* 7. Arm the idle-liveness heartbeat, then run the executive until STOP /
     *    P NSF (evt_mainloop runs the §5.4 shutdown sequence internally).
     *
     *    WHAT THIS PROVIDES UNDER THE ADR-0034 CONTRACT. It is ONLY an
     *    operator-liveness wake while the timer queue is empty. It does NOT
     *    affect timer cadence any more: under the fixed contract nsftmr owns the
     *    STIMER (tmr_start arms on the empty->nonempty bootstrap; the loop calls
     *    nsftmr_wake, which advances the ARMED tick count), so a real timer
     *    cleanly displaces this 1-tick arm with its own delta and the queue
     *    drives itself. The old comment here claimed this arm "keeps the loop's
     *    nsftmr_run(1) tick accounting correct" -- that was the issue #40
     *    misconception; the accounting is now correct via nsftmr_wake regardless.
     *
     *    The arm is a DIRECT platform call, deliberately outside nsftmr's
     *    g_armed bookkeeping: it is harmless because it only operates when the
     *    queue is empty (g_armed == 0), where nsftmr_wake advances 0 -- a no-op
     *    that just wakes the loop so an operator MODIFY arriving while the stack
     *    is IDLE (an idle network stack is the normal state) is serviced. The
     *    async STIMER exit is self-re-arming (ADR-0017), so one arm gives the
     *    idle heartbeat; a real timer's arm replaces the interval, and drain
     *    disarms (spec 6.3). Cost while idle: 10 interrupts/s -- ADR-0011's
     *    "idle stack takes zero timer interrupts" is consciously traded for
     *    operator liveness here (unchanged from prior behaviour).
     *
     *    KEPT, not removed: removing it would restore true zero-idle-interrupts,
     *    but whether the console CIB ECB alone wakes the WAIT on a MODIFY when
     *    idle is an EMPIRICAL question, and the existing TSTEVTM only counts
     *    heartbeats over an empty queue -- it does NOT issue an idle MODIFY, so
     *    it cannot prove redundancy. ADR-0034 forbids removing on reasoning
     *    alone; a proper idle-MODIFY liveness test is the gate for that (future
     *    work). evt_shutdown disarms it (nsftmr_plat_disarm). */
    nsftmr_plat_arm(1u);
    evt_mainloop();

    /* 8. Final teardown. Quiesce devices first (close channels, release SVC 99
     *    allocations + I/O buffers) while the pools still exist, then delete the
     *    ESTAE (so a fault while releasing pools cannot re-enter recovery) and
     *    release the regions. */
    /* Transport down FIRST: restore the SVC slot so no new client can enter,
     * invalidate the published wake-ECB address (that storage dies with this
     * address space), wake anyone parked and drain the in-flight count --
     * all while the pools and the executive state still exist. */
    nsfsx_stop();
    dev_foreach(nsf_quiesce_device, NULL);
    __estae(ESTAE_DELETE, NULL, NULL);
    nsf_shutdown();
    nsfmsg("NSF011I NSFS SHUTDOWN COMPLETE");
    return 0;
}
