/*
 * nsfxq_host.c -- host implementation of the exit->mainline handoff (spec
 * 4.2). Swapped in for asm/nsfxq.asm on the native test build via the
 * project.toml [host].replace map; never compiled by cc370.
 *
 * Uses GCC/clang atomic builtins (available under -std=gnu99) so the lock-free
 * push/drain contract is exercised for real under make test-host, not merely
 * simulated. On MVS the same contract is met by a CS retry loop (asm/nsfxq.asm).
 */
#include "nsfxq.h"

void xq_init(XQ *xq)
{
    xq->head = NULL;
}

void xq_push(XQ *xq, QELEM *e)
{
    QELEM *old;

    do {
        old = xq->head;
        e->next = old;                  /* prepend: e becomes the new top */
    } while (!__sync_bool_compare_and_swap(&xq->head, old, e));
}

QELEM *xq_drain(XQ *xq)
{
    /* Atomic swap: take the whole chain, leave the stack empty. The returned
     * chain is in LIFO order; the executive reverses it to FIFO. */
    return __atomic_exchange_n(&xq->head, (QELEM *)NULL, __ATOMIC_SEQ_CST);
}
