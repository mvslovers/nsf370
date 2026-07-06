/*
 * tstxq.c -- NSFXQ exit->mainline handoff host unit tests (spec 4.2).
 *
 * Single-threaded: exercises the push/drain ordering contract, not concurrency
 * (the lock-free property is a property of the CS/atomic implementation). On
 * the host the xq_* entries come from src/nsfxq_host.c, swapped in for
 * asm/nsfxq.asm by the project.toml [host].replace map.
 */
#include "nsfxq.h"
#include <mbtcheck.h>

typedef struct node { int id; QELEM link; } NODE;

/* The executive task performs this LIFO->FIFO reversal on drained chains
 * (NSFEVT, M0-6). Reproduced here to check the full drain contract. */
static QELEM *reverse(QELEM *head)
{
    QELEM *prev = NULL;
    QELEM *cur  = head;

    while (cur != NULL) {
        QELEM *nxt = cur->next;
        cur->next = prev;
        prev = cur;
        cur = nxt;
    }
    return prev;
}

int main(void)
{
    XQ     xq;
    NODE   a, b, c;
    QELEM *chain, *e;

    printf("=== nsf370 NSFXQ handoff tests ===\n");
    a.id = 1; b.id = 2; c.id = 3;

    xq_init(&xq);
    CHECK(xq_drain(&xq) == NULL, "drain of an empty xq is NULL");

    /* exit side pushes a, then b, then c */
    xq_push(&xq, &a.link);
    xq_push(&xq, &b.link);
    xq_push(&xq, &c.link);

    /* executive drains: whole chain in one swap, LIFO order (c, b, a) */
    chain = xq_drain(&xq);
    CHECK(chain != NULL, "drain returns the pushed chain");
    e = chain;
    CHECK_EQ(Q_ENTRY(e, NODE, link)->id, 3, "drain #1 = c (LIFO: last in, first out)");
    e = e->next;
    CHECK_EQ(Q_ENTRY(e, NODE, link)->id, 2, "drain #2 = b");
    e = e->next;
    CHECK_EQ(Q_ENTRY(e, NODE, link)->id, 1, "drain #3 = a");
    CHECK(e->next == NULL, "chain terminates after three elements");

    CHECK(xq_drain(&xq) == NULL, "second drain is empty (the swap took it all)");

    /* the executive's reversal restores the original push (FIFO) order */
    xq_push(&xq, &a.link);
    xq_push(&xq, &b.link);
    xq_push(&xq, &c.link);
    chain = reverse(xq_drain(&xq));
    CHECK_EQ(Q_ENTRY(chain, NODE, link)->id, 1, "reversed #1 = a (FIFO)");
    CHECK_EQ(Q_ENTRY(chain->next, NODE, link)->id, 2, "reversed #2 = b");
    CHECK_EQ(Q_ENTRY(chain->next->next, NODE, link)->id, 3, "reversed #3 = c");

    return mbt_test_summary("TSTXQ");
}
