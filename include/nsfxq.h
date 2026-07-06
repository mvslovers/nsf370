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
