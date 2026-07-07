#ifndef NSFSTS_H
#define NSFSTS_H
/*
 * nsfsts.h -- the Statistics Manager (spec ch. 08).
 *
 * A registry of named 32-bit counters, one registered per component per metric
 * at init. Counters are plain increments on the executive task -- no atomicity
 * (spec 8.1); the few exit-owned counters live in exit cache lines and are
 * sampled, not shared, which is an M1 concern.
 *
 * The registry is a file-scope BSS array -- NOT NSFMM storage -- so counters
 * exist before any pool does and survive into a dump (it carries the eyecatcher
 * "NSFSTATS"). A registered STSCTR* is stable for the life of the run: the
 * array never moves and registration only appends, so a component may cache the
 * pointer once and STS_INC it forever.
 *
 * Registration is init-window only: sts_register draws from the fixed table and
 * returns NULL once it is full (a build-time miscount, reported by the caller,
 * exactly as mm_pool_create returns NULL past its pool max). It never abends.
 */

#include "nsf.h"

/* One counter, exactly 16 bytes and pointer-free (spec 8.2). The owning
 * component is kept OUT of this struct (in the registry record, see nsfsts.c)
 * so STSCTR stays 16 B; `name` is the metric, e.g. "in", "badcksum". */
typedef struct stsctr {
    char name[12];              /* 12  metric name, NUL-padded            */
    UINT value;                 /*  4  the 32-bit counter                 */
} STSCTR;                       /* 16 bytes */
NSF_SIZE_ASSERT(STSCTR, 16);

/* Fixed registry capacity. Spec 8.1 counts a few metrics per component across
 * ~8 components; 128 slots is comfortable headroom with a tiny fixed cost. */
#define NSFSTS_MAX  128

/* Hot-path counter updates -- plain increments, no function call (spec 8.2). */
#define STS_INC(c)      ((c)->value++)
#define STS_ADD(c, n)   ((c)->value += (UINT)(n))

/* Zero the registry, stamp the "NSFSTATS" eyecatcher. Safe at earliest init
 * (static storage, no pools) and safe to re-call. */
void    sts_init(void);

/* Register a counter for `component` named `name`, returning a stable STSCTR*
 * (value 0). Init-window only. Returns NULL when the registry is full -- the
 * caller reports it; this is never an abend. Both strings are truncated to
 * their fields (component to 8, name to 12). */
STSCTR *sts_register(const char *component, const char *name);

/* Zero every registered counter's value; registrations are kept (spec 8.1:
 * "reset via operator command"). */
void    sts_reset(void);

/* Number of counters currently registered. */
UINT    sts_count(void);

/* Render "component name value" lines (one per registered counter, in
 * registration order) into buf, NUL-terminating when bufsize > 0. Stops at the
 * last whole line that fits -- never a partial line. Returns the number of
 * bytes written (excluding the terminating NUL). The M0-8 operator DISPLAY
 * STATS hookup calls this; here a plain buffer keeps it trivially testable. */
UINT    sts_render(char *buf, UINT bufsize);

#endif /* NSFSTS_H */
