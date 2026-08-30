#ifndef NSFTIME_H
#define NSFTIME_H
/*
 * nsftime.h -- platform-primitive seam: a monotonic-ish timestamp and the
 * current task id.
 *
 * Both are single scalar facts the platform answers differently on MVS and on
 * the host, so they share one seam (MVS: asm/nsftime.asm, host:
 * src/nsftime_host.c, swapped by the project.toml [host].replace map -- the
 * same mechanism NSFXQ uses). The name leads on `nsftime` because nsf_now() is
 * the shared clock; nsf_taskid() rides along as the second platform fact rather
 * than earn its own asm file for a single instruction.
 *
 * nsf_now() is NOT trace-private: NSFTMR reuses it at M0-5. NOTE the value's
 * epoch and scale are platform-specific -- MVS returns the 64-bit STCK TOD
 * clock (2**-12 microsecond units since 1900); the host returns a wall-clock
 * reading (gettimeofday: seconds + microseconds). It is suitable for ordering
 * trace entries and for relative timing on one platform; callers must NOT
 * assume a shared tick unit across platforms or derive wall-clock time from it.
 * NSFTMR's tick (100 ms, spec 6.3) is driven by STIMERM, not by this value.
 *
 * nsf_taskid() returns the current task as a NUMERIC id (MVS: the current TCB
 * address as an integer, PSATOLD; host: 0 -- the single mainline task), never
 * a pointer. The trace entry stores it as a UINT so the control block stays
 * pointer-free (spec 7.2). Exit-safe multi-writer tracing (real per-exit task
 * ids) is an M1 concern; see nsftrc.c.
 */
#include "nsf.h"

/* 8-byte, pointer-free timestamp. On MVS it is the 64-bit STCK store (hi/lo
 * halves of the TOD clock); on the host the two halves of a clock reading
 * (hi = seconds, lo = microseconds). Identical size on host and target, so it
 * embeds directly in the 128-byte TRCENT. */
typedef struct nsftime {
    UINT hi;
    UINT lo;
} NSFTIME;
NSF_SIZE_ASSERT(NSFTIME, 8);

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"). These
 * two are the C<->asm seam: the alias here MUST be character-identical to the
 * CSECT label in asm/nsftime.asm (host builds use src/nsftime_host.c, which
 * inherits the same alias transparently). Pinning the name makes the boundary
 * independent of cc370's 8-char '_' -> '@' mangling:
 *   nsf_now NSFNOW   nsf_taskid NSFTASK   nsf_elapsed_ge NSFELAPS
 */

/* Fill *out with the current timestamp. Never fails; cannot allocate or WAIT
 * (it is on trace and, later, timer hot paths). */
void nsf_now(NSFTIME *out) asm("NSFNOW");

/* Numeric id of the running task (see the file header). */
UINT nsf_taskid(void) asm("NSFTASK");

/* "Have at least `secs` seconds elapsed between *since and *now?" -- 1 yes,
 * 0 no.  M5-2c1: the app-registry sweep needs a real-time interval, and this
 * header forbids the obvious shortcut (above): the epoch and the unit of an
 * NSFTIME are platform facts, so the conversion is the platform's to make.
 * MVS gives 2**-12 microsecond STCK units (the hi word steps every 1.048576 s)
 * and the host gives seconds + microseconds; that MVS's hi unit is CLOSE to a
 * second is a coincidence, not a contract, and nothing may lean on it.
 *
 * IT IS PURE ON PURPOSE -- both timestamps are parameters and it reads no
 * clock.  A version that called nsf_now() itself would be untestable at the
 * boundary, because the reading would move between the test building `since`
 * and the function taking `now`.  Callers do the two lines themselves.
 *
 * IT ROUNDS LATE, NEVER EARLY.  The MVS side converts `secs` into whole hi
 * units rounded UP, so the answer turns 1 at or after `secs`, never before --
 * a rate limiter that fires slightly late is a rate limiter; one that fires
 * early is not.  A `since` that lies in the FUTURE (a clock that went
 * backwards, or a caller passing a garbage timestamp) answers 0 for the same
 * reason, and THAT RULE WINS over the next one: a timestamp that cannot be
 * believed is not a measurement, whatever interval was asked for.  Otherwise
 * secs == 0 is 1 -- no interval was asked for.
 *
 * The two platforms therefore differ on ONE input, and it is a resolution
 * limit rather than a disagreement: a `since` that is BACKWARDS BY LESS THAN A
 * SECOND is visible to the host (which keeps microseconds) and invisible to
 * MVS (whose hi word IS the tick), so the host calls it broken while MVS sees
 * no elapsed time at all.  Both answers are inside the "broken timestamp"
 * domain above, and no caller may depend on which one it gets.
 *
 * A ZERO NSFTIME ({0,0}) IS A DELIBERATE "LONG AGO" ON BOTH PLATFORMS -- 1900
 * on MVS, 1970 on the host -- so a caller that leaves its last-run stamp zero
 * gets its first interval immediately rather than after an epoch's wait.  That
 * is the behaviour the sweep's first pass wants, and it is stated here so it
 * reads as a decision rather than as luck.
 *
 * MVS: src/nsftime_plat.c, host: src/nsftime_plat_host.c (the [host].replace
 * pair; it is C on BOTH sides -- this is arithmetic over a value the seam has
 * already produced, and hand-writing it in HLASM would buy nothing and cost
 * the whole column-72 / as370-listing gate). */
int  nsf_elapsed_ge(const NSFTIME *since, const NSFTIME *now,
                    UINT secs) asm("NSFELAPS");

#endif /* NSFTIME_H */
