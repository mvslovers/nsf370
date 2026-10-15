/*----------------------------------------------------------------------------
 * nsfsct.h -- the d2 rendezvous: a named SSCT carrying the anchor address.
 *
 * ADR-0048.  Round 1: register / find / remove.  Round 2 (#136) adds ONE pure
 * decision, nsfsct_start_action -- what a start does with the verdict.  The
 * adoption itself (re-stamp, scrub, router) is src/nsfsx.c's, because every
 * step of it is a store into the anchor.
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
 *                 NSFSCJOB NSFSCPFN NSFSCPRG NSFSCPRM NSFSCSTA NSFSCPST
 *                 NSFSCADR
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
#define NSFSCT_VER       4U

/*--------------------------------------------------------------------------
 * VERDICTS.  What `find` may return -- and every one of these is a statement
 * about what was FOUND, never about what to do with it.  Acting on a verdict
 * is nsfsct_start_action's (round 2).
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

/*--------------------------------------------------------------------------
 * PURE.  May an SSCTSUSE value be DEREFERENCED as an anchor of `len` bytes?
 *
 * Round 2 made every start call nsfsct_find, and find reads through SSCTSUSE.
 * Whatever is chained under our name is read in OUR address space, so a value
 * that is not common storage names some address in our own private area --
 * the round-1b HOLD fixture left exactly that (SSCTSUSE=00098464, a private
 * address of a job that has ended), and so would anything a test or another
 * product chains under the name.  Reading it is at best a stranger's bytes and
 * at worst an S0C4 in NSFS's start.
 *
 * An anchor is SP=241 CSA, which on MVS 3.8j lies ABOVE the private area.  So
 * the address must be non-zero, fullword aligned, at or above the private
 * area's end (`pa_start` + `pa_size`, GDA PASTRT/PASIZE), and the whole anchor
 * must end at or below 16 MB.  An unknown private area (`pa_size` 0) answers
 * NO: a check that cannot be made never licenses a read.
 *
 * WHAT IT DOES NOT PROVE: that the storage is allocated or ours.  Common
 * storage above the private area is SQA/CSA/LPA, and the eyecatcher and
 * version that classify reads next are what decide "ours".
 *------------------------------------------------------------------------*/
int nsfsct_addr_plausible(UINT addr, UINT len, UINT pa_start, UINT pa_size)
                                                              asm("NSFSCADR");

/* PURE.  A verdict's name, for messages and for the record.  Never a decision. */
const char *nsfsct_verdict_name(int verdict) asm("NSFSCVDS");

/*--------------------------------------------------------------------------
 * PURE.  The POSTURE of an anchor whose verdict was OK -- an OK verdict says
 * the anchor is ours, not that nobody is using it.  Checked in this order:
 *
 *   DAMAGED   nslots differs from ours under the same version
 *   SERVED    ACTIVE is set, or a wake-ECB pointer is published: someone is
 *             serving it (both retain paths clear both)
 *   ALIVE     the retain posture, but the server that retained it has not
 *             ended (nsfreqx_server_gone): the previous instance still inside
 *             its drain, or a second one under the same jobname
 *   RETAINED  the retain posture, and its server has ended -- adoptable
 *------------------------------------------------------------------------*/
#define NSFSCT_POSTURE_RETAINED  0
#define NSFSCT_POSTURE_DAMAGED   1
#define NSFSCT_POSTURE_SERVED    2
#define NSFSCT_POSTURE_ALIVE     3
int nsfsct_posture(int nslots_ok, int active, int ecb_published,
                   int server_gone) asm("NSFSCPST");

/*--------------------------------------------------------------------------
 * PURE.  What a START does with the verdict (round 2, decided 2026-10-15).
 *
 *   NONE                      FRESH    allocate, register
 *   OK, RETAINED              ADOPT    reuse the anchor, keep the SSCT
 *   OK, DAMAGED               REPLACE  (below)
 *   OK, SERVED or ALIVE       ASIDE    allocate, touch nothing, do not register
 *   BADEYE, BADVER            REPLACE  remove the SSCT by name, leave the old
 *                                      storage where it is, allocate, register
 *   FOREIGN, anything else    ASIDE
 *
 * OK + SERVED/ALIVE is ASIDE because adopting an anchor its owner may yet free
 * is a use-after-free in common storage, and removing the SSCT would strand
 * whatever that owner later retains (`P NSFS` then `S NSFS` while the old
 * instance is still in its 10 s drain).  OK + DAMAGED is REPLACE, not ASIDE:
 * no owner will ever come back for a damaged anchor, and leaving its SSCT
 * chained would send every later start ASIDE -- the rendezvous lost until IPL.
 *
 * REPLACE never frees the old storage: BADEYE says SSCTSUSE does not lead to
 * anything we can read as ours, and BADVER that we cannot read its layout --
 * in neither case is a free something this instance can justify (ADR-0048
 * 3.2: "removable but not adoptable").
 *------------------------------------------------------------------------*/
#define NSFSCT_START_FRESH    0
#define NSFSCT_START_ADOPT    1
#define NSFSCT_START_REPLACE  2
#define NSFSCT_START_ASIDE    3
int nsfsct_start_action(int verdict, int posture) asm("NSFSCSTA");

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
