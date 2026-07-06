#ifndef NSF_H
#define NSF_H
/*
 * nsf370 -- Network Services Facility for MVS 3.8j
 * Project-wide base definitions.
 *
 * At M0-1 this header carries only the primitives the build skeleton needs.
 * Component interfaces (NSFMM, NSFBUF, NSFQUE, ...) arrive from M0-2 onward,
 * one header per component in include/. See docs/CLAUDE.md and the
 * Architecture Specification for the full picture.
 */

#define NSF_VERSION "0.1.0-dev"

/*
 * Fixed-width base types.
 *   S/370 (cc370): char = 8, short = 16, int = 32 bits.
 *   Host test builds (ILP32/LP64): identical widths for these types.
 * Finalized against libc370's headers in M0-2 (guarded there if it already
 * provides them).
 */
typedef unsigned char   UCHAR;   /*  8 bits */
typedef unsigned short  USHORT;  /* 16 bits */
typedef unsigned int    UINT;    /* 32 bits */
typedef int             INT;     /* 32 bits, signed */

/*
 * Compile-time size guard. Every pooled control block asserts its on-target
 * byte size so a layout change can never silently break the fixed-pool memory
 * budget (spec 1.5). The declared sizes are the S/370 target sizes: char=8,
 * short=16, int=32 bits, and 4-byte (24-bit) pointers.
 *
 * The value check is therefore only meaningful under cc370, which predefines
 * __MVS__. A native host test build uses 8-byte pointers, so any control block
 * containing a pointer has a different sizeof there and asserting the target
 * size would fail to compile. Host binaries are never exchanged with MVS, so
 * the target budget is irrelevant on the host: the macro stays callable
 * everywhere but drops the value check off-target. (Same reasoning rexx370
 * uses to guard its ENVBLOCK size lock behind __MVS__.)
 *
 *   typedef struct { UINT a, b; } MYCB;
 *   NSF_SIZE_ASSERT(MYCB, 8);   // enforced on MVS; no-op placeholder on host
 */
#ifdef __MVS__
#define NSF_SIZE_ASSERT(type, size) \
    typedef char nsf_assert_##type[(sizeof(type) == (size)) ? 1 : -1]
#else
#define NSF_SIZE_ASSERT(type, size) \
    typedef char nsf_assert_##type[1]
#endif

#endif /* NSF_H */
