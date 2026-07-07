#ifndef NSFTRC_H
#define NSFTRC_H
/*
 * nsftrc.h -- the Trace Facility (spec ch. 07).
 *
 * A per-subsystem-flagged, in-storage ring of fixed 128-byte entries. The
 * disabled cost is one flag test (the TRC macro's single AND); only when a
 * flag is set does anything format or store. The ring is a file-scope BSS
 * array -- NOT NSFMM storage -- so it works during earliest init before any
 * pool exists and survives into a dump. It carries the eyecatcher "NSFTRACE"
 * so post-mortem analysis of an ABEND can find it without reproducing the run
 * (spec 7.1).
 *
 * Single writer at M0-4: there are no asynchronous MVS exits yet, so the
 * mainline executive task is the only caller and the ring needs no locking.
 * M1 introduces exit-side tracing and must revisit exit-safe entry reservation
 * (a CS-claimed slot index, as NSFXQ does for the handoff) before an exit may
 * call nsftrc_write. See nsftrc.c.
 *
 * Ownership: nsftrc_write and nsftrc_hexdump keep nothing and free nothing;
 * they copy the caller's text into the ring and return.
 */

#include "nsf.h"
#include "nsftime.h"            /* NSFTIME (timestamp embedded in TRCENT) */

/* Per-subsystem trace flags (spec 7.1). A trace entry is stamped with the one
 * flag that produced it. Runtime/PROFILE control of nsftrc_flags arrives at
 * M0-7/M0-8; at M0-4 tests set it through nsftrc_enable / nsftrc_disable. */
#define TRCF_IP      0x0001u
#define TRCF_TCP     0x0002u
#define TRCF_UDP     0x0004u
#define TRCF_ICMP    0x0008u
#define TRCF_DRIVER  0x0010u
#define TRCF_MEMORY  0x0020u
#define TRCF_TIMER   0x0040u
#define TRCF_SOCKET  0x0080u
#define TRCF_API     0x0100u
#define TRCF_CONFIG  0x0200u
#define TRCF_ALL     0x03FFu    /* every defined flag */

/* Ring geometry (spec 7.2): 128-byte entries, 64 KB default ring -> 512 slots.
 * Both are fixed layout constants; a configurable ring size (spec "configurable
 * size, default 64 KB") is an M0-7 concern and would only shrink the count. */
#define NSFTRC_TEXT     112     /* formatted text bytes per entry (with NUL) */
#define NSFTRC_ENTRIES  512     /* 512 * 128 B == 64 KB ring                  */

/* One trace entry: exactly 128 bytes and pointer-free, so its size is identical
 * on host and target and it embeds cleanly in the static ring (spec 7.2). */
typedef struct trcent {
    NSFTIME ts;                 /*  8  timestamp (nsf_now)                    */
    UINT    flag;               /*  4  the TRCF_* that produced this entry    */
    UINT    task;               /*  4  nsf_taskid (numeric, never a pointer)  */
    char    text[NSFTRC_TEXT];  /* 112 truncated formatted text (NUL-term'd)  */
} TRCENT;                       /* 128 bytes */
NSF_SIZE_ASSERT(TRCENT, 128);

/* printf-style format checking where the compiler supports it (cc370 is a GCC
 * fork, so __GNUC__ holds on both host and target). */
#if defined(__GNUC__)
#define NSFTRC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define NSFTRC_PRINTF(a, b)
#endif

/* The trace flag word (spec 7.2). Public because the TRC macro tests it inline;
 * treat it as configuration, set only through nsftrc_enable/disable. */
extern UINT nsftrc_flags;

/* Trace one formatted line under `comp` (IP, TCP, ... -- the suffix of a TRCF_
 * name). One AND when the flag is clear; otherwise formats into the ring. The
 * ", ##__VA_ARGS__" form lets a flag-only call -- TRC(TCP, "listening") -- work
 * with no trailing arguments (a GNU extension, fine under -std=gnu99). */
#define TRC(comp, fmt, ...) \
    do { if (nsftrc_flags & TRCF_##comp) \
             nsftrc_write(TRCF_##comp, (fmt), ##__VA_ARGS__); } while (0)

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module nsftrc_* pins a unique 8-char linker name so cc370's 8-char
 * external truncation (upcased, '_' -> '@') can never fold two into one on MVS.
 * Scheme NSFTR + verb:
 *   nsftrc_init NSFTRINI   nsftrc_enable NSFTRENA   nsftrc_disable NSFTRDIS
 *   nsftrc_write NSFTRWRT   nsftrc_hexdump NSFTRHEX   nsftrc_count NSFTRCNT
 *   nsftrc_peek NSFTRPEK   nsftrc_ring_base NSFTRRNG
 * The trace flag word nsftrc_flags is data, not a function; its mangled name
 * NSFTRC@F is already unique, so it is intentionally left un-aliased. */

/* Zero the ring, stamp the "NSFTRACE" eyecatcher, and clear all flags. Safe to
 * call at earliest init (static storage, no pools required) and to re-call. */
void nsftrc_init(void) asm("NSFTRINI");

/* Set / clear trace flags (an OR / AND-NOT of the mask into nsftrc_flags). */
void nsftrc_enable(UINT flags) asm("NSFTRENA");
void nsftrc_disable(UINT flags) asm("NSFTRDIS");

/* Append one formatted entry to the ring, oldest-overwritten. vsnprintf
 * truncates the text to fit the 112-byte field (always NUL-terminated). Does
 * NOT test nsftrc_flags -- the TRC macro is the gate; a direct caller has
 * already decided to trace. */
void nsftrc_write(UINT flag, const char *fmt, ...) asm("NSFTRWRT") NSFTRC_PRINTF(2, 3);

/* Format a hex + printable-character dump of len bytes at p into ring entries
 * (16 bytes/line), preceded by a `tag (len bytes)` header line. Self-gated on
 * `flag` (there is no macro wrapper), so a disabled dump costs one flag test. */
void nsftrc_hexdump(UINT flag, const char *tag, const void *p, USHORT len) asm("NSFTRHEX");

/* Inspection, reused by the operator DISPLAY dump at M0-8 (and the tests). */
UINT          nsftrc_count(void) asm("NSFTRCNT");      /* live entries, saturates */
const TRCENT *nsftrc_peek(UINT i) asm("NSFTRPEK");     /* i==0 oldest; NULL if >= */
const void   *nsftrc_ring_base(void) asm("NSFTRRNG");  /* dump anchor; 8 B eyecat */

#endif /* NSFTRC_H */
