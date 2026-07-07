#ifndef NSFABEND_H
#define NSFABEND_H
/*
 * nsfabend.h -- the single, deliberate "this must never happen" exit.
 *
 * Every enforced invariant in NSF funnels through nsf_abend() rather than
 * ABENDing ad hoc: mm_pool_create after initialization is sealed, a corrupted
 * or doubly-freed pool object, and (from M0-8) any executive-task failure the
 * ESTAE cannot recover. One choke point means one place to wire the real
 * ESTAE/ABEND path later, and one place for tests to observe.
 *
 * Contract: nsf_abend() does NOT return to its caller. Callers still place an
 * explicit `return` after it -- defence in depth, so a test hook that
 * mistakenly returns cannot fall through into the very corruption the abend
 * was guarding against.
 *
 * Host builds install a hook (nsf_abend_sethook) so a unit test can assert an
 * invariant fired: the hook longjmp()s out, so nsf_abend still never returns.
 * With no hook installed, nsf_abend prints a diagnostic and abort()s.
 */

#include "nsf.h"

typedef void (*nsf_abend_fn)(UINT code);

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): without
 * them cc370's 8-char external truncation (upcased, '_' -> '@') folds BOTH
 * nsf_abend and nsf_abend_sethook to the single symbol NSF@ABEN, so ld370 binds
 * every nsf_abend call to whichever one it kept -- a silent mis-dispatch host
 * tests cannot see. Scheme NSFA + verb:
 *   nsf_abend NSFABEND   nsf_abend_sethook NSFAHOOK
 */

/* Raise a user completion `code` (NSF message range of the failing component,
 * e.g. 100-199 for memory). Does not return. */
void nsf_abend(UINT code) asm("NSFABEND");

/* Install (or clear, with NULL) the abend hook; returns the previous hook.
 * Test-facing: production code never sets a hook. */
nsf_abend_fn nsf_abend_sethook(nsf_abend_fn hook) asm("NSFAHOOK");

#endif /* NSFABEND_H */
