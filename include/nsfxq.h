#ifndef NSFXQ_H
#define NSFXQ_H
/*
 * nsfxq.h -- the interrupt-safe exit->mainline handoff (spec 4.2).
 *
 * Deliberately separate from QUEUE: a lock-free single-consumer LIFO stack
 * that an asynchronous MVS exit can push onto with nothing but Compare-and-
 * Swap (the only synchronization an exit ever uses besides POST), and that the
 * executive task drains in one atomic swap.
 *
 * Elements are the shared QELEM, linked through ->next only (->prev unused
 * here). xq_drain returns the chain in LIFO order -- most-recently pushed
 * first; the executive reverses it to FIFO before dispatch (NSFEVT, M0-6).
 *
 * ABA safety (why plain CAS is sufficient here): CAS by itself does NOT
 * prevent the ABA problem -- the safety comes from the usage pattern, not the
 * instruction. There is exactly one consumer, and it removes work by swapping
 * out the WHOLE chain at once (xq_drain); individual elements are never popped.
 * A node can therefore never be freed and re-pushed to reappear as a stale-
 * but-equal head underneath an in-flight push. On MVS the exit (producer) and
 * the executive (consumer) additionally run on the uniprocessor, disabled-for-
 * the-exit model, so they are not truly concurrent. WARNING: adding a per-
 * element pop (a CAS that advances head to head->next) would reintroduce the
 * ABA hazard and require a tag/generation counter or hazard pointers -- do not
 * add one; keep removal as the whole-chain swap.
 *
 * Two implementations sit behind this identical interface:
 *   - MVS : asm/nsfxq.asm      -- a CS retry loop (on-target build+validation
 *                                 is deferred to M0-6).
 *   - host: src/nsfxq_host.c   -- GCC/clang atomic builtins, swapped in for the
 *                                 asm by the project.toml [host].replace map so
 *                                 the lock-free contract is exercised for real
 *                                 under make test-host.
 */

#include "nsfque.h"

typedef struct xq {
    QELEM * volatile head;      /* top of stack; NULL when empty */
} XQ;

void    xq_init (XQ *xq);
void    xq_push (XQ *xq, QELEM *e);   /* exit side: prepend e (CS retry loop) */
QELEM  *xq_drain(XQ *xq);             /* executive side: swap out whole chain  */

#endif /* NSFXQ_H */
