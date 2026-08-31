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

/* Optional APPS verb (M5-2c1): the app registry with a liveness verdict per
 * slot.  NULL until a build registers one, so Phase 1's dispatcher is
 * unchanged -- same seam shape as g_statsextra above and g_swapfn below. */
static void (*g_appsfn)(void);

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

/* Render EVERY registered counter, and say so if we ever fail to (issue #92).
 *
 * THE DEFECT THIS REPLACES.  This used to be one sts_render into one 512-byte
 * buffer, which fitted about 32 of 52 counters -- and the cut point MOVED,
 * because each line carries the counter's value, so a counter gaining a digit
 * pushed a later one off the end.  Nothing said so: the reply simply ended.  An
 * operator saw "NSF810I STATS 52 COUNTER(S)" followed by 32 lines and had to
 * count them to notice.  A measurement round reading counters from this reply
 * (which is what (e) does) would read a set silently missing a fifth of itself,
 * with a different fifth missing as the numbers grew.
 *
 * TWO CHANGES, and the second is the durable one:
 *   1. COMPLETE: resume with sts_render_from until every counter is out, so the
 *      buffer bounds one CHUNK rather than the whole reply.
 *   2. VISIBLE: count what was emitted and compare it against what exists.  If
 *      they ever differ, say so loudly (NSF818W) instead of just ending.  This
 *      should never fire -- the loop above is what makes it so -- but a
 *      renderer that CAN drop output must be able to report that it did, or the
 *      next counter added past some future boundary is lost in the same silence
 *      that produced #92.  It is also why nsfsx_stats_extra stops being the
 *      reason a counter is placed outside the registry: it was a workaround for
 *      this, and it worked, and it was not a fix. */
#define OPR_STATS_CHUNK 512

/* The STATS render chunk.  A VARIABLE rather than a constant for exactly one
 * reason, and it is the point of the visibility half: NSF818W is UNREACHABLE in
 * production -- a rendered line is at most ~33 bytes and the chunk is 512, so
 * the loop always makes progress and emitted always equals total.  Without a
 * way to shrink the chunk, the warning could never be OBSERVED to fire, and a
 * warning nobody has ever seen fire is the same "absence indistinguishable from
 * success" shape this issue is about, one level up.  NSF_DEBUG only; the
 * production module compiles the setter out and keeps the 512. */
static UINT g_statschunk = OPR_STATS_CHUNK;

#if NSF_DEBUG
void nsfopr_set_stats_chunk(UINT n)
{
    g_statschunk = (n != 0u && n <= OPR_STATS_CHUNK) ? n : OPR_STATS_CHUNK;
}
#endif

static void op_stats(void)
{
    char  buf[OPR_STATS_CHUNK];
    UINT  total   = sts_count();
    UINT  emitted = 0;
    UINT  first   = 0;

    nsfmsg("NSF810I STATS %u COUNTER(S)", (unsigned)total);

    while (first < total) {
        UINT  next = first;
        UINT  n    = sts_render_from(buf, g_statschunk, first, &next);
        char *line = buf;

        if (next == first) {
            break;                      /* no progress: one line exceeds buf */
        }
        while ((UINT)(line - buf) < n) {
            char *nl = strchr(line, '\n');
            if (nl != NULL) {
                *nl = '\0';
            }
            nsfmsg("NSF811I %s", line);
            emitted++;
            if (nl == NULL) {
                break;
            }
            line = nl + 1;
        }
        first = next;
    }

    /* The completeness check.  Turns a silent loss into a visible one. */
    if (emitted != total) {
        nsfmsg("NSF818W STATS INCOMPLETE -- RENDERED %u OF %u COUNTER(S)",
               (unsigned)emitted, (unsigned)total);
    }

    /* The supplement still comes last.  It no longer has to: it is no longer
     * competing with a truncating renderer for room.  Kept as a mechanism for
     * things that are genuinely not registry counters. */
    if (g_statsextra != NULL) {
        g_statsextra();
    }
}

void nsfopr_set_stats_extra(void (*fn)(void))
{
    g_statsextra = fn;
}

void nsfopr_set_apps(void (*fn)(void))
{
    g_appsfn = fn;
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
    if (g_appsfn != NULL) {
        nsfmsg("NSF880I   APPS              -- app registry + client liveness");
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
    if (tok_is(p, e, "APPS") && g_appsfn != NULL) {
        g_appsfn();
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
