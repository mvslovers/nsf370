/*
 * tstsmoke.c -- M0-1 build-skeleton smoke test.
 *
 * Purpose: prove the toolchain (cc370 on MVS, native cc on the host), the
 * base types and the NSF_SIZE_ASSERT machinery compile and run before any
 * real code exists. Dual test: runs under `make test-host` (native) and
 * `make test-mvs` (crt1 + main on MVS).
 *
 * Exit code: 0 = pass, 1 = failure (mvslovers test convention).
 */
#include "nsf.h"
#include <stdio.h>

/* Exercise NSF_SIZE_ASSERT at compile time on a known-size struct. */
typedef struct { UINT a; UINT b; } smoke_cb;
NSF_SIZE_ASSERT(smoke_cb, 8);

int main(void)
{
    int failures = 0;

    if (sizeof(UCHAR)  != 1) { printf("FAIL: sizeof(UCHAR) = %u\n",  (UINT)sizeof(UCHAR));  failures++; }
    if (sizeof(USHORT) != 2) { printf("FAIL: sizeof(USHORT) = %u\n", (UINT)sizeof(USHORT)); failures++; }
    if (sizeof(UINT)   != 4) { printf("FAIL: sizeof(UINT) = %u\n",   (UINT)sizeof(UINT));   failures++; }
    if (sizeof(INT)    != 4) { printf("FAIL: sizeof(INT) = %u\n",    (UINT)sizeof(INT));    failures++; }

    if (failures == 0) {
        printf("nsf370 %s smoke test: OK\n", NSF_VERSION);
        return 0;
    }
    printf("nsf370 smoke test: %d failure(s)\n", failures);
    return 1;
}
