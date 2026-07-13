/*
 * nsfstc.c -- STC startup sequencing + NSFCFG->component-init wiring (nsfstc.h).
 *
 * Portable C only (no MVS services), so the whole cfg->init mapping and the
 * refuse-to-start failure path are host-unit-tested (test/tststc.c). The MVS
 * main (src/nsfmain.c) supplies the loaded NSFCFG and drives the loop; this file
 * turns config into component state and owns the shared teardown.
 *
 * CHARSET TRANSPARENCY (spec 15.3): name comparisons here are byte-wise over
 * NUL-padded fields and against string literals only -- no hardcoded byte
 * values -- so the same source is correct on EBCDIC (cc370) and ASCII (host).
 */
#include "nsfstc.h"
#include "nsftrc.h"             /* TRCF_*, nsftrc_enable / nsftrc_disable */
#include "nsfbuf.h"             /* buf_init_counts, NSFBUF_*_COUNT        */
#include "nsfmm.h"              /* mm_shutdown                            */
#include "nsfstim.h"            /* nsftmr_plat_disarm (stop the timer exit) */
#include "nsffmt.h"             /* nsf_snprintf (libc370's own snprintf does
                                 * not NUL-terminate on truncation, #25.2) */

/* The config remembered by the last successful nsf_init_from_cfg. */
static const NSFCFG *g_active_cfg;

/* -- name helpers (charset-transparent: byte compares, literals only) ------- */

/* Compare a NUL-padded fixed field (up to `len` bytes) with a C string. */
static int field_eq(const char *field, UINT len, const char *name)
{
    UINT i;

    for (i = 0u; i < len; i++) {
        if (field[i] != name[i]) {
            return 0;
        }
        if (name[i] == '\0') {          /* both ended within the field */
            return 1;
        }
    }
    return (name[len] == '\0');          /* consumed the whole field: exact fit */
}

/* Compare two NUL-padded fixed fields of the same width. */
static int names_eq(const char *a, const char *b, UINT len)
{
    UINT i;

    for (i = 0u; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {             /* both padded from here on */
            return 1;
        }
    }
    return 1;
}

/* -- trace flags ----------------------------------------------------------- */

UINT nsf_trace_flag(const char *comp)
{
    if (comp == NULL) {
        return 0u;
    }
    if (field_eq(comp, 8u, "IP"))     { return TRCF_IP;     }
    if (field_eq(comp, 8u, "TCP"))    { return TRCF_TCP;    }
    if (field_eq(comp, 8u, "UDP"))    { return TRCF_UDP;    }
    if (field_eq(comp, 8u, "ICMP"))   { return TRCF_ICMP;   }
    if (field_eq(comp, 8u, "DRIVER")) { return TRCF_DRIVER; }
    if (field_eq(comp, 8u, "MEMORY")) { return TRCF_MEMORY; }
    if (field_eq(comp, 8u, "TIMER"))  { return TRCF_TIMER;  }
    if (field_eq(comp, 8u, "SOCKET")) { return TRCF_SOCKET; }
    if (field_eq(comp, 8u, "API"))    { return TRCF_API;    }
    if (field_eq(comp, 8u, "CONFIG")) { return TRCF_CONFIG; }
    if (field_eq(comp, 8u, "ALL"))    { return TRCF_ALL;    }
    return 0u;
}

UINT nsf_cfg_apply_trace(const NSFCFG *cfg)
{
    UINT i;
    UINT applied = 0u;

    if (cfg == NULL) {
        return 0u;
    }
    for (i = 0u; i < cfg->ntrace; i++) {
        UINT flag = nsf_trace_flag(cfg->trace[i].comp);
        if (flag == 0u) {
            continue;                   /* unknown component name: skip */
        }
        if (cfg->trace[i].on) {
            nsftrc_enable(flag);
        } else {
            nsftrc_disable(flag);
        }
        applied++;
    }
    return applied;
}

/* -- pool counts ----------------------------------------------------------- */

UINT nsf_cfg_pool_count(const NSFCFG *cfg, const char *name, UINT deflt)
{
    UINT i;

    if (cfg == NULL || name == NULL) {
        return deflt;
    }
    for (i = 0u; i < cfg->npool; i++) {
        if (field_eq(cfg->pool[i].name, (UINT)NSFCFG_POOLLEN, name)) {
            return cfg->pool[i].count;
        }
    }
    return deflt;
}

/* -- referential integrity (spec 14.5) ------------------------------------- */

/* Is `name` (a NUL-padded NSFCFG_NAMELEN field) one of the `n` defined names in
 * the record array `defs`, each at byte offset `off` within a `stride`-byte
 * record? A tiny generic walk so LINK/HOME/GATEWAY share one lookup. */
static int name_defined(const char *name, const void *defs, UINT n,
                        UINT stride, UINT off)
{
    const char *base = (const char *)defs;
    UINT        i;

    for (i = 0u; i < n; i++) {
        if (names_eq(name, base + (i * stride) + off, (UINT)NSFCFG_NAMELEN)) {
            return 1;
        }
    }
    return 0;
}

static void render(char *errbuf, UINT errsize, const char *fmt,
                   UINT code, const char *nm)
{
    if (errbuf != NULL && errsize > 0u) {
        nsf_snprintf(errbuf, errsize, fmt, (unsigned)code, nm);
    }
}

INT nsf_cfg_check_refs(const NSFCFG *cfg, char *errbuf, UINT errsize)
{
    UINT i;

    if (cfg == NULL) {
        return NSFSTC_OK;
    }

    /* Every LINK must name a defined DEVICE. */
    for (i = 0u; i < cfg->nlink; i++) {
        if (!name_defined(cfg->link[i].devname, cfg->dev, cfg->ndev,
                          (UINT)sizeof(cfg->dev[0]),
                          (UINT)offsetof(NSFCFGDEV, name))) {
            if (errbuf != NULL && errsize > 0u) {
                nsf_snprintf(errbuf, errsize,
                            "NSF%03uE LINK %.16s NAMES UNDEFINED DEVICE %.16s",
                            (unsigned)NSFSTC_E_LINKDEV,
                            cfg->link[i].name, cfg->link[i].devname);
            }
            return NSFSTC_E_LINKDEV;
        }
    }

    /* Every HOME must name a defined LINK. */
    for (i = 0u; i < cfg->nhome; i++) {
        if (!name_defined(cfg->home[i].link, cfg->link, cfg->nlink,
                          (UINT)sizeof(cfg->link[0]),
                          (UINT)offsetof(NSFCFGLINK, name))) {
            render(errbuf, errsize, "NSF%03uE HOME NAMES UNDEFINED LINK %.16s",
                   (UINT)NSFSTC_E_HOMELNK, cfg->home[i].link);
            return NSFSTC_E_HOMELNK;
        }
    }

    /* Every GATEWAY must name a defined LINK. */
    for (i = 0u; i < cfg->ngw; i++) {
        if (!name_defined(cfg->gw[i].link, cfg->link, cfg->nlink,
                          (UINT)sizeof(cfg->link[0]),
                          (UINT)offsetof(NSFCFGLINK, name))) {
            render(errbuf, errsize, "NSF%03uE GATEWAY NAMES UNDEFINED LINK %.16s",
                   (UINT)NSFSTC_E_GWLINK, cfg->gw[i].link);
            return NSFSTC_E_GWLINK;
        }
    }

    return NSFSTC_OK;
}

/* -- the wiring step ------------------------------------------------------- */

INT nsf_init_from_cfg(const NSFCFG *cfg, char *errbuf, UINT errsize)
{
    INT  rc;
    UINT small;
    UINT large;

    /* 1. Reject an internally inconsistent config before creating anything. */
    rc = nsf_cfg_check_refs(cfg, errbuf, errsize);
    if (rc != NSFSTC_OK) {
        return rc;
    }

    /* 2. Create the buffer pools sized from NSFPOOL (init window). */
    small = nsf_cfg_pool_count(cfg, "BUFSMALL", (UINT)NSFBUF_SMALL_COUNT);
    large = nsf_cfg_pool_count(cfg, "BUFLARGE", (UINT)NSFBUF_LARGE_COUNT);
    if (buf_init_counts(small, large) != 0) {
        if (errbuf != NULL && errsize > 0u) {
            nsf_snprintf(errbuf, errsize,
                        "NSF%03uE BUFFER POOL CREATION FAILED",
                        (unsigned)NSFSTC_E_BUFPOOL);
        }
        return NSFSTC_E_BUFPOOL;
    }

    /* 3. Apply the startup trace flags (static state, no allocation). */
    (void)nsf_cfg_apply_trace(cfg);

    /* 4. Remember the config for the operator DISPLAY and later consumers. */
    g_active_cfg = cfg;
    return NSFSTC_OK;
}

const NSFCFG *nsf_active_cfg(void)
{
    return g_active_cfg;
}

/* -- shared teardown (orderly exit and ESTAE recovery, spec 17.1) ---------- */

void nsf_shutdown(void)
{
    nsftmr_plat_disarm();       /* stop the async STIMER exit (idempotent)     */
    mm_shutdown();              /* release every pool region                   */
    g_active_cfg = NULL;
}
