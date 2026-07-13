/*
 * nsftrc.c -- the Trace Facility (see nsftrc.h, spec ch. 07).
 *
 * The ring is one file-scope object -- a header (eyecatcher, geometry, cursor)
 * followed by the fixed TRCENT array. It is BSS, never NSFMM storage (goal 2
 * does not apply: trace must run before pools exist and must survive into a
 * dump). All ring arithmetic uses the compile-time constant NSFTRC_ENTRIES, so
 * a write before nsftrc_init cannot divide by zero; it merely lands in a ring
 * that has not yet been stamped.
 *
 * Cursor invariant (the one place ring buffers hide off-by-ones):
 *   next  = physical index of the NEXT write, in [0, NSFTRC_ENTRIES)
 *   count = live entries = min(total writes, NSFTRC_ENTRIES)
 *   oldest physical index = (count < NSFTRC_ENTRIES) ? 0 : next
 *   peek(i) = ent[(oldest + i) % NSFTRC_ENTRIES]   for i in [0, count)
 * Once full, `next` is simultaneously the slot about to be overwritten and the
 * oldest live entry -- hence oldest == next in the full case.
 *
 * Single writer (M0-4): the mainline task is the only caller; no locking. When
 * M1 adds exit-side tracing, an exit must reserve its slot with a CS on `next`
 * (as NSFXQ claims its stack top) before this code can be called off-mainline.
 */
#include "nsftrc.h"
#include "nsffmt.h"           /* nsf_vsnprintf (libc370 doesn't NUL-terminate
                                * on truncation, issue #25.2)                */
#include "nsftime.h"

#include <stdarg.h>            /* va_list                                   */
#include <string.h>          /* memset, memcpy */
#include <ctype.h>          /* isprint (EBCDIC-aware on the target) */

/* Nibble -> hex digit. The literal is compiled to the platform's encoding, so
 * the digits are EBCDIC-correct under cc370 -- no hardcoded character codes. */
static const char nsftrc_hexdig[] = "0123456789ABCDEF";

/* The whole ring: a small header then the entry array. The header is first so
 * a dump that finds the "NSFTRACE" eyecatcher immediately has the geometry and
 * cursor needed to walk the entries. */
static struct nsftrc_ring {
    char   eye[8];              /* "NSFTRACE" (not NUL-terminated)            */
    UINT   entries;             /* NSFTRC_ENTRIES (copy for the dump reader)  */
    UINT   next;                /* physical index of the next write           */
    UINT   count;              /* live entries, saturating at entries         */
    UINT   rsvd;               /* pad the header to 24 bytes                  */
    TRCENT ent[NSFTRC_ENTRIES];
} g_ring;

UINT nsftrc_flags;             /* all flags clear until nsftrc_enable */

void nsftrc_init(void)
{
    memset(&g_ring, 0, sizeof(g_ring));
    memcpy(g_ring.eye, "NSFTRACE", sizeof(g_ring.eye));
    g_ring.entries = NSFTRC_ENTRIES;
    g_ring.next    = 0;
    g_ring.count   = 0;
    nsftrc_flags   = 0;
}

void nsftrc_enable(UINT flags)
{
    nsftrc_flags |= flags;
}

void nsftrc_disable(UINT flags)
{
    nsftrc_flags &= ~flags;
}

void nsftrc_write(UINT flag, const char *fmt, ...)
{
    TRCENT  *e = &g_ring.ent[g_ring.next];
    va_list  ap;

    nsf_now(&e->ts);
    e->flag = flag;
    e->task = nsf_taskid();

    va_start(ap, fmt);
    (void)nsf_vsnprintf(e->text, sizeof(e->text), fmt, ap);  /* truncates to fit */
    va_end(ap);

    /* Advance the cursor and grow the live count until the ring is full. */
    g_ring.next = (g_ring.next + 1u) % NSFTRC_ENTRIES;
    if (g_ring.count < NSFTRC_ENTRIES) {
        g_ring.count++;
    }
}

void nsftrc_hexdump(UINT flag, const char *tag, const void *p, USHORT len)
{
    const UCHAR *b = (const UCHAR *)p;
    UINT         off;

    if ((nsftrc_flags & flag) == 0) {
        return;                         /* self-gated: no TRC wrapper for dumps */
    }

    nsftrc_write(flag, "%s (%u bytes)",
                 (tag != NULL) ? tag : "", (unsigned)len);

    /* One entry per 16-byte line: "OOOO: XX XX .. XX  gutter". Widths are
     * fixed so the worst case (6 + 16*3 + 1 + 16 = 71 chars + NUL) fits the
     * 112-byte text field with room to spare. `off` is UINT so stepping by 16
     * cannot wrap a USHORT near 64 KB. */
    for (off = 0; off < len; off += 16) {
        char line[NSFTRC_TEXT];
        int  c = 0;
        int  n = (int)((USHORT)(len - off));    /* bytes left on this line */
        int  j;

        if (n > 16) {
            n = 16;
        }

        line[c++] = nsftrc_hexdig[(off >> 12) & 0xF];
        line[c++] = nsftrc_hexdig[(off >> 8) & 0xF];
        line[c++] = nsftrc_hexdig[(off >> 4) & 0xF];
        line[c++] = nsftrc_hexdig[off & 0xF];
        line[c++] = ':';
        line[c++] = ' ';

        for (j = 0; j < 16; j++) {
            if (j < n) {
                UCHAR v = b[off + (UINT)j];
                line[c++] = nsftrc_hexdig[(v >> 4) & 0xF];
                line[c++] = nsftrc_hexdig[v & 0xF];
            } else {
                line[c++] = ' ';
                line[c++] = ' ';
            }
            line[c++] = ' ';
        }
        line[c++] = ' ';

        for (j = 0; j < n; j++) {
            UCHAR v = b[off + (UINT)j];
            line[c++] = isprint(v) ? (char)v : '.';
        }
        line[c] = '\0';

        nsftrc_write(flag, "%s", line);
    }
}

UINT nsftrc_count(void)
{
    return g_ring.count;
}

const TRCENT *nsftrc_peek(UINT i)
{
    UINT oldest;

    if (i >= g_ring.count) {
        return NULL;
    }
    oldest = (g_ring.count < NSFTRC_ENTRIES) ? 0u : g_ring.next;
    return &g_ring.ent[(oldest + i) % NSFTRC_ENTRIES];
}

const void *nsftrc_ring_base(void)
{
    return &g_ring;
}
