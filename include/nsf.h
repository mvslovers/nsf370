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
 * Compile-time size guard. Every control block asserts its byte size so a
 * layout change can never silently break the fixed-pool memory budget.
 * Fails to compile (negative array bound) when the size is wrong.
 *
 *   typedef struct { UINT a, b; } MYCB;
 *   NSF_SIZE_ASSERT(MYCB, 8);   // OK; would fail if sizeof(MYCB) != 8
 */
#define NSF_SIZE_ASSERT(type, size) \
    typedef char nsf_assert_##type[(sizeof(type) == (size)) ? 1 : -1]

#endif /* NSF_H */
