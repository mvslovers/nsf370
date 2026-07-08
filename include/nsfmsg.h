#ifndef NSFMSG_H
#define NSFMSG_H
/*
 * nsfmsg.h -- the operator / log message seam (WTO wrapper, spec ch. 05/17).
 *
 * One C-callable choke point for every NSFnnns line NSF writes to the operator:
 * startup/shutdown (NSF0xx), config (NSF7xx), operator replies (NSF8xx) and
 * recovery (NSF9xx). Message IDs are NSF-prefixed (ADR-0007) so they never
 * collide with IBM's EZA/real z/OS message space.
 *
 *   nsfmsg(fmt, ...)   format one line and emit it
 *
 * The FORMATTING is portable (vsnprintf into a fixed line buffer); only the
 * final one-line EMIT is platform-specific and lives behind nsfmsg_emit:
 *   MVS : src/nsfmsg_plat.c       -> libc370 wto() (a real WTO)
 *   host: src/nsfmsg_host.c       -> a capture ring the tests read back + stdout
 * swapped by the project.toml [host].replace map (the NSFXQ / NSFEVT pattern),
 * so the callers (nsfopr, nsfstc, nsfmain) stay portable and host-testable.
 *
 * CHARSET TRANSPARENCY (spec 15.3). nsfmsg compares nothing; it only formats
 * literal message text plus caller values through the platform vsnprintf, so
 * cc370 emits EBCDIC (which wto expects) and the host emits ASCII with no
 * hardcoded byte values -- the same source is correct on both code pages.
 */

#include "nsf.h"
#include <stdarg.h>

/* printf-style format checking where the compiler supports it (cc370 is a GCC
 * fork, so __GNUC__ holds on both host and target). */
#if defined(__GNUC__)
#define NSFMSG_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define NSFMSG_PRINTF(a, b)
#endif

/* Longest line nsfmsg formats. WTO caps a single line at 124 text bytes; a
 * comfortable 128 covers the NSFnnns id + text and matches the trace entry. */
#define NSFMSG_LINE  128

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module nsfmsg_* pins a unique 8-char linker name. Scheme NSFMSG*:
 *   nsfmsg NSFMSG   nsfmsg_emit NSFMSGE
 *   (host-only capture: nsfmsg_cap_reset NSFMSGCR   nsfmsg_cap_count NSFMSGCC
 *    nsfmsg_cap_line NSFMSGCL) */

/* Format one NSFnnns line and emit it (WTO on MVS, captured+printed on host).
 * Never allocates; truncates to NSFMSG_LINE-1 bytes (always NUL-terminated). */
void nsfmsg(const char *fmt, ...) asm("NSFMSG") NSFMSG_PRINTF(1, 2);

/* Platform emit seam: write one finished, NUL-terminated line (no newline).
 * Swapped by [host].replace; never called directly by mainline code. */
void nsfmsg_emit(const char *line) asm("NSFMSGE");

#if NSF_DEBUG
/* Host-test introspection of the lines nsfmsg has emitted (host emit shim only;
 * compiled out of the production module, which has NSF_DEBUG == 0). */
void        nsfmsg_cap_reset(void) asm("NSFMSGCR");   /* clear the capture ring */
UINT        nsfmsg_cap_count(void) asm("NSFMSGCC");   /* lines emitted since reset */
const char *nsfmsg_cap_line(UINT i) asm("NSFMSGCL");  /* i-th line, NULL if >=    */
#endif

#endif /* NSFMSG_H */
