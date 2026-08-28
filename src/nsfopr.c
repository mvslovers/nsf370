/*
 * nsfopr.c -- the portable operator command dispatcher (see nsfopr.h).
 *
 * Feeds on a MODIFY command string ("DISPLAY", "TRACE IP ON", "STATS", "STOP",
 * "HELP") and acts on it, writing NSF8xx replies through nsfmsg. No MVS
 * services, so the whole routing table is host-tested (test/tstopr.c) by
 * feeding strings and reading nsfmsg's capture ring / observing nsftrc_flags /
 * the stop request. The CIB seam (src/nsfopr_plat*.c) only delivers the string.
 *
 * Console input arrives uppercase on MVS; this dispatcher upper-cases anyway so
 * a lowercase host test still matches. Matching is against string literals only
 * (charset-transparent, spec 15.3); toupper is the platform's own (EBCDIC under
 * cc370, ASCII on the host), exactly as the ecosystem STCs do (ufsd).
 */
#include "nsfopr.h"
#include "nsfmsg.h"
#include "nsfevt.h"             /* nsfevt_stop */
#include "nsfstc.h"             /* nsf_trace_flag, nsf_active_cfg */
#include "nsftrc.h"             /* nsftrc_flags, nsftrc_enable / _disable */
#include "nsfsts.h"             /* sts_render, sts_count */
#include <string.h>            /* strlen, memcpy, memcmp */
#include <ctype.h>             /* toupper */

#define OPR_CMDMAX  120         /* longest MODIFY text we act on (CIB is small) */

/* Optional STATS supplement (issue #64, step 64-0).  NULL until a build
 * registers one, so Phase 1's `F NSF,STATS` reply is unchanged; Phase 2 wires
 * nsfsx_stats_extra here from nsfsmain.  Deliberately a seam and not an
 * #ifdef: nsfopr.c is shared by both STCs and host-tested as pure C. */
static void (*g_statsextra)(void);

/* Optional SWAP verb (issue #64, step 64-3-1).  NULL until a build registers
 * one, so Phase 1's dispatcher is unchanged -- `F NSF,SWAP` falls through to
 * NSF808E exactly as any other unknown verb, and the help text does not grow.
 * Only the Phase-2 STC registers a handler (nsfswap_op), because SYSEVENT
 * needs APF authorisation, supervisor state and key 0 and the Phase-1 module
 * has none of the three.  Same seam shape as g_statsextra above, and for the
 * same reason: nsfopr.c is shared by both STCs and host-tested as pure C. */
static void (*g_swapfn)(const char *);

/* -- tiny tokenizer over the uppercased work buffer ------------------------ */

static const char *skip_sep(const char *p)
{
    while (*p == ' ' || *p == ',') {
        p++;
    }
    return p;
}

static const char *tok_end(const char *p)
{
    while (*p != '\0' && *p != ' ' && *p != ',') {
        p++;
    }
    return p;
}

/* token [p,e) equals the literal `lit`? (byte compare; literal is per-platform) */
static int tok_is(const char *p, const char *e, const char *lit)
{
    UINT n = (UINT)(e - p);
    return (strlen(lit) == n && memcmp(p, lit, n) == 0);
}

/* -- command handlers ------------------------------------------------------ */

static void op_display(void)
{
    const NSFCFG *cfg = nsf_active_cfg();

    nsfmsg("NSF800I NSF %s EXECUTIVE READY", NSF_VERSION);
    if (cfg != NULL) {
        nsfmsg("NSF801I CONFIG %u DEVICE %u LINK %u HOME %u GATEWAY %u PORT",
               (unsigned)cfg->ndev, (unsigned)cfg->nlink, (unsigned)cfg->nhome,
               (unsigned)cfg->ngw, (unsigned)cfg->nport);
    } else {
        nsfmsg("NSF801I CONFIG (NONE)");
    }
    nsfmsg("NSF802I TRACE FLAGS %04X", (unsigned)(nsftrc_flags & 0xFFFFu));
}

static void op_stats(void)
{
    char  buf[512];
    UINT  n;
    char *line;

    nsfmsg("NSF810I STATS %u COUNTER(S)", (unsigned)sts_count());
    n = sts_render(buf, sizeof(buf));
    line = buf;
    while ((UINT)(line - buf) < n) {
        char *nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        nsfmsg("NSF811I %s", line);
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }

    /* After the rendered counters, so the supplement is the LAST thing on the
     * reply and cannot be pushed out by sts_render's fixed buffer. */
    if (g_statsextra != NULL) {
        g_statsextra();
    }
}

void nsfopr_set_stats_extra(void (*fn)(void))
{
    g_statsextra = fn;
}

void nsfopr_set_swap(void (*fn)(const char *))
{
    g_swapfn = fn;
}

static void op_trace(const char *rest)
{
    const char *c  = skip_sep(rest);
    const char *ce = tok_end(c);
    const char *s  = skip_sep(ce);
    const char *se = tok_end(s);
    char        comp[16];
    UINT        clen = (UINT)(ce - c);
    UINT        flag;

    if (clen == 0u || se == s) {
        nsfmsg("NSF828E TRACE SYNTAX: TRACE COMP ON|OFF");
        return;
    }
    if (clen >= sizeof(comp)) {
        clen = sizeof(comp) - 1u;
    }
    memcpy(comp, c, clen);
    comp[clen] = '\0';

    flag = nsf_trace_flag(comp);
    if (flag == 0u) {
        nsfmsg("NSF829E TRACE UNKNOWN COMPONENT %s", comp);
        return;
    }
    if (tok_is(s, se, "ON")) {
        nsftrc_enable(flag);
        nsfmsg("NSF820I TRACE %s ON", comp);
    } else if (tok_is(s, se, "OFF")) {
        nsftrc_disable(flag);
        nsfmsg("NSF820I TRACE %s OFF", comp);
    } else {
        nsfmsg("NSF828E TRACE SYNTAX: TRACE COMP ON|OFF");
    }
}

static void op_help(void)
{
    nsfmsg("NSF880I NSF MODIFY COMMANDS:");
    nsfmsg("NSF880I   DISPLAY            -- executive state + config summary");
    nsfmsg("NSF880I   STATS             -- statistics counters");
    nsfmsg("NSF880I   TRACE comp ON|OFF -- e.g. F NSF,TRACE IP ON");
    nsfmsg("NSF880I   STOP  (or P NSF)  -- orderly shutdown");
    if (g_swapfn != NULL) {
        nsfmsg("NSF880I   SWAP              -- SRM swappability probe");
    }
}

/* -- the router ------------------------------------------------------------ */

int nsfopr_dispatch(const char *text)
{
    char        buf[OPR_CMDMAX + 1];
    UINT        i;
    UINT        len;
    const char *p;
    const char *e;
    const char *rest;

    if (text == NULL) {
        return 0;
    }

    /* Copy, upper-case, and trim trailing blanks into a bounded work buffer. */
    len = 0u;
    for (i = 0u; text[i] != '\0' && len < OPR_CMDMAX; i++) {
        buf[len++] = (char)toupper((unsigned char)text[i]);
    }
    while (len > 0u && buf[len - 1u] == ' ') {
        len--;
    }
    buf[len] = '\0';

    p    = skip_sep(buf);
    e    = tok_end(p);
    rest = skip_sep(e);

    if (p == e) {                       /* empty command: treat as HELP */
        op_help();
        return 0;
    }
    if (tok_is(p, e, "DISPLAY") || tok_is(p, e, "D")) {
        op_display();
        return 0;
    }
    if (tok_is(p, e, "STATS")) {
        op_stats();
        return 0;
    }
    if (tok_is(p, e, "TRACE") || tok_is(p, e, "T")) {
        op_trace(rest);
        return 0;
    }
    if (tok_is(p, e, "STOP")) {
        nsfmsg("NSF830I NSF STOP ACCEPTED -- SHUTTING DOWN");
        nsfevt_stop();
        return 1;
    }
    if (tok_is(p, e, "SWAP") && g_swapfn != NULL) {
        g_swapfn(rest);
        return 0;
    }
    if (tok_is(p, e, "HELP") || tok_is(p, e, "?")) {
        op_help();
        return 0;
    }

    nsfmsg("NSF808E UNKNOWN COMMAND %.*s", (int)(e - p), p);
    op_help();
    return 0;
}
