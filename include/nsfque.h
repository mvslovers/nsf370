#ifndef NSFQUE_H
#define NSFQUE_H
/*
 * nsfque.h -- the one intrusive queue used across NSF (spec ch. 04).
 *
 * A circular doubly-linked list threaded through a sentinel head, giving O(1)
 * enqueue (at the tail), dequeue (from the head, so FIFO) and remove (from
 * anywhere). Bounded by default: q_enq rejects at `maxcount` rather than
 * growing -- memory determinism, the same discipline everywhere (socket rx
 * queues, listen backlogs, device queues).
 *
 * The queue never allocates. QELEMs are always embedded in pooled objects
 * (PBUF, TMR, EVT, NSFRQE); the owner supplies the storage.
 */

#include "nsf.h"
#include <stddef.h>             /* offsetof */

typedef struct qelem {
    struct qelem *next;
    struct qelem *prev;
} QELEM;                        /* 8 bytes on the S/370 target */
NSF_SIZE_ASSERT(QELEM, 8);

typedef struct queue {
    QELEM  head;                /* sentinel; the list is circular through it */
    USHORT count;               /* current element count                     */
    USHORT maxcount;            /* 0 = unbounded (avoid); else reject-at      */
} QUEUE;

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module q_* pins a unique 8-char linker name so cc370's 8-char external
 * truncation (upcased, '_' -> '@') can never fold two into one on MVS. Scheme
 * NSFQ + verb:  q_init NSFQINIT   q_enq NSFQENQ   q_deq NSFQDEQ
 *   q_remove NSFQREMV
 */
void    q_init  (QUEUE *q, USHORT maxcount) asm("NSFQINIT");
int     q_enq   (QUEUE *q, QELEM *e) asm("NSFQENQ");   /* 0 = ok; !=0 = full   */
QELEM  *q_deq   (QUEUE *q) asm("NSFQDEQ");             /* NULL when empty      */
void    q_remove(QUEUE *q, QELEM *e) asm("NSFQREMV");  /* unlink e (in q now)  */

#define Q_EMPTY(q)  ((q)->count == 0)

/* container-of: recover the enclosing object from an embedded QELEM. The
 * void* hop keeps -Wcast-align quiet about the char* -> type* cast. */
#define Q_ENTRY(e, type, member) \
    ((type *)(void *)((char *)(e) - offsetof(type, member)))

#endif /* NSFQUE_H */
