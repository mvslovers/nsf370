#ifndef NSFFMT_H
#define NSFFMT_H
/*
 * nsffmt.h -- the safe formatting seam (issue #25.2).
 *
 * libc370's vsnprintf/snprintf do NOT NUL-terminate on truncation -- a real
 * glibc/C99 (7.19.6.5) conformance gap, pinned live on MVSCE by TSTVSNP
 * (test/mvs/tstvsnp.c): `size` IS respected as a hard write bound (no buffer
 * overflow -- not a memory-safety bug), but on truncation the target is
 * filled SOLID through byte size-1 with data, leaving no byte for a
 * terminator. The return value is unaffected -- it is still the C99
 * "would-be length" (the untruncated source length) even when the NUL is
 * missing.
 *
 * nsf_vsnprintf / nsf_snprintf are the ONLY sanctioned way to call a
 * printf-family truncating formatter in this codebase (CLAUDE.md): they
 * always NUL-terminate when size > 0, no exceptions, so a caller never has to
 * remember to do it manually (that inconsistency -- nsfmsg.c doing it,
 * nsftrc.c not -- was the actual bug behind issue #25.2, not just the
 * libc370 gap itself).
 *
 * CONTRACT (deliberately NOT the raw C99 return value):
 *   - ALWAYS NUL-terminates when size > 0: buf[size-1] = '\0' after the call.
 *     (Always in-bounds: TSTVSNP proved `size` is a hard write bound, so
 *     writing to buf[size-1] ourselves can never collide with what the
 *     platform call did or clobber anything beyond the caller's buffer.)
 *   - Returns the number of characters ACTUALLY present in buf, excluding the
 *     NUL: n < 0 -> 0; n >= size -> size - 1 (truncated: exactly what fits
 *     before the forced terminator); otherwise n. This is a CLAMPED count, not
 *     the raw "would-be" length C99 defines -- every caller in this codebase
 *     that reads the return value wants "how many bytes are actually in the
 *     buffer" (e.g. to know how much to memcpy), not a hypothetical.
 */

#include "nsf.h"
#include <stdarg.h>

/* printf-style format checking where the compiler supports it (cc370 is a GCC
 * fork, so __GNUC__ holds on both host and target) -- mirrors NSFTRC_PRINTF
 * in nsftrc.h. Argument 0 for the va_list form: gcc checks the format string
 * itself but has no "..." list to cross-check against. */
#if defined(__GNUC__)
#define NSFFMT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define NSFFMT_PRINTF(a, b)
#endif

/* asm() external-symbol aliases (CLAUDE.md §3). Scheme NSFFM + verb:
 *   nsf_vsnprintf NSFFMVSN   nsf_snprintf NSFFMSNP
 */

/* va_list form -- used where the caller already has an ap (e.g. a variadic
 * wrapper like nsftrc_write / nsfmsg forwarding its own "..."). */
int nsf_vsnprintf(char *buf, unsigned size, const char *fmt, va_list ap)
    asm("NSFFMVSN") NSFFMT_PRINTF(3, 0);

/* Variadic form -- the direct snprintf replacement. */
int nsf_snprintf(char *buf, unsigned size, const char *fmt, ...)
    asm("NSFFMSNP") NSFFMT_PRINTF(3, 4);

#endif /* NSFFMT_H */
