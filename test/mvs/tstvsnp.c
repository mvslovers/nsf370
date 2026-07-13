/*
 * tstvsnp.c -- pin libc370's vsnprintf/snprintf truncation contract (issue #25.2,
 * host = false: the divergence is invisible on host, so a host test would prove
 * nothing).
 *
 * glibc/C99 (7.19.6.5) NUL-terminate the output within `size` whenever size > 0,
 * even when the formatted result is truncated, and return the number of chars
 * that WOULD have been written (excluding the NUL) had size been unlimited.
 * TSTTRC caught libc370 failing that contract on real MVS (a 255-char source
 * into a 112-byte field came back with strlen == 112 and text[111] != 0).
 *
 * This probes vsnprintf/snprintf DIRECTLY over a CANARY ARENA -- a flat buffer
 * with generous poisoned space both before and after the `size`-bounded target
 * region -- so the question "does it also write PAST `size`" (a memory-safety
 * question, not just a cosmetic one) gets a bounded, reproducible answer instead
 * of undefined-behavior-dependent noise from reading past a bare local array.
 * Every byte examined is inside the arena; nothing here is itself UB.
 *
 * KEEP THIS TEST even after nsffmt.c lands -- it is the reference the wrapper's
 * contract is built against, the same way TSTTHRW keeps pinning the pre-fix
 * timed-wait shape.
 *
 * PINNED RESULT (live on MVSCE, CC 0 both legs -- see issue #25 for the spool):
 *   - `size` IS a hard write bound: truncation never writes past target+size
 *     (not a memory-safety bug -- just a missing terminator).
 *   - On truncation, the target is filled SOLID through byte size-1 with
 *     data; no byte is reserved for a NUL.
 *   - The return value on truncation is still the C99 "would-be length" (the
 *     untruncated source length) -- ONLY the NUL-termination half of the
 *     contract is broken, not the return-value half.
 *   - size == 0 writes nothing at all, and size that exactly fits still
 *     NUL-terminates correctly -- both match C99/glibc.
 * This is exactly what nsf_vsnprintf/nsf_snprintf (src/nsffmt.c) exist to fix:
 * force buf[size-1] = '\0' after the call (always in-bounds, given the first
 * bullet), and clamp the trustworthy-but-unclamped return value.
 */
#include <mbtcheck.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define POISON      0x5Au
#define PRE_LEN     16u
#define TARGET_LEN  16u
#define POST_LEN    64u
#define ARENA_LEN   (PRE_LEN + TARGET_LEN + POST_LEN)

/* A 40-char source: longer than TARGET_LEN so truncation is forced, short
 * enough that even a fully size-blind write (the worst case under test) still
 * lands completely inside POST_LEN -- so this probe can never itself read or
 * write outside `arena`, whatever libc370 turns out to do. */
static const char SRC40[] = "0123456789ABCDEF0123456789ABCDEF01234567";

/* The character at SRC40[TARGET_LEN - 1] ('F', by construction of SRC40 and
 * TARGET_LEN above) -- as a LITERAL, not as SRC40[TARGET_LEN - 1] itself.
 * cc370 constant-folds a literal string indexed by a compile-time constant
 * using the HOST's (ASCII) byte value instead of the target EBCDIC encoding
 * it correctly uses for the runtime array (confirmed live: target[15], a real
 * copy through vsnprintf, read back 0xC6/198 -- correct EBCDIC 'F' -- while
 * SRC40[15] evaluated at compile time to 0x46/70, ASCII 'F'). A single
 * CHARACTER literal doesn't have this problem (the whole codebase relies on
 * cc370 encoding those correctly, per CLAUDE.md); only indexing a literal
 * STRING with a constant index does. Keep this pinned as its own char, not a
 * re-derived index into SRC40. */
#define SRC40_LAST_TARGET_CHAR  'F'

static void poison(unsigned char *p, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++) {
        p[i] = (unsigned char)POISON;
    }
}

/* Count the poisoned bytes in p[0..n) that were overwritten (!= POISON). */
static unsigned count_touched(const unsigned char *p, unsigned n)
{
    unsigned i, touched = 0u;
    for (i = 0u; i < n; i++) {
        if (p[i] != (unsigned char)POISON) {
            touched++;
        }
    }
    return touched;
}

/* Index of the first zero byte in p[0..n), or n if none (a BOUNDED strlen --
 * never reads past the arena, unlike a bare strlen() on an unterminated
 * buffer). */
static unsigned first_zero(const unsigned char *p, unsigned n)
{
    unsigned i;
    for (i = 0u; i < n; i++) {
        if (p[i] == 0u) {
            return i;
        }
    }
    return n;
}

/* va_list plumbing so vsnprintf can be probed with the same call shape
 * nsftrc_write uses (a variadic wrapper forwarding into vsnprintf). */
static int call_vsnprintf(char *buf, unsigned size, const char *fmt, ...)
{
    va_list ap;
    int     rc;

    va_start(ap, fmt);
    rc = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return rc;
}

/* Run one truncation probe (either vsnprintf via the wrapper or snprintf
 * directly) over a fresh poisoned arena and report/assert what it finds.
 * `label` distinguishes the two call paths in output. */
static void probe_truncation(const char *label, int use_snprintf)
{
    unsigned char arena[ARENA_LEN];
    unsigned char *target = arena + PRE_LEN;
    unsigned char *post   = arena + PRE_LEN + TARGET_LEN;
    unsigned       pre_touched, post_touched, zero_at;
    int            rc;

    poison(arena, ARENA_LEN);
    if (use_snprintf) {
        rc = snprintf((char *)target, TARGET_LEN, "%s", SRC40);
    } else {
        rc = call_vsnprintf((char *)target, TARGET_LEN, "%s", SRC40);
    }

    pre_touched  = count_touched(arena, PRE_LEN);
    post_touched = count_touched(post, POST_LEN);
    zero_at      = first_zero(arena, ARENA_LEN);   /* bounded scan of the WHOLE arena */

    printf("%s: rc=%d pre_touched=%u post_touched=%u zero_at_arena_offset=%u "
           "(target starts at %u, size=%u)\n",
           label, rc, pre_touched, post_touched, zero_at, PRE_LEN, TARGET_LEN);

    CHECK_EQ((long)pre_touched, 0L, "no write before the target pointer");

    /* THE memory-safety question: does libc370 respect `size` as a hard upper
     * bound on how much it writes, even when it fails to leave room for a
     * NUL? If post_touched > 0, libc370 wrote PAST the declared size -- a
     * buffer overflow, not merely a missing terminator. */
    CHECK_EQ((long)post_touched, 0L,
             "size is a hard write bound (no bytes touched past target+size)");

    /* Pinned from TSTTRC's live evidence: within the size-bounded target, the
     * data is written up to the last byte with NO room left for a NUL. */
    CHECK_EQ((long)target[TARGET_LEN - 1u], (long)(unsigned char)SRC40_LAST_TARGET_CHAR,
             "last byte of the target region is DATA, not NUL (libc370 quirk)");

    /* Return value on truncation: the C99 "would-be length" contract. */
    CHECK_EQ((long)rc, (long)(sizeof(SRC40) - 1u),
             "truncation return value equals the untruncated length (C99 rule)");
}

int main(void)
{
    unsigned char arena[ARENA_LEN];
    unsigned char *target;
    int            rc;

    printf("=== nsf370 vsnprintf/snprintf toolchain-divergence probe ===\n");

    probe_truncation("vsnprintf truncation", 0);
    probe_truncation("snprintf  truncation", 1);

    /* -- size == 0: nothing may be written, not even a NUL ----------------- */
    poison(arena, ARENA_LEN);
    target = arena + PRE_LEN;
    rc = call_vsnprintf((char *)target, 0u, "%s", "hello");
    {
        unsigned touched = count_touched(arena, ARENA_LEN);
        printf("size==0: rc=%d bytes touched in the whole arena=%u\n", rc, touched);
        CHECK_EQ((long)touched, 0L, "size==0 writes nothing anywhere in the arena");
        CHECK_EQ((long)rc, 5L, "size==0 still returns the would-be length (C99 rule)");
    }

    /* -- exact fit: no truncation, the well-understood case ---------------- */
    poison(arena, ARENA_LEN);
    target = arena + PRE_LEN;
    rc = call_vsnprintf((char *)target, 6u, "%s", "hello");   /* "hello"+NUL == 6 */
    {
        unsigned post_touched = count_touched(arena + PRE_LEN + 6u, ARENA_LEN - PRE_LEN - 6u);
        printf("exact fit: rc=%d target=\"%s\" target[5]=0x%02X post_touched=%u\n",
               rc, (char *)target, target[5], post_touched);
        CHECK(strcmp((char *)target, "hello") == 0, "exact fit: target holds the full string");
        CHECK_EQ((long)target[5], 0L, "exact fit: NUL-terminated when it fits exactly");
        CHECK_EQ((long)rc, 5L, "exact fit: return value is the string length");
        CHECK_EQ((long)post_touched, 0L, "exact fit: nothing written past the string+NUL");
    }

    return mbt_test_summary("TSTVSNP");
}
