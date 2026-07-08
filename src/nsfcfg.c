/*
 * nsfcfg.c -- the Configuration parser (see nsfcfg.h, spec ch. 14).
 *
 * A single pass over the member text. Each physical line is copied into a
 * bounded local buffer, its ';' comment stripped, and split into whitespace-
 * delimited tokens; the first token selects a statement handler. Any error
 * aborts the whole parse (spec 14.1) -- the handler renders an NSF7xxE message
 * with the 1-based line number into out->err and cfg_parse returns the code.
 * A successful parse stamps the "NSFCFG  " eyecatcher as its LAST act, so a
 * consumer can trust "eyecatcher present" == "fully valid config".
 *
 * CHARSET TRANSPARENCY. Every character test goes through the cfg_* helpers
 * below; none hardcodes a byte value, and none assumes letters collate
 * contiguously across the whole alphabet. The upcase and hex helpers rely only
 * on the sub-ranges that are contiguous in BOTH EBCDIC and ASCII (0-9; A-I and
 * a-i, hence A-F/a-f; J-R/j-r; S-Z/s-z), so the identical source parses an
 * EBCDIC PDS member on MVS and the ASCII host corpus. See nsfcfg.h.
 */
#include "nsfcfg.h"

#include <stdio.h>              /* snprintf, FILE, fopen, fread, fgetc */
#include <string.h>            /* memset, memcpy */

/* Longest physical line we accept; a longer line is a syntax error (NSF700E)
 * rather than a silent truncation. Comfortably over the 80-column PROFILE
 * convention. */
#define NSFCFG_LINE_MAX   256

/* Most tokens on one line. The widest statement (TCPCONFIG with both size
 * keyword/value pairs) needs 5; 16 is generous headroom and bounds the loop. */
#define NSFCFG_MAX_TOK     16

/* cfg_load's init-window read buffer. A PROFILE.TCPIP member for a hobbyist
 * stack is a few hundred bytes to ~1 KB; 4 KB is ample. File-scope BSS, used
 * only at init -- chosen over a 4 KB stack frame on the executive task (the
 * more dangerous resource on MVS) at the cost of bounded, benign BSS. */
#define NSFCFG_FILE_MAX  4096

/* -------- character helpers (the charset-transparency core) -------------- */

/* EBCDIC/ASCII-safe uppercase. Folds only within the three letter blocks that
 * are contiguous on both code pages; every other byte is returned unchanged.
 * No hardcoded byte values, no full-alphabet contiguity assumption. */
static char cfg_toupper(char c)
{
    if (c >= 'a' && c <= 'i') return (char)('A' + (c - 'a'));
    if (c >= 'j' && c <= 'r') return (char)('J' + (c - 'j'));
    if (c >= 's' && c <= 'z') return (char)('S' + (c - 's'));
    return c;
}

/* Token delimiters. '\r' is folded to whitespace so a CRLF host file's
 * trailing CR is consumed. (Line splitting handles '\n' separately.) */
static int cfg_isspace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* One hex digit's value, or -1. Uses only the EBCDIC-contiguous sub-ranges. */
static int cfg_hexval(char c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');  /* A-F within A-I */
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');  /* a-f within a-i */
    return -1;
}

/* Case-insensitive compare of a NUL-terminated token against a keyword literal
 * (kw is uppercase in the source; cfg_toupper of an uppercase letter is
 * itself). Matches only when both end together. */
static int cfg_kweq(const char *tok, const char *kw)
{
    while (*tok != '\0' && *kw != '\0') {
        if (cfg_toupper(*tok) != cfg_toupper(*kw)) return 0;
        tok++;
        kw++;
    }
    return *tok == '\0' && *kw == '\0';
}

/* Case-insensitive compare of a stored NUL-padded field (max fsize bytes, no
 * NUL if it exactly fills the field) against a NUL-terminated token. Used for
 * duplicate-name detection. */
static int cfg_nameq(const char *field, unsigned fsize, const char *tok)
{
    unsigned i;
    for (i = 0; i < fsize; i++) {
        if (cfg_toupper(field[i]) != cfg_toupper(tok[i])) return 0;
        if (field[i] == '\0') return 1;      /* both reached the terminator */
    }
    return tok[fsize] == '\0';               /* field full: token ends here */
}

/* Copy up to dstsize bytes of a NUL-terminated field; dst is pre-zeroed by the
 * caller's whole-struct memset, so a shorter value leaves the tail zeroed and a
 * value that exactly fills the field needs no NUL (every read is bounded).
 * Mirrors sts_copyfield. */
static void cfg_copyfield(char *dst, unsigned dstsize, const char *src)
{
    unsigned i;
    for (i = 0; i < dstsize && src != NULL && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
}

/* -------- value parsers ------------------------------------------------- */

/* Parse an unsigned decimal that fits in 32 bits. Returns 0 on success (all
 * digits, no overflow), -1 on any non-digit, an empty string, or overflow.
 * Range checking against a field maximum is the caller's (so it can raise the
 * precise NSFCFG_E_RANGE vs NSFCFG_E_SYNTAX). */
static int cfg_parse_uint(const char *s, UINT *out)
{
    UINT v = 0;
    int  any = 0;

    if (s == NULL) return -1;
    for (; *s != '\0'; s++) {
        UINT d;
        if (*s < '0' || *s > '9') return -1;         /* 0-9 contiguous both CPs */
        d = (UINT)(*s - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) return -1;   /* 32-bit overflow */
        v = v * 10u + d;
        any = 1;
    }
    if (!any) return -1;
    *out = v;
    return 0;
}

/* Parse dotted-decimal IPv4 into a UINT with the first octet in the MSB
 * (10.1.2.3 -> 0x0A010203). Rejects an octet > 255, an empty octet (leading /
 * trailing / doubled dot), fewer or more than four octets, and any trailing
 * junk. Returns 0 / -1. */
static int cfg_parse_ipv4(const char *s, UINT *out)
{
    UINT v = 0;
    int  oct;

    if (s == NULL) return -1;
    for (oct = 0; oct < 4; oct++) {
        UINT o = 0;
        int  ndig = 0;

        if (oct > 0) {
            if (*s != '.') return -1;       /* missing separator / too few */
            s++;
        }
        while (*s >= '0' && *s <= '9') {
            o = o * 10u + (UINT)(*s - '0');
            if (o > 255u) return -1;         /* octet out of range */
            s++;
            ndig++;
        }
        if (ndig == 0) return -1;            /* empty octet */
        v = (v << 8) | o;
    }
    if (*s != '\0') return -1;               /* extra dot / fifth octet / junk */
    *out = v;
    return 0;
}

/* Parse a 1-4 digit hex device address (cuu) into a USHORT. Rejects a non-hex
 * digit, an empty token, or more than four digits. Returns 0 / -1. */
static int cfg_parse_cuu(const char *s, USHORT *out)
{
    UINT v = 0;
    int  ndig = 0;

    if (s == NULL || *s == '\0') return -1;
    for (; *s != '\0'; s++) {
        int h = cfg_hexval(*s);
        if (h < 0) return -1;
        v = (v << 4) | (UINT)h;
        if (++ndig > 4) return -1;
    }
    *out = (USHORT)v;
    return 0;
}

/* -------- error reporting ---------------------------------------------- */

/* Record an error and abort the parse. Zeroes the whole config first so NO
 * partial config survives (spec 14.1) and no eyecatcher is left behind; only
 * out->err is meaningful afterward. Returns the code so handlers can
 * `return cfg_err(...)`. `line` is 1-based, or 0 for a whole-config error. */
static INT cfg_err(NSFCFG *out, UINT line, UINT code, const char *text)
{
    memset(out, 0, sizeof(*out));
    out->err.line = line;
    out->err.code = code;
    snprintf(out->err.msg, sizeof(out->err.msg), "NSF%03uE %s",
             (unsigned)code, text);
    return (INT)code;
}

/* -------- line tokenizer ----------------------------------------------- */

/* Split `line` in place into whitespace-delimited tokens (NUL-terminating each
 * by overwriting its trailing delimiter). Returns the token count, or -1 if
 * there are more than maxtok tokens. */
static int cfg_tokenize(char *line, char **tok, int maxtok)
{
    int   n = 0;
    char *p = line;

    for (;;) {
        while (*p != '\0' && cfg_isspace(*p)) p++;
        if (*p == '\0') break;
        if (n >= maxtok) return -1;
        tok[n++] = p;
        while (*p != '\0' && !cfg_isspace(*p)) p++;
        if (*p != '\0') { *p = '\0'; p++; }
    }
    return n;
}

/* Known-but-unconsumed single-line PROFILE.TCPIP statements: recognized as
 * valid syntax so a real profile does not fail to parse, but not yet acted on
 * by NSF v1 -- each occurrence is counted (out->nwarn) and skipped with a
 * warning (spec 14.1). Deliberately NONE are block statements (AUTOLOG,
 * ASSORTEDPARMS, ...): a stray block body then fails loud as unknown-statement
 * errors rather than being mis-skipped. */
static int cfg_is_ignorable(const char *stmt)
{
    static const char *const ign[] = {
        "TRANSLATE", "DATASETPREFIX", "ARPAGE", "INFORM"
    };
    unsigned i;
    for (i = 0; i < sizeof(ign) / sizeof(ign[0]); i++) {
        if (cfg_kweq(stmt, ign[i])) return 1;
    }
    return 0;
}

/* -------- statement handlers ------------------------------------------- *
 * Each validates operand count and values, checks for duplicates and array
 * overflow, and appends one entry. On any error it returns cfg_err's code
 * (which has already zeroed out); on success it returns NSFCFG_OK. */

/* DEVICE devname CTC cuu */
static INT cfg_do_device(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGDEV *d;
    USHORT     cuu;
    UINT       j;

    if (n != 4)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "DEVICE needs: name CTC cuu");
    if (!cfg_kweq(t[2], "CTC"))
        return cfg_err(o, line, NSFCFG_E_KEYWORD, "DEVICE type must be CTC");
    if (cfg_parse_cuu(t[3], &cuu) != 0)
        return cfg_err(o, line, NSFCFG_E_CUU, "malformed device address (cuu)");
    for (j = 0; j < o->ndev; j++)
        if (cfg_nameq(o->dev[j].name, NSFCFG_NAMELEN, t[1]))
            return cfg_err(o, line, NSFCFG_E_DUP, "duplicate DEVICE name");
    if (o->ndev >= NSFCFG_MAX_DEVICES)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many DEVICE statements");

    d = &o->dev[o->ndev++];
    cfg_copyfield(d->name, NSFCFG_NAMELEN, t[1]);
    d->cuu  = cuu;
    d->type = NSFCFG_DEV_CTC;
    return NSFCFG_OK;
}

/* LINK link CTC linknum devname */
static INT cfg_do_link(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGLINK *l;
    UINT        num;
    UINT        j;

    if (n != 5)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "LINK needs: name CTC num dev");
    if (!cfg_kweq(t[2], "CTC"))
        return cfg_err(o, line, NSFCFG_E_KEYWORD, "LINK type must be CTC");
    if (cfg_parse_uint(t[3], &num) != 0)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "LINK number must be numeric");
    if (num > 255)
        return cfg_err(o, line, NSFCFG_E_RANGE, "LINK number out of range");
    for (j = 0; j < o->nlink; j++)
        if (cfg_nameq(o->link[j].name, NSFCFG_NAMELEN, t[1]))
            return cfg_err(o, line, NSFCFG_E_DUP, "duplicate LINK name");
    if (o->nlink >= NSFCFG_MAX_LINKS)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many LINK statements");

    l = &o->link[o->nlink++];
    cfg_copyfield(l->name,    NSFCFG_NAMELEN, t[1]);
    cfg_copyfield(l->devname, NSFCFG_NAMELEN, t[4]);
    l->type = NSFCFG_DEV_CTC;
    return NSFCFG_OK;
}

/* HOME ipaddr link */
static INT cfg_do_home(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGHOME *h;
    UINT        ip;
    UINT        j;

    if (n != 3)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "HOME needs: ipaddr link");
    if (cfg_parse_ipv4(t[1], &ip) != 0)
        return cfg_err(o, line, NSFCFG_E_IPADDR, "malformed HOME address");
    for (j = 0; j < o->nhome; j++)
        if (o->home[j].ip == ip)
            return cfg_err(o, line, NSFCFG_E_DUP, "duplicate HOME address");
    if (o->nhome >= NSFCFG_MAX_HOMES)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many HOME statements");

    h = &o->home[o->nhome++];
    h->ip = ip;
    cfg_copyfield(h->link, NSFCFG_NAMELEN, t[2]);
    return NSFCFG_OK;
}

/* GATEWAY (net|DEFAULTNET) firsthop link mtu (mask|0) */
static INT cfg_do_gateway(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGGW *g;
    UINT      net = 0, firsthop, mask = 0, mtu;
    UCHAR     is_default = 0;

    if (n != 6)
        return cfg_err(o, line, NSFCFG_E_SYNTAX,
                       "GATEWAY needs: net hop link mtu mask");
    if (cfg_kweq(t[1], "DEFAULTNET")) {
        is_default = 1;
    } else if (cfg_parse_ipv4(t[1], &net) != 0) {
        return cfg_err(o, line, NSFCFG_E_IPADDR, "malformed GATEWAY network");
    }
    if (cfg_parse_ipv4(t[2], &firsthop) != 0)
        return cfg_err(o, line, NSFCFG_E_IPADDR, "malformed GATEWAY first hop");
    if (cfg_parse_uint(t[4], &mtu) != 0)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "GATEWAY mtu must be numeric");
    if (mtu < 68 || mtu > 65535)
        return cfg_err(o, line, NSFCFG_E_RANGE, "GATEWAY mtu out of range");
    if (!cfg_kweq(t[5], "0") && cfg_parse_ipv4(t[5], &mask) != 0)
        return cfg_err(o, line, NSFCFG_E_MASK, "malformed GATEWAY subnet mask");
    if (o->ngw >= NSFCFG_MAX_GATEWAYS)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many GATEWAY statements");

    g = &o->gw[o->ngw++];
    g->net        = net;
    g->mask       = mask;
    g->firsthop   = firsthop;
    g->mtu        = (USHORT)mtu;
    g->is_default = is_default;
    cfg_copyfield(g->link, NSFCFG_NAMELEN, t[3]);
    return NSFCFG_OK;
}

/* PORT port (TCP|UDP) jobname */
static INT cfg_do_port(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGPORT *p;
    UINT        port;
    UCHAR       proto;
    UINT        j;

    if (n != 4)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "PORT needs: port proto job");
    if (cfg_parse_uint(t[1], &port) != 0)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "PORT number must be numeric");
    if (port < 1 || port > 65535)
        return cfg_err(o, line, NSFCFG_E_RANGE, "PORT number out of range");
    if (cfg_kweq(t[2], "TCP"))
        proto = NSFCFG_PROTO_TCP;
    else if (cfg_kweq(t[2], "UDP"))
        proto = NSFCFG_PROTO_UDP;
    else
        return cfg_err(o, line, NSFCFG_E_KEYWORD, "PORT protocol must be TCP/UDP");
    for (j = 0; j < o->nport; j++)
        if (o->port[j].port == (USHORT)port && o->port[j].proto == proto)
            return cfg_err(o, line, NSFCFG_E_DUP, "duplicate PORT reservation");
    if (o->nport >= NSFCFG_MAX_PORTS)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many PORT statements");

    p = &o->port[o->nport++];
    p->port  = (USHORT)port;
    p->proto = proto;
    cfg_copyfield(p->jobname, NSFCFG_JOBLEN, t[3]);
    return NSFCFG_OK;
}

/* NSFPOOL poolname count */
static INT cfg_do_nsfpool(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGPOOL *pl;
    UINT        count;
    UINT        j;

    if (n != 3)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "NSFPOOL needs: name count");
    if (cfg_parse_uint(t[2], &count) != 0)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "NSFPOOL count must be numeric");
    if (count < 1 || count > 1000000)
        return cfg_err(o, line, NSFCFG_E_RANGE, "NSFPOOL count out of range");
    for (j = 0; j < o->npool; j++)
        if (cfg_nameq(o->pool[j].name, NSFCFG_POOLLEN, t[1]))
            return cfg_err(o, line, NSFCFG_E_DUP, "duplicate NSFPOOL name");
    if (o->npool >= NSFCFG_MAX_POOLS)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many NSFPOOL statements");

    pl = &o->pool[o->npool++];
    cfg_copyfield(pl->name, NSFCFG_POOLLEN, t[1]);
    pl->count = count;
    return NSFCFG_OK;
}

/* NSFTRACE comp ON|OFF */
static INT cfg_do_nsftrace(NSFCFG *o, char **t, int n, UINT line)
{
    NSFCFGTRACE *tr;
    UCHAR        on;
    UINT         j;

    if (n != 3)
        return cfg_err(o, line, NSFCFG_E_SYNTAX, "NSFTRACE needs: comp ON|OFF");
    if (cfg_kweq(t[2], "ON"))
        on = 1;
    else if (cfg_kweq(t[2], "OFF"))
        on = 0;
    else
        return cfg_err(o, line, NSFCFG_E_KEYWORD, "NSFTRACE flag must be ON/OFF");
    for (j = 0; j < o->ntrace; j++)
        if (cfg_nameq(o->trace[j].comp, NSFCFG_COMPLEN, t[1]))
            return cfg_err(o, line, NSFCFG_E_DUP, "duplicate NSFTRACE component");
    if (o->ntrace >= NSFCFG_MAX_TRACES)
        return cfg_err(o, line, NSFCFG_E_OVERFLOW, "too many NSFTRACE statements");

    tr = &o->trace[o->ntrace++];
    cfg_copyfield(tr->comp, NSFCFG_COMPLEN, t[1]);
    tr->on = on;
    return NSFCFG_OK;
}

/* TCPCONFIG [RECVBUFRSIZE n] [SENDBUFRSIZE n] -- singleton, at least one kw. */
static INT cfg_do_tcpconfig(NSFCFG *o, char **t, int n, UINT line)
{
    int i;

    if (o->tcp.present)
        return cfg_err(o, line, NSFCFG_E_DUP, "duplicate TCPCONFIG statement");

    for (i = 1; i < n; i++) {
        UINT v;
        if (cfg_kweq(t[i], "RECVBUFRSIZE") || cfg_kweq(t[i], "SENDBUFRSIZE")) {
            int is_recv = cfg_kweq(t[i], "RECVBUFRSIZE");
            if (i + 1 >= n)
                return cfg_err(o, line, NSFCFG_E_SYNTAX,
                               "TCPCONFIG size keyword needs a value");
            if (cfg_parse_uint(t[i + 1], &v) != 0)
                return cfg_err(o, line, NSFCFG_E_SYNTAX,
                               "TCPCONFIG size must be numeric");
            if (v < 256 || v > 262144)
                return cfg_err(o, line, NSFCFG_E_RANGE,
                               "TCPCONFIG buffer size out of range");
            if (is_recv) o->tcp.recvbufsize = v;
            else         o->tcp.sendbufsize = v;
            i++;                            /* consumed the value token */
        } else {
            return cfg_err(o, line, NSFCFG_E_KEYWORD, "unknown TCPCONFIG keyword");
        }
    }
    o->tcp.present = 1;
    return NSFCFG_OK;
}

/* UDPCONFIG [UDPCHKSUM|NOUDPCHKSUM] -- singleton. */
static INT cfg_do_udpconfig(NSFCFG *o, char **t, int n, UINT line)
{
    int i;

    if (o->udp.present)
        return cfg_err(o, line, NSFCFG_E_DUP, "duplicate UDPCONFIG statement");

    for (i = 1; i < n; i++) {
        if (cfg_kweq(t[i], "UDPCHKSUM"))
            o->udp.chksum = 1;
        else if (cfg_kweq(t[i], "NOUDPCHKSUM"))
            o->udp.chksum = 0;
        else
            return cfg_err(o, line, NSFCFG_E_KEYWORD, "unknown UDPCONFIG keyword");
    }
    o->udp.present = 1;
    return NSFCFG_OK;
}

/* -------- public entry points ------------------------------------------ */

INT cfg_parse(const char *buf, UINT len, NSFCFG *out)
{
    UINT pos  = 0;
    UINT line = 0;

    memset(out, 0, sizeof(*out));
    out->udp.chksum = 1;                 /* IBM default: UDP checksums ON */

    while (pos < len) {
        char  lb[NSFCFG_LINE_MAX];
        char *tok[NSFCFG_MAX_TOK];
        UINT  ll = 0;
        int   ntok;
        int   i;
        INT   rc;

        line++;                          /* every physical line advances it */

        /* Copy one physical line [pos .. '\n') into lb, bounded. */
        while (pos < len && buf[pos] != '\n') {
            if (ll >= sizeof(lb) - 1)
                return cfg_err(out, line, NSFCFG_E_SYNTAX, "line too long");
            lb[ll++] = buf[pos];
            pos++;
        }
        if (pos < len) pos++;            /* consume the '\n' */
        lb[ll] = '\0';

        /* Truncate at a ';' comment before tokenizing. */
        for (i = 0; lb[i] != '\0'; i++) {
            if (lb[i] == ';') { lb[i] = '\0'; break; }
        }

        ntok = cfg_tokenize(lb, tok, NSFCFG_MAX_TOK);
        if (ntok < 0)
            return cfg_err(out, line, NSFCFG_E_SYNTAX, "too many tokens on line");
        if (ntok == 0)
            continue;                    /* blank or comment-only line */

        if      (cfg_kweq(tok[0], "DEVICE"))    rc = cfg_do_device(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "LINK"))      rc = cfg_do_link(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "HOME"))      rc = cfg_do_home(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "GATEWAY"))   rc = cfg_do_gateway(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "PORT"))      rc = cfg_do_port(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "TCPCONFIG")) rc = cfg_do_tcpconfig(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "UDPCONFIG")) rc = cfg_do_udpconfig(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "NSFPOOL"))   rc = cfg_do_nsfpool(out, tok, ntok, line);
        else if (cfg_kweq(tok[0], "NSFTRACE"))  rc = cfg_do_nsftrace(out, tok, ntok, line);
        else if (cfg_is_ignorable(tok[0])) {
            out->nwarn++;                /* recognized, not consumed in v1 */
            rc = NSFCFG_OK;
        } else {
            return cfg_err(out, line, NSFCFG_E_STMT, "unknown statement");
        }

        if (rc != NSFCFG_OK)
            return rc;                   /* out->err already set + zeroed */
    }

    /* Required statements. A stack with no HOME address is unusable (spec 14.1
     * names HOME as the example required statement). Whole-config error: line 0. */
    if (out->nhome == 0)
        return cfg_err(out, 0, NSFCFG_E_MISSING, "missing required HOME statement");

    memcpy(out->eye, "NSFCFG  ", sizeof(out->eye));   /* success: stamp last */
    return NSFCFG_OK;
}

INT cfg_load(const char *name, NSFCFG *out)
{
    static char g_cfgbuf[NSFCFG_FILE_MAX];
    FILE  *f;
    size_t nread;

    f = fopen(name, "r");
    if (f == NULL)
        return cfg_err(out, 0, NSFCFG_E_OPEN, "cannot open config member");

    nread = fread(g_cfgbuf, 1, sizeof(g_cfgbuf), f);
    /* A full buffer with at least one more byte means the member overflows the
     * load buffer -- reject rather than parse a truncated config. */
    if (nread == sizeof(g_cfgbuf) && fgetc(f) != EOF) {
        fclose(f);
        return cfg_err(out, 0, NSFCFG_E_TOOBIG, "config member too large");
    }
    fclose(f);

    return cfg_parse(g_cfgbuf, (UINT)nread, out);
}
