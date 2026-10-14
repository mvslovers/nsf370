/*----------------------------------------------------------------------------
 * nsfsct.h -- the d2 rendezvous: a named SSCT carrying the anchor address.
 *
 * ADR-0048, ROUND 1.  Register / find / remove, and NOTHING ELSE.  Adoption --
 * what a start does with a retained anchor once it has found one -- is round 2,
 * and no declaration here may anticipate it.
 *
 * WHY AN SSCT AT ALL.  A retained anchor is unreachable today:
 * `nsfsx_anchor_alloc` getmains unconditionally and never looks for one, which
 * is why 40-chk's 139264 bytes is a PER-RECYCLE cost rather than a one-time
 * one.  The SSCT is the only 3.8j mechanism that is NAMED, system-wide, and
 * outlives the address space that made it: ENQ is purged at task and memory
 * termination (IEAVENQ1.asm), CVTUSER is a single word, and name/token services
 * do not exist.  See docs/measurements/d2-rendezvous/.
 *
 * THE SPLIT, AND WHY IT IS WHERE IT IS.  `nsfsct_split` and `nsfsct_classify`
 * are PURE -- no MVS call, no CSA, no layout dependency -- so the truth table
 * is pinned by a host test instead of by a live run that would have to hit
 * every row.  Everything that touches the SSI chain or the TIOT lives in
 * `src/nsfsct_plat.c` and is swapped out on the host ([host].replace), the
 * `nsftime_plat.c` pattern.
 *
 * The pure half deliberately does NOT include nsfvsvc.h: that header is the
 * MVS-only CSA contract and does not build on the host.  It takes the two
 * values it needs -- the eyecatcher and the version -- as PARAMETERS, extracted
 * by the caller.  Same reasoning nsfreqx.h records for the same reason.
 *
 * 8-char aliases: NSFSCSPL NSFSCCLS NSFSCVDS NSFSCREG NSFSCFND NSFSCRMV
 *                 NSFSCJOB NSFSCPFN NSFSCPRG NSFSCPRM
 *--------------------------------------------------------------------------*/
#ifndef NSFSCT_H
#define NSFSCT_H

#include "nsf.h"

/* The SSCT name is CL4 and a jobname is 8 characters. */
#define NSFSCT_KEYLEN    4
#define NSFSCT_TAILLEN   4
#define NSFSCT_JOBLEN    8

/* Mirrored from nsfvsvc.h, which the pure half cannot include (above).  They
 * are cross-checked against the real definitions in the MVS half, where both
 * headers ARE available -- a mirror nobody compares is a mirror that drifts. */
#define NSFSCT_EYE       "NSFVANCR"
#define NSFSCT_EYELEN    8
#define NSFSCT_VER       3U

/*--------------------------------------------------------------------------
 * VERDICTS.  What `find` may return -- and every one of these is a statement
 * about what was FOUND, never about what to do with it.  Acting on a verdict
 * is round 2 (ADR-0048 7).
 *------------------------------------------------------------------------*/
#define NSFSCT_NONE      0  /* no SSCT under our name -- the ordinary start   */
#define NSFSCT_OK        1  /* found; eyecatcher, version and identity agree  */
#define NSFSCT_BADEYE    2  /* SSCT found; SSCTSUSE null or eyecatcher wrong  */
#define NSFSCT_BADVER    3  /* eye ok, version differs                        */
#define NSFSCT_FOREIGN   4  /* eye+version ok, jobname tail differs           */

/*--------------------------------------------------------------------------
 * PURE.  Split an 8-character jobname into the SSCT key (chars 0..3, FOLDED TO
 * LOWER CASE) and the tail stored in the anchor (chars 4..7, as they are).
 * Neither output is NUL-terminated -- both are fixed-width fields, compared
 * with memcmp, exactly as MVS stores them.  A short jobname arrives
 * blank-padded from the TIOT and stays that way; this function never trims,
 * because trimming would make "NSF " and "NSF" two spellings of one name.
 *
 * WHY LOWER CASE (ADR-0048 6, decided after round 1).  Round 1 found that a
 * chained SSCT whose name equals a PROCEDURE name makes `S <name>` fail
 * IEF612I PROCEDURE NOT FOUND: MVS resolves the operand as a subsystem first.
 * A key equal to jobname[0:4] is exactly that whenever the jobname is four
 * characters or fewer, which is the production case (NSFS).  A procedure
 * name is upper case, and so is START's operand, so a key with at least one
 * lower-case letter can never equal one.  Folding keeps all four characters,
 * so the key and the tail still reconstruct the jobname EXACTLY (upper case is
 * the inverse) -- nothing is lost and the anchor layout does not move.
 *
 * RETURNS 1 when the key differs from jobname[0:4] (some character was a
 * letter), 0 when folding changed nothing -- a key of digits, blanks and
 * national characters only, which IS a possible procedure name.  The caller
 * must not register such a key; nsfsct_register refuses it.
 *------------------------------------------------------------------------*/
int nsfsct_split(const char *jobname, char *key, char *tail) asm("NSFSCSPL");

/*--------------------------------------------------------------------------
 * PURE.  Classify what an SSCT pointed at.  `eye` and `version` are the
 * anchor's, extracted by the caller; `stored_tail` is the anchor's
 * jobname_tail; `our_tail` is this instance's.  A NULL `eye` means SSCTSUSE
 * was null -- an SSCT with no anchor behind it.
 *
 * THE ORDER IS THE POINT, and it is checked-then-trusted:
 *   1. eyecatcher  -- is this plausibly one of ours at all?
 *   2. version     -- do we understand the layout?
 *   3. identity    -- and is it OURS, or a CL4 truncation collision?
 * Step 3 reads a field whose OFFSET step 2 has just vouched for.  Reversing
 * them would compare a byte range whose position is not yet established.
 *------------------------------------------------------------------------*/
int nsfsct_classify(const char *eye, UINT version,
                    const char *stored_tail, const char *our_tail)
                                                              asm("NSFSCCLS");

/* PURE.  A verdict's name, for messages and for the record.  Never a decision. */
const char *nsfsct_verdict_name(int verdict) asm("NSFSCVDS");

/*--------------------------------------------------------------------------
 * THE ROUND-1 API.  All three derive the name from the jobname themselves, so
 * no caller can pass a name that drifted from the one registration used -- the
 * drift failure ADR-0048 6 rejects a configured name over.
 *------------------------------------------------------------------------*/

/* Register this instance's SSCT with `anchor` in SSCTSUSE.  REFUSES, loudly and
 * without chaining anything, if an SSCT already exists under our name: a
 * second chain entry is the failure, not the fix (ADR-0048 2).  Returns 0 on
 * success, NSFSCT_RC_NOTSAFE if the derived key could equal a procedure name
 * (see nsfsct_split), -1 on any other refusal or failure. */
#define NSFSCT_RC_NOTSAFE  (-2)
int nsfsct_register(void *anchor) asm("NSFSCREG");

/* Find our SSCT and classify what it points at.  Returns a verdict; `*out`
 * receives the anchor address for every verdict in which one was reachable
 * (OK, BADVER, FOREIGN) and NULL otherwise.
 *
 * A CALLER MAY NOT TREAT `*out` AS ADOPTABLE.  Only NSFSCT_OK says the layout
 * is understood AND the anchor is this instance's; what to DO about that is
 * round 2. */
int nsfsct_find(void **out) asm("NSFSCFND");

/* Remove our SSCT by name.  Works on ANY verdict, including BADEYE and BADVER:
 * removal goes through ssct_remove_by_name, which never dereferences SSCTSUSE,
 * so a corrupt anchor -- or one pointing at freed storage -- does not prevent
 * it.  That asymmetry is why a version-mismatched anchor is REMOVABLE BUT NOT
 * ADOPTABLE (ADR-0048 3.2).  Returns 0 if one was removed, -1 otherwise. */
int nsfsct_remove(void) asm("NSFSCRMV");

/*--------------------------------------------------------------------------
 * PLATFORM SEAM.  src/nsfsct_plat.c on MVS, src/nsfsct_plat_host.c on the host.
 *------------------------------------------------------------------------*/
int   nsfsct_plat_jobname(char *out8)                         asm("NSFSCJOB");
void *nsfsct_plat_find(const char *key)                       asm("NSFSCPFN");
int   nsfsct_plat_register(const char *key, void *anchor)     asm("NSFSCPRG");
int   nsfsct_plat_remove(const char *key)                     asm("NSFSCPRM");

#endif /* NSFSCT_H */
