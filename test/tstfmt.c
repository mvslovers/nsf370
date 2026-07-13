/*
 * tstfmt.c -- NSFFMT host unit tests (see nsffmt.h, issue #25.2).
 *
 * Portable C: runs both natively (`make test-host`, where the underlying
 * platform vsnprintf already NUL-terminates on truncation per glibc/C99) and
 * on MVS (where TSTVSNP pins that libc370's does NOT). Either way, the
 * WRAPPER's own contract must hold: always NUL-terminate when size > 0, and
 * return the clamped "characters actually in the buffer" count rather than
 * C99's raw "would-be" length. That clamping logic is a pure, platform-
 * independent transformation -- fully exercised here regardless of which
 * underlying vsnprintf runs underneath.
 */
#include "nsffmt.h"
#include <mbtcheck.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define POISON  0x5Au

static void poison(char *buf, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++) {
        buf[i] = (char)POISON;
    }
}

/* va_list plumbing so nsf_vsnprintf can be probed with the same call shape
 * nsftrc_write / nsfmsg use (a variadic wrapper forwarding into it). */
static int call_vsnprintf(char *buf, unsigned size, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = nsf_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return rc;
}

int main(void)
{
    char buf[64];
    int  n;

    printf("=== nsf370 NSFFMT tests ===\n");

    /* -- truncation: always NUL-terminates, return value is clamped ------- */
    poison(buf, sizeof(buf));
    n = nsf_snprintf(buf, 8u, "%s", "0123456789ABCDEF");   /* 16 chars, size 8 */
    CHECK_EQ((long)buf[7], 0L, "truncation: always NUL-terminated at buf[size-1]");
    CHECK_EQ((long)strlen(buf), 7L, "truncation: 7 data chars before the forced NUL");
    CHECK(strncmp(buf, "0123456", 7) == 0, "truncation: the 7 chars that fit are correct");
    CHECK_EQ((long)n, 7L,
             "truncation: return value is size-1 (chars actually in buf), "
             "not the C99 would-be length");

    /* -- exact fit: no truncation, return value is the plain string length */
    poison(buf, sizeof(buf));
    n = nsf_snprintf(buf, 6u, "%s", "hello");              /* "hello"+NUL == 6 */
    CHECK(strcmp(buf, "hello") == 0, "exact fit: buffer holds the full string");
    CHECK_EQ((long)n, 5L, "exact fit: return value is the string length");

    /* -- size == 0: nothing is written, return value is 0 ------------------ */
    poison(buf, sizeof(buf));
    n = nsf_snprintf(buf, 0u, "%s", "hello");
    CHECK_EQ((long)(unsigned char)buf[0], (long)POISON, "size==0: buffer left untouched");
    CHECK_EQ((long)n, 0L, "size==0: return value is 0 (nothing is in the buffer)");

    /* -- a format with multiple conversions, comfortably truncated --------- */
    poison(buf, sizeof(buf));
    n = nsf_snprintf(buf, 10u, "%s %u %s", "abc", 12345u, "defghijk");
    CHECK_EQ((long)buf[9], 0L, "multi-conversion truncation: still NUL-terminated");
    CHECK_EQ((long)strlen(buf), 9L, "multi-conversion truncation: 9 data chars before the NUL");
    CHECK_EQ((long)n, 9L, "multi-conversion truncation: return value clamped to size-1");

    /* -- va_list form (nsftrc_write / nsfmsg's call shape) ------------------ */
    poison(buf, sizeof(buf));
    n = call_vsnprintf(buf, 5u, "%s", "0123456789");        /* 10 chars, size 5 */
    CHECK_EQ((long)buf[4], 0L, "va_list form: always NUL-terminated at buf[size-1]");
    CHECK_EQ((long)strlen(buf), 4L, "va_list form: 4 data chars before the forced NUL");
    CHECK_EQ((long)n, 4L, "va_list form: return value clamped the same way as nsf_snprintf");

    return mbt_test_summary("TSTFMT");
}
