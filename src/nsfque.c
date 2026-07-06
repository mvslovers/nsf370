/*
 * nsfque.c -- intrusive FIFO queue (see nsfque.h, spec ch. 04).
 *
 * Sentinel-node representation: an empty queue has head.next == head.prev ==
 * &head. Enqueue links before the sentinel (the tail); dequeue takes the node
 * after the sentinel (the head) -- hence FIFO. All operations are O(1) and
 * allocate nothing.
 */
#include "nsfque.h"

void q_init(QUEUE *q, USHORT maxcount)
{
    q->head.next = &q->head;
    q->head.prev = &q->head;
    q->count = 0;
    q->maxcount = maxcount;
}

int q_enq(QUEUE *q, QELEM *e)
{
    QELEM *tail;

    if (q->maxcount != 0 && q->count >= q->maxcount) {
        return 1;                       /* bounded: reject, never grow */
    }
    tail = q->head.prev;                /* insert e between tail and sentinel */
    e->next = &q->head;
    e->prev = tail;
    tail->next = e;
    q->head.prev = e;
    q->count++;
    return 0;
}

QELEM *q_deq(QUEUE *q)
{
    QELEM *e;

    if (q->count == 0) {
        return NULL;
    }
    e = q->head.next;                   /* the front element */
    q_remove(q, e);
    return e;
}

void q_remove(QUEUE *q, QELEM *e)
{
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->next = NULL;                     /* poison: catch use-after-remove */
    e->prev = NULL;
    q->count--;
}
