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

/* Fill *out with the current timestamp. Never fails; cannot allocate or WAIT
 * (it is on trace and, later, timer hot paths). */
void nsf_now(NSFTIME *out);

/* Numeric id of the running task (see the file header). */
UINT nsf_taskid(void);

#endif /* NSFTIME_H */
