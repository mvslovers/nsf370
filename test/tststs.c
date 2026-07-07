/*
 * tststs.c -- NSFSTS host unit tests (spec ch. 08).
 *
 * White-box tests of the statistics registry. Portable C: builds and runs
 * natively via `make test-host` (the CI gate) with NSF_DEBUG=1.
 *
 * Covers: STSCTR is 16 bytes on the host (the value guard NSF_SIZE_ASSERT
 * cannot make on a host build); register hands back distinct, stable counters
 * starting at 0; STS_INC / STS_ADD update the value; the render lists each
 * registered counter as "component name value" in registration order; reset
 * zeroes the values but keeps the registrations; and registering past the fixed
 * maximum is handled by returning NULL without growing the registry.
 */
#include "nsfsts.h"
#include <mbtcheck.h>
#include <string.h>

int main(void)
{
    STSCTR *a, *b, *c;
    char    buf[512];
    UINT    n;
    int     i, allok;

    printf("=== nsf370 NSFSTS tests ===\n");

    /* ---- pointer-free layout: STSCTR must be 16 B here too (spec 8.2) ---- */
    CHECK_EQ((long)sizeof(STSCTR), 16, "STSCTR is 16 bytes on the host");

    /* ---- init leaves an empty registry ---- */
    sts_init();
    CHECK_EQ((long)sts_count(), 0, "init empties the registry");

    /* ---- register returns distinct counters, each starting at 0 ---- */
    a = sts_register("IP",  "in");
    b = sts_register("IP",  "out");
    c = sts_register("TCP", "segsrcv");
    CHECK(a != NULL && b != NULL && c != NULL, "register returns counters");
    CHECK(a != b && b != c && a != c, "each registration is a distinct counter");
    CHECK_EQ((long)a->value, 0, "a new counter starts at 0");
    CHECK_EQ((long)sts_count(), 3, "three counters registered");

    /* ---- STS_INC / STS_ADD update the value ---- */
    STS_INC(a);
    STS_INC(a);
    STS_ADD(b, 10);
    CHECK_EQ((long)a->value, 2, "STS_INC increments the counter");
    CHECK_EQ((long)b->value, 10, "STS_ADD adds n to the counter");
    CHECK_EQ((long)c->value, 0, "an untouched counter stays 0");

    /* ---- render lists component + name + value, in registration order ---- */
    n = sts_render(buf, sizeof(buf));
    CHECK(n > 0, "render writes a non-empty report");
    CHECK_EQ((long)strlen(buf), (long)n, "render returns the byte count it wrote");
    CHECK(strstr(buf, "IP in 2\n")  != NULL, "render shows IP in == 2");
    CHECK(strstr(buf, "IP out 10\n") != NULL, "render shows IP out == 10");
    CHECK(strstr(buf, "TCP segsrcv 0\n") != NULL, "render shows TCP segsrcv == 0");

    /* ---- render into a buffer too small for even one line stays clean ---- */
    {
        char tiny[4];
        UINT t = sts_render(tiny, sizeof(tiny));
        CHECK_EQ((long)t, 0, "render into a too-small buffer writes 0 bytes");
        CHECK_EQ((long)tiny[0], 0, "too-small render still NUL-terminates");
    }

    /* ---- reset zeroes values but keeps the registrations ---- */
    sts_reset();
    CHECK_EQ((long)a->value, 0, "reset zeroes counter a");
    CHECK_EQ((long)b->value, 0, "reset zeroes counter b");
    CHECK_EQ((long)sts_count(), 3, "reset keeps the registrations");

    /* ---- registering past the fixed max is handled (NULL, no growth) ---- */
    sts_init();
    allok = 1;
    for (i = 0; i < NSFSTS_MAX; i++) {
        if (sts_register("FULL", "ctr") == NULL) {
            allok = 0;
        }
    }
    CHECK(allok, "all NSFSTS_MAX registrations succeed");
    CHECK_EQ((long)sts_count(), NSFSTS_MAX, "registry filled to the maximum");
    CHECK(sts_register("OVER", "flow") == NULL,
          "registration past the maximum returns NULL");
    CHECK_EQ((long)sts_count(), NSFSTS_MAX,
             "an overflow registration does not grow the registry");

    return mbt_test_summary("TSTSTS");
}
