/*
 * tstque.c -- NSFQUE host unit tests (spec ch. 04).
 *
 * Portable C: builds and runs natively via `make test-host` (the CI gate) and,
 * when the on-MVS suite is enabled, as a load module too. Covers FIFO order,
 * q_remove from the middle, the bounded-reject contract, and the Q_ENTRY
 * container-of round trip.
 */
#include "nsfque.h"
#include <mbtcheck.h>

/* A container that embeds a QELEM off both ends, so Q_ENTRY has a non-zero
 * member offset to recover and we can prove the whole object round-trips. */
typedef struct item {
    int   tag;
    QELEM link;
    int   tag2;
} ITEM;

int main(void)
{
    QUEUE  q;
    ITEM   a, b, c;
    QELEM *e;
    ITEM  *ip;
    int    rc;

    printf("=== nsf370 NSFQUE tests ===\n");

    a.tag = 1; a.tag2 = 11;
    b.tag = 2; b.tag2 = 22;
    c.tag = 3; c.tag2 = 33;

    /* ---- FIFO order + Q_ENTRY round trip ---- */
    q_init(&q, 0);
    CHECK(Q_EMPTY(&q), "new queue is empty");
    CHECK_EQ(q_enq(&q, &a.link), 0, "enq a accepted");
    CHECK_EQ(q_enq(&q, &b.link), 0, "enq b accepted");
    CHECK_EQ(q_enq(&q, &c.link), 0, "enq c accepted");
    CHECK_EQ(q.count, 3, "count == 3 after three enqueues");
    CHECK(!Q_EMPTY(&q), "queue not empty");

    e  = q_deq(&q);
    ip = Q_ENTRY(e, ITEM, link);
    CHECK_EQ(ip->tag, 1, "deq #1 is a (FIFO)");
    CHECK_EQ(ip->tag2, 11, "Q_ENTRY recovers the whole container");
    CHECK_EQ(Q_ENTRY(q_deq(&q), ITEM, link)->tag, 2, "deq #2 is b");
    CHECK_EQ(Q_ENTRY(q_deq(&q), ITEM, link)->tag, 3, "deq #3 is c");
    CHECK(q_deq(&q) == NULL, "deq on empty returns NULL");
    CHECK_EQ(q.count, 0, "count back to 0");

    /* ---- q_remove from the middle ---- */
    q_init(&q, 0);
    q_enq(&q, &a.link);
    q_enq(&q, &b.link);
    q_enq(&q, &c.link);
    q_remove(&q, &b.link);              /* pull the middle element */
    CHECK_EQ(q.count, 2, "count == 2 after removing the middle");
    CHECK_EQ(Q_ENTRY(q_deq(&q), ITEM, link)->tag, 1, "remaining order: a first");
    CHECK_EQ(Q_ENTRY(q_deq(&q), ITEM, link)->tag, 3, "remaining order: c second");
    CHECK(Q_EMPTY(&q), "empty after draining");

    /* ---- bounded reject at maxcount ---- */
    q_init(&q, 2);
    CHECK_EQ(q_enq(&q, &a.link), 0, "bounded enq a ok");
    CHECK_EQ(q_enq(&q, &b.link), 0, "bounded enq b ok");
    rc = q_enq(&q, &c.link);
    CHECK(rc != 0, "enq at maxcount is rejected");
    CHECK_EQ(q.count, 2, "count stays at maxcount, queue did not grow");
    /* the queue is still usable: dequeue frees a slot, then enq succeeds */
    (void)q_deq(&q);
    CHECK_EQ(q_enq(&q, &c.link), 0, "enq succeeds again after a dequeue");

    return mbt_test_summary("TSTQUE");
}
