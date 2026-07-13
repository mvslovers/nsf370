# ADR-0026 — nsffmt: a safe vsnprintf/snprintf seam (libc370 truncation gap, issue #25)

**Status:** Accepted (2026-07-13), from a toolchain-hygiene pass closing out
issue #25 before the M3 preamble.
**Relates to:** CLAUDE.md §3 ("External symbols" / toolchain-quirk rules),
issue #25, `test/mvs/tstvsnp.c`, `test/tstfmt.c`.

## Context

`TSTTRC`'s over-long-text case failed under `test-mvs` (only there — host was
always green): a 255-char source formatted into the 112-byte `TRCENT.text`
field came back with `strlen(text) == 112` (not 111) and `text[111] != 0`. The
codebase already half-knew something was off — `nsfmsg.c` manually
NUL-terminated its formatted line "to be explicit" right after calling
`vsnprintf`, while `nsftrc.c` trusted the call alone. That inconsistency, not
just the underlying libc370 gap, was the actual bug: nothing enforced the
same discipline everywhere, so the next new caller was exactly as likely to
get it wrong as right.

## Investigation (Stage 0 — `test/mvs/tstvsnp.c`, host = false)

Isolated the platform `vsnprintf`/`snprintf` from `NSFTRC` entirely and probed
over a canary arena (poisoned bytes both before and after a `size`-bounded
target region), so "does it also write past `size`" — a memory-safety
question, not just a cosmetic one — gets a bounded, reproducible answer
instead of undefined-behavior-dependent noise from reading past a bare local
array. Pinned live on MVSCE (CC 0 both legs):

- **`size` IS a hard write bound.** Truncation never writes past
  `target + size` — this is *not* a buffer-overflow bug, just a missing
  terminator. (An earlier draft of this probe, without the canary arena, gave
  a false alarm here: reading a bare unterminated buffer's `strlen()` walks
  into whatever adjacent stack memory happens to follow, which is undefined
  behavior, not evidence.)
- On truncation, the target region is filled **solid** through byte
  `size - 1` with data — no byte is reserved for a NUL.
- The return value on truncation is still the **C99 "would-be length"** (the
  untruncated source length) — only the NUL-termination half of the contract
  is broken, not the return-value half.
- `size == 0` writes nothing at all; `size` that exactly fits still
  NUL-terminates correctly. Both match glibc/C99.

A second, unrelated cc370 quirk surfaced while building the probe and is
documented inline in `tstvsnp.c`: indexing a **literal string** with a
compile-time constant index (`"...ABCDEF..."[15]`) constant-folds using the
**host's (ASCII)** byte value, not the target's EBCDIC encoding the same
literal correctly gets when stored and read as a runtime array. A single
**character** literal (`'F'`) does not have this problem — the whole codebase
already relies on cc370 encoding those correctly (CLAUDE.md §3's "use
character literals" rule) — only a literal-string-indexed-by-a-constant
expression does. Worked around by pinning the expected byte as its own
character literal rather than re-deriving it from the source string.

## Decision

1. **One seam, `nsf_vsnprintf`/`nsf_snprintf` (`include/nsffmt.h`,
   `src/nsffmt.c`, aliases `NSFFMVSN`/`NSFFMSNP`), is the only sanctioned way
   to call a truncating formatter in this codebase.** No portable-C source
   calls the platform `vsnprintf`/`snprintf` directly again (CLAUDE.md gains
   this rule).
2. **Contract** (deliberately not the raw C99 return value):
   - **Always** NUL-terminates when `size > 0`: `buf[size-1] = '\0'` after the
     call. Always in-bounds, per the Stage-0 finding that `size` is a hard
     write bound — this can never collide with or extend what the platform
     call already did.
   - Returns the count of characters **actually in the buffer**, excluding
     the NUL: `n < 0 -> 0`; `n >= size -> size - 1`; else `n`. Every existing
     caller that reads the return value wants "how much is really there" (to
     `memcpy` or index it), not a hypothetical unbounded length.
3. Every existing caller converted in the same change: `nsftrc.c`, `nsfmsg.c`
   (dropping its now-redundant manual terminator — the wrapper owns that
   now), `nsfcfg.c`, `nsfstc.c` (three call sites), `nsfsts.c`, `nsfctci.c`
   (two call sites). `src/nsffmt.c` added to every module/test that links a
   converted file.
4. `test/mvs/tstvsnp.c` (`TSTVSNP`, `host = false`) stays permanently — it is
   the reference `nsffmt`'s contract is built against, the same way
   `TSTTHRW` keeps pinning the pre-#21 timed-wait shape. If libc370 is ever
   fixed (or regresses further), this goes green (or redder) first.
   `test/tstfmt.c` (`TSTFMT`, dual host+MVS) exercises the wrapper's own
   clamping logic directly — platform-independent, so it holds regardless of
   which `vsnprintf` runs underneath.

## Consequences

- A future format-buffer bug of this exact shape is structurally impossible
  for a *new* caller: there is no direct platform call left to misuse, and
  `nsffmt.h`'s contract is documented once instead of re-derived (correctly or
  not) at each call site.
- `sts_render`'s `len < 0` guard is now effectively dead code (`nsf_snprintf`
  never returns negative) but stays as cheap, harmless defense — see the
  Stage-0 return-value evidence for why it was never wrong, just unreachable
  given the field widths in play.
- The literal-string-constant-index quirk is not itself fixed (it's a cc370
  front-end behavior, not something NSF can patch) — it is documented as a
  toolchain gotcha for future test/data-table authors: prefer re-deriving an
  expected byte from a **character literal**, never from indexing a **string
  literal** with a compile-time constant.

## Evidence (live, MVSCE, issue #25)

- `TSTVSNP` CC 0 both legs (14/14 assertions each): canary arena shows
  `post_touched == 0` (no overflow past `size`), `target[size-1]` is data,
  return value on truncation `== 40` (the untruncated `SRC40` length).
- Full `test-mvs` regression after the fix: **25 modules, batch+TSO, 950
  PASS, 0 FAIL** — `TSTTRC` (previously 2 failures/leg) now clean, `TSTVSNP`/
  `TSTFMT` both CC 0.
