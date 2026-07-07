/*
 * tsttrc.c -- NSFTRC host unit tests (spec ch. 07).
 *
 * White-box tests of the trace facility: they read the ring back through the
 * nsftrc_peek / nsftrc_count / nsftrc_ring_base inspection seam (the same seam
 * the operator DISPLAY dump reuses at M0-8). Portable C: builds and runs
 * natively via `make test-host` (the CI gate) with NSF_DEBUG=1. The nsf_now /
 * nsf_taskid seam is provided here by src/nsftime_host.c (asm/nsftime.asm
 * swapped out by the project.toml [host].replace map).
 *
 * Covers: TRCENT is 128 bytes on the host (the value guard NSF_SIZE_ASSERT
 * cannot make on a host build); a cleared flag suppresses the write THROUGH THE
 * TRC MACRO (nothing recorded); a set flag records exactly one entry with the
 * right flag and text; over-long text is truncated into the 112-byte field and
 * stays NUL-terminated; the ring wraps oldest-first after > 512 writes with the
 * cursor/count correct (peek(0) is the SECOND write, peek(count-1) the last);
 * hexdump formats a known buffer into a header + data line; the eyecatcher is
 * present.
 */
#include "nsftrc.h"
#include <mbtcheck.h>
#include <string.h>

int main(void)
{
    const TRCENT *e;
    UINT          base;
    int           i;
    char          longtext[256];

    printf("=== nsf370 NSFTRC tests ===\n");

    /* ---- pointer-free layout: TRCENT must be 128 B here too (spec 7.2) ---- *
     * NSF_SIZE_ASSERT only value-checks under __MVS__ (host pointers differ),
     * but TRCENT is pointer-free, so its size is identical and we assert it. */
    CHECK_EQ((long)sizeof(TRCENT), 128, "TRCENT is 128 bytes on the host");

    /* ---- init stamps the dump eyecatcher and clears everything ---- */
    nsftrc_init();
    CHECK(memcmp(nsftrc_ring_base(), "NSFTRACE", 8) == 0,
          "ring carries the NSFTRACE eyecatcher");
    CHECK_EQ((long)nsftrc_count(), 0, "init leaves an empty ring");
    CHECK_EQ((long)nsftrc_flags, 0, "init clears all trace flags");

    /* ---- a cleared flag suppresses the write, through the TRC macro ---- */
    base = nsftrc_count();
    TRC(TCP, "this must not be recorded");     /* TRCF_TCP not enabled */
    CHECK_EQ((long)nsftrc_count(), (long)base,
             "cleared flag: TRC records nothing");

    /* ---- a set flag records exactly one entry ---- */
    nsftrc_enable(TRCF_TCP);
    CHECK((nsftrc_flags & TRCF_TCP) != 0, "nsftrc_enable sets the flag");
    TRC(TCP, "connect %d.%d.%d.%d", 10, 0, 0, 1);
    CHECK_EQ((long)nsftrc_count(), (long)base + 1,
             "set flag: TRC records one entry");
    e = nsftrc_peek(nsftrc_count() - 1);
    CHECK(e != NULL, "newest entry is peekable");
    CHECK_EQ((long)e->flag, (long)TRCF_TCP, "entry stamped with TRCF_TCP");
    CHECK(strcmp(e->text, "connect 10.0.0.1") == 0,
          "entry holds the formatted text");

    /* ---- nsftrc_disable clears the flag again ---- */
    nsftrc_disable(TRCF_TCP);
    base = nsftrc_count();
    TRC(TCP, "suppressed again");
    CHECK_EQ((long)nsftrc_count(), (long)base,
             "nsftrc_disable re-suppresses TRC");

    /* ---- over-long text is truncated into the 112-byte field ---- */
    nsftrc_init();
    nsftrc_enable(TRCF_IP);
    for (i = 0; i < (int)sizeof(longtext) - 1; i++) {
        longtext[i] = 'A';
    }
    longtext[sizeof(longtext) - 1] = '\0';         /* 255 'A's */
    nsftrc_write(TRCF_IP, "%s", longtext);
    e = nsftrc_peek(nsftrc_count() - 1);
    CHECK(e != NULL, "truncated entry is peekable");
    CHECK_EQ((long)strlen(e->text), NSFTRC_TEXT - 1,
             "over-long text truncated to 111 chars");
    CHECK_EQ((long)e->text[NSFTRC_TEXT - 1], 0,
             "truncated text stays NUL-terminated");
    CHECK(e->text[0] == 'A', "truncated text keeps its leading bytes");

    /* ---- the ring wraps oldest-first after > ENTRIES writes ---- *
     * Write ENTRIES+1 distinctly-tagged entries (seq 0 .. ENTRIES). The first
     * (seq 0) is overwritten, so the oldest survivor is seq 1 and the newest is
     * seq ENTRIES -- the discriminating check a bare count==ENTRIES misses. */
    nsftrc_init();
    nsftrc_enable(TRCF_IP);
    for (i = 0; i <= NSFTRC_ENTRIES; i++) {        /* 0 .. ENTRIES inclusive */
        nsftrc_write(TRCF_IP, "seq %d", i);
    }
    CHECK_EQ((long)nsftrc_count(), NSFTRC_ENTRIES,
             "count saturates at the ring capacity");
    e = nsftrc_peek(0);
    CHECK(e != NULL && strcmp(e->text, "seq 1") == 0,
          "peek(0) is the 2nd write: the oldest was overwritten");
    e = nsftrc_peek(NSFTRC_ENTRIES - 1);
    {
        char want[16];
        sprintf(want, "seq %d", NSFTRC_ENTRIES);
        CHECK(e != NULL && strcmp(e->text, want) == 0,
              "peek(count-1) is the newest write");
    }
    CHECK(nsftrc_peek(NSFTRC_ENTRIES) == NULL, "peek past count returns NULL");

    /* ---- hexdump formats a known buffer into header + data lines ---- */
    nsftrc_init();
    nsftrc_enable(TRCF_DRIVER);
    {
        static const UCHAR pkt[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

        nsftrc_hexdump(TRCF_DRIVER, "PKT", pkt, (USHORT)sizeof(pkt));
        CHECK_EQ((long)nsftrc_count(), 2,
                 "hexdump of 4 bytes writes a header + one data line");
        e = nsftrc_peek(0);
        CHECK(e != NULL && strstr(e->text, "PKT") != NULL
              && strstr(e->text, "4 bytes") != NULL,
              "hexdump header line names the tag and length");
        e = nsftrc_peek(1);
        CHECK(e != NULL && strstr(e->text, "0000: DE AD BE EF") != NULL,
              "hexdump data line renders the bytes as hex");
    }

    /* ---- a cleared flag suppresses hexdump too (self-gated) ---- */
    nsftrc_init();
    {
        static const UCHAR pkt[4] = { 1, 2, 3, 4 };

        nsftrc_hexdump(TRCF_DRIVER, "OFF", pkt, (USHORT)sizeof(pkt));
        CHECK_EQ((long)nsftrc_count(), 0,
                 "hexdump self-gates: nothing when the flag is clear");
    }

    return mbt_test_summary("TSTTRC");
}
