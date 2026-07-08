/*
 * tstcfg.c -- NSFCFG host unit tests (spec ch. 14).
 *
 * Drives the PROFILE.TCPIP parser against the corpus under test/cfg/ (loaded
 * through cfg_load, which exercises the fopen/fread wrapper too) plus a few
 * inline cfg_parse buffers for the dotted-decimal edge cases. Portable C:
 * builds and runs natively via `make test-host` (the CI gate) with NSF_DEBUG=1.
 *
 * Covers the M0-7 Definition of Done:
 *   - every statement type parses and populates NSFCFG with the expected values
 *     (valid_full), incl. the MSB-first IPv4 encoding and the TCP/UDP scalars;
 *   - the minimal (HOME-only) and cosmetics (comments, blank + tab-only lines,
 *     leading/inline tabs, extra spaces, mixed case) profiles;
 *   - ignorable statements warn + continue (counted, rc still 0);
 *   - one broken profile per error class, each asserting rejection, the NSF7xx
 *     code, AND the exact 1-based line number, plus the missing eyecatcher;
 *   - dotted-decimal edge cases (256, empty octet, extra dots, too few octets);
 *   - case-insensitivity that does not lean on ASCII collation.
 *
 * CHARSET NOTE. On the host these files are ASCII; on MVS the identical parser
 * reads the same members as EBCDIC PDS records. The parser compares literals
 * only and folds case block-wise, so no assertion here depends on the code page
 * (see the "no hardcoded byte value" scan in the parser and its header).
 */
#include "nsfcfg.h"
#include <mbtcheck.h>
#include <string.h>

/* Compare a NUL-padded fixed field (which may exactly fill its array with no
 * terminator) against an expected string: the prefix must match and any
 * remainder must be zero padding. */
static int feq(const char *field, unsigned fsize, const char *want)
{
    unsigned wl = (unsigned)strlen(want);
    if (wl > fsize) return 0;
    if (memcmp(field, want, wl) != 0) return 0;
    if (wl < fsize && field[wl] != '\0') return 0;
    return 1;
}

/* Load a corpus member by base name. Paths are relative to the repo root, which
 * is the working directory `make test-host` runs the binary from. */
static INT load(const char *base, NSFCFG *c)
{
    char path[128];
    snprintf(path, sizeof(path), "test/cfg/%s", base);
    return cfg_load(path, c);
}

/* Assert that a broken profile is rejected with `code` on 1-based `line`
 * (line 0 == whole-config), and that no eyecatcher is left behind. */
static void reject(const char *base, UINT code, UINT line)
{
    NSFCFG c;
    INT    rc;
    char   m[96];

    rc = load(base, &c);
    snprintf(m, sizeof(m), "%s: rejected with code %u", base, (unsigned)code);
    CHECK(rc == (INT)code && c.err.code == code, m);
    snprintf(m, sizeof(m), "%s: error reported on line %u", base, (unsigned)line);
    CHECK_EQ((long)c.err.line, (long)line, m);
    snprintf(m, sizeof(m), "%s: no eyecatcher on a rejected config", base);
    CHECK(memcmp(c.eye, "NSFCFG  ", 8) != 0, m);
}

/* Parse an inline buffer and return the result code (for the dotted-decimal
 * edge cases, which are cleaner as literals than as separate corpus files). */
static INT parse_str(const char *s, NSFCFG *c)
{
    return cfg_parse(s, (UINT)strlen(s), c);
}

int main(void)
{
    NSFCFG c;
    INT    rc;

    printf("=== nsf370 NSFCFG tests ===\n");

    /* ---- fixed, pointer-free layout (host sizeof == target sizeof) ---- */
    CHECK_EQ((long)sizeof(NSFCFG),      1160, "NSFCFG is 1160 bytes on the host");
    CHECK_EQ((long)sizeof(NSFCFGDEV),     20, "NSFCFGDEV is 20 bytes");
    CHECK_EQ((long)sizeof(NSFCFGGW),      32, "NSFCFGGW is 32 bytes");
    CHECK_EQ((long)sizeof(NSFCFGPORT),    12, "NSFCFGPORT is 12 bytes");

    /* ---- sentinel: prove the corpus is reachable from the cwd ---- */
    rc = load("valid_minimal.profile", &c);
    if (rc == (INT)NSFCFG_E_OPEN) {
        printf("  FAIL: cannot open test/cfg/ -- run `make test-host` "
               "from the repo root\n");
        return 1;
    }

    /* ================= valid: minimal (HOME only) ================= */
    CHECK_EQ((long)rc, 0, "valid_minimal parses");
    CHECK(memcmp(c.eye, "NSFCFG  ", 8) == 0, "valid_minimal stamps the eyecatcher");
    CHECK_EQ((long)c.err.code, 0, "valid_minimal reports no error code");
    CHECK_EQ((long)c.err.line, 0, "valid_minimal reports no error line");
    CHECK_EQ((long)c.nhome, 1, "valid_minimal has one HOME");
    CHECK_EQ((long)c.home[0].ip, (long)0x7F000001u, "127.0.0.1 -> 0x7F000001 (MSB first)");
    CHECK(feq(c.home[0].link, NSFCFG_NAMELEN, "LOOPBACK"), "HOME link is LOOPBACK");
    CHECK_EQ((long)c.udp.chksum, 1, "UDP checksum defaults ON with no UDPCONFIG");
    CHECK_EQ((long)c.ndev, 0, "valid_minimal has no DEVICE");

    /* ================= valid: full (every statement) ================= */
    rc = load("valid_full.profile", &c);
    CHECK_EQ((long)rc, 0, "valid_full parses");
    CHECK(memcmp(c.eye, "NSFCFG  ", 8) == 0, "valid_full stamps the eyecatcher");
    CHECK_EQ((long)c.nwarn, 0, "valid_full raises no warnings");

    /* DEVICE */
    CHECK_EQ((long)c.ndev, 1, "one DEVICE");
    CHECK(feq(c.dev[0].name, NSFCFG_NAMELEN, "CTCA"), "DEVICE name is CTCA");
    CHECK_EQ((long)c.dev[0].cuu, (long)0x0E20, "DEVICE cuu 0E20 parsed as hex");
    CHECK_EQ((long)c.dev[0].type, NSFCFG_DEV_CTC, "DEVICE type is CTC");

    /* LINK */
    CHECK_EQ((long)c.nlink, 1, "one LINK");
    CHECK(feq(c.link[0].name,    NSFCFG_NAMELEN, "LNK1"), "LINK name is LNK1");
    CHECK(feq(c.link[0].devname, NSFCFG_NAMELEN, "CTCA"), "LINK binds to CTCA");

    /* HOME */
    CHECK_EQ((long)c.nhome, 1, "one HOME");
    CHECK_EQ((long)c.home[0].ip, (long)0x0A010102u, "HOME 10.1.1.2 -> 0x0A010102");
    CHECK(feq(c.home[0].link, NSFCFG_NAMELEN, "LNK1"), "HOME link is LNK1");

    /* GATEWAY (default + explicit) */
    CHECK_EQ((long)c.ngw, 2, "two GATEWAYs");
    CHECK_EQ((long)c.gw[0].is_default, 1, "gw[0] is DEFAULTNET");
    CHECK_EQ((long)c.gw[0].firsthop, (long)0x0A010101u, "gw[0] first hop 10.1.1.1");
    CHECK_EQ((long)c.gw[0].mtu, 1500, "gw[0] mtu 1500");
    CHECK_EQ((long)c.gw[0].mask, 0, "gw[0] mask 0 (default route)");
    CHECK_EQ((long)c.gw[1].is_default, 0, "gw[1] is a specific net");
    CHECK_EQ((long)c.gw[1].net,  (long)0xC0A80000u, "gw[1] net 192.168.0.0");
    CHECK_EQ((long)c.gw[1].mask, (long)0xFFFFFF00u, "gw[1] mask 255.255.255.0");

    /* PORT */
    CHECK_EQ((long)c.nport, 2, "two PORTs");
    CHECK_EQ((long)c.port[0].port, 23, "port[0] is 23");
    CHECK_EQ((long)c.port[0].proto, NSFCFG_PROTO_TCP, "port[0] is TCP");
    CHECK(feq(c.port[0].jobname, NSFCFG_JOBLEN, "TELNET"), "port[0] job TELNET");
    CHECK_EQ((long)c.port[1].port, 161, "port[1] is 161");
    CHECK_EQ((long)c.port[1].proto, NSFCFG_PROTO_UDP, "port[1] is UDP");

    /* TCPCONFIG / UDPCONFIG scalars */
    CHECK_EQ((long)c.tcp.present, 1, "TCPCONFIG present");
    CHECK_EQ((long)c.tcp.recvbufsize, 16384, "TCPCONFIG RECVBUFRSIZE 16384");
    CHECK_EQ((long)c.tcp.sendbufsize, 16384, "TCPCONFIG SENDBUFRSIZE 16384");
    CHECK_EQ((long)c.udp.present, 1, "UDPCONFIG present");
    CHECK_EQ((long)c.udp.chksum, 0, "UDPCONFIG NOUDPCHKSUM turns checksums off");

    /* NSFPOOL */
    CHECK_EQ((long)c.npool, 2, "two NSFPOOLs");
    CHECK(feq(c.pool[0].name, NSFCFG_POOLLEN, "PBUFSMAL"), "pool[0] name PBUFSMAL");
    CHECK_EQ((long)c.pool[0].count, 256, "pool[0] count 256");
    CHECK(feq(c.pool[1].name, NSFCFG_POOLLEN, "PBUFLARG"), "pool[1] name PBUFLARG");
    CHECK_EQ((long)c.pool[1].count, 64, "pool[1] count 64");

    /* NSFTRACE */
    CHECK_EQ((long)c.ntrace, 2, "two NSFTRACEs");
    CHECK(feq(c.trace[0].comp, NSFCFG_COMPLEN, "IP"), "trace[0] comp IP");
    CHECK_EQ((long)c.trace[0].on, 1, "trace[0] IP ON");
    CHECK(feq(c.trace[1].comp, NSFCFG_COMPLEN, "TCP"), "trace[1] comp TCP");
    CHECK_EQ((long)c.trace[1].on, 0, "trace[1] TCP OFF");

    /* ================= valid: cosmetics (case, tabs, comments) ======= */
    rc = load("valid_cosmetics.profile", &c);
    CHECK_EQ((long)rc, 0, "valid_cosmetics parses");
    CHECK_EQ((long)c.nwarn, 0, "valid_cosmetics raises no warnings");
    /* lowercase keyword + extra spaces; storage keeps the original case */
    CHECK_EQ((long)c.ndev, 1, "cosmetics: lowercase 'device' recognized");
    CHECK(feq(c.dev[0].name, NSFCFG_NAMELEN, "ctcb"), "cosmetics: device name kept as typed");
    CHECK_EQ((long)c.dev[0].cuu, (long)0x0F10, "cosmetics: lowercase hex cuu 0f10");
    /* leading + inline tabs as delimiters */
    CHECK_EQ((long)c.nhome, 1, "cosmetics: tab-delimited HOME recognized");
    CHECK_EQ((long)c.home[0].ip, (long)0x0A090807u, "cosmetics: HOME 10.9.8.7");
    CHECK(feq(c.home[0].link, NSFCFG_NAMELEN, "lnk9"), "cosmetics: HOME link lnk9");
    /* ';' with no leading space still ends the statement */
    CHECK_EQ((long)c.nport, 1, "cosmetics: PORT before a no-space comment");
    CHECK(feq(c.port[0].jobname, NSFCFG_JOBLEN, "HTTPD"), "cosmetics: job HTTPD, comment stripped");
    /* mixed-case keyword + ON/OFF value */
    CHECK_EQ((long)c.ntrace, 1, "cosmetics: mixed-case NSFTRACE recognized");
    CHECK_EQ((long)c.trace[0].on, 1, "cosmetics: mixed-case oN parsed as ON");

    /* ================= valid: ignorable statements warn + continue === */
    rc = load("valid_ignorable.profile", &c);
    CHECK_EQ((long)rc, 0, "valid_ignorable parses (warnings are not errors)");
    CHECK_EQ((long)c.nwarn, 3, "three ignorable statements counted as warnings");
    CHECK_EQ((long)c.nhome, 1, "ignorable file still captured its HOME");
    CHECK_EQ((long)c.home[0].ip, (long)0x0A000005u, "ignorable file HOME 10.0.0.5");

    /* ================= broken: one class per file, exact line ======== */
    reject("bad_syntax.profile",    NSFCFG_E_SYNTAX,   3);
    reject("bad_range.profile",     NSFCFG_E_RANGE,    3);
    reject("bad_dup.profile",       NSFCFG_E_DUP,      4);
    reject("bad_missing.profile",   NSFCFG_E_MISSING,  0);
    reject("bad_overflow.profile",  NSFCFG_E_OVERFLOW, 7);
    reject("bad_ipaddr.profile",    NSFCFG_E_IPADDR,   2);
    reject("bad_mask.profile",      NSFCFG_E_MASK,     3);
    reject("bad_cuu.profile",       NSFCFG_E_CUU,      3);
    reject("bad_keyword.profile",   NSFCFG_E_KEYWORD,  3);
    reject("bad_statement.profile", NSFCFG_E_STMT,     3);

    /* ================= dotted-decimal edge cases (inline) ============ */
    CHECK_EQ((long)parse_str("HOME 256.1.1.1 L\n",  &c), NSFCFG_E_IPADDR, "octet 256 rejected");
    CHECK_EQ((long)parse_str("HOME 10..1.1 L\n",    &c), NSFCFG_E_IPADDR, "empty octet rejected");
    CHECK_EQ((long)parse_str("HOME 1.2.3.4.5 L\n",  &c), NSFCFG_E_IPADDR, "five octets rejected");
    CHECK_EQ((long)parse_str("HOME 1.2.3 L\n",      &c), NSFCFG_E_IPADDR, "three octets rejected");
    CHECK_EQ((long)parse_str("HOME 1.2.3. L\n",     &c), NSFCFG_E_IPADDR, "trailing dot rejected");
    CHECK_EQ((long)parse_str("HOME 1.2.3.4x L\n",   &c), NSFCFG_E_IPADDR, "trailing junk rejected");

    /* a well-formed inline config still succeeds (control for the above) */
    CHECK_EQ((long)parse_str("HOME 1.2.3.4 L\n", &c), 0, "well-formed inline HOME parses");
    CHECK_EQ((long)c.home[0].ip, (long)0x01020304u, "inline HOME 1.2.3.4 -> 0x01020304");

    /* ================= case-insensitivity is not ASCII-bound ========= */
    CHECK_EQ((long)parse_str("hOmE 10.0.0.9 lZ\n", &c), 0, "mixed-case HOME keyword accepted");
    CHECK_EQ((long)c.nhome, 1, "mixed-case keyword produced a HOME entry");

    return mbt_test_summary("TSTCFG");
}
