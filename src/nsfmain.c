/*
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
#include "nsfevt.h"
#include "nsfmsg.h"
#include "nsfmm.h"
#include "nsftmr.h"
#include "nsftrc.h"
#include "nsfsts.h"
#include "nsfdev.h"            /* dev_register / dev_start / dev_shutdown     */
#include "nsfctci.h"          /* ctci_reserve / ctci_devops                  */
#include "nsfbuf.h"           /* PBUF, buf_free (the RX terminus)            */
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
 * EV_PACKET_RECEIVED terminus (M1-4). Until IP (M2) claims inbound packets, the
 * executive still MUST free every PBUF a device hands up: the loop frees the EVT
 * but never ev->p1, so an unhandled EV_PACKET_RECEIVED leaks the buffer. This
 * handler counts a driver trace line and frees it -- the receive path's honest
 * end for now (the DISPLAY,STATS device ctr_in is the real proof of arrival).
 */
static void nsf_rx_packet(EVT *ev)
{
    PBUF *b = (PBUF *)ev->p1;

    if (b != NULL) {
        TRC(DRIVER, "RX %u bytes on dev %u", (unsigned)buf_chain_len(b),
            (unsigned)ev->u1);
        buf_free(b);
    }
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
    (void)argv;

    /* Foundation FFDC surfaces first (static storage, no pool), so even a very
     * early failure leaves a trace ring + eyecatchers in the dump. */
    nsftrc_init();
    sts_init();

    nsfmsg("NSF000I NSF %s STARTING", NSF_VERSION);

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

    mm_init_complete();                          /* seal: mm_pool_create is closed */

    /* 4. Operator interface (CIB/QEDIT); add its console ECB to the ECBLIST. */
    if (nsfopr_init() != 0) {
        nsfmsg("NSF807E NSF CONSOLE INTERFACE UNAVAILABLE");
        nsf_shutdown();
        return 8;
    }
    evt_set_operator(nsfopr_ecb(), nsfopr_drain);

    /* 5. Register + start the configured interfaces (M1-4). The inbound
     *    terminus handler is mandatory: it frees each received PBUF (the loop
     *    frees the EVT, never ev->p1) until IP claims the packets (M2). */
    evt_register(EV_PACKET_RECEIVED, nsf_rx_packet);
    nsf_start_devices(&g_cfg);

    /* 6. ESTAE from init onward (ADR-0006): recovery uses the same teardown. */
    __estae(ESTAE_CREATE, (void *)nsf_recover, NULL);

    nsfmsg("NSF001I NSF INITIALIZATION COMPLETE");

    /* 7. Run the executive until STOP / P NSF (evt_mainloop runs the §5.4
     *    shutdown sequence internally before it returns). */
    evt_mainloop();

    /* 8. Final teardown. Quiesce devices first (close channels, release SVC 99
     *    allocations + I/O buffers) while the pools still exist, then delete the
     *    ESTAE (so a fault while releasing pools cannot re-enter recovery) and
     *    release the regions. */
    dev_foreach(nsf_quiesce_device, NULL);
    __estae(ESTAE_DELETE, NULL, NULL);
    nsf_shutdown();
    nsfmsg("NSF011I NSF SHUTDOWN COMPLETE");
    return 0;
}
