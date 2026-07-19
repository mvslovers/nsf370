/*
 * nsfsel.c -- NSFSEL: the SELECT multiplex engine (see nsfsel.h, spec ch. 10.3,
 * ADR-0035).
 *
 * Executive-side, single-task machinery (spec 3, run-to-completion, no locking):
 * a fixed static pool of parked-SELECT control blocks (SELCB), each with an
 * embedded timeout TMR. A SELECT that finds a socket already ready completes at
 * once; otherwise it parks here and is completed by (a) a readiness poke re-eval
 * (soc_notify_ready -> nsfsel_on_notify), (b) its timeout TMR firing, or (c) the
 * teardown of one of its sockets (SEL_DEAD poke -> NSF_ECONNABORTED). Every exit
 * cancels the TMR and frees the slot, so no SELECT hangs and the pool returns to
 * baseline (the leak gate).
 *
 * REACHED ONLY THROUGH REGISTRATION (nsfsel_init): the RQ_SELECT handler
 * (nsfsel_dispatch) and the re-eval callback (nsfsel_on_notify) are STATIC, wired
 * into NSFREQ / NSFSOC by function pointer -- so a build without this TU leaves
 * RQ_SELECT at NSF_EOPNOTSUPP and every readiness poke a no-op (nsfsel.h).
 *
 * OWNERSHIP. A parked SELECT's NSFRQE is app-owned (same-space Phase 1); the engine
 * only reads its ubuf item array and writes each item's `ready`, then POSTs the
 * app's ecb (soc_complete) -- after which the app owns it again. The NSFSELITEM
 * array lives in the app's storage (the caller's stack in nsfeza); it stays valid
 * while the app is parked in WAIT on the request's ecb (its frame is preserved).
 */
#include "nsfsel.h"
#include "nsfsoc.h"             /* SOCKCB, PROTOPS.poll, sock_lookup, soc_*, SEL_* */
#include "nsfreq.h"             /* NSFRQE, RQ_*, NSF_E*, nsfreq_register_select    */
#include "nsftmr.h"             /* TMR, tmr_start / tmr_cancel                      */
#include "nsfque.h"             /* Q_EMPTY (generic readiness fallback)            */
#include <string.h>

/* ---- parked-SELECT control block + pool ------------------------------------ */

typedef struct selcb {
    UCHAR       busy;           /* slot occupied                                */
    UCHAR       timed;          /* a finite-timeout TMR is armed                */
    NSFRQE     *req;            /* the parked SELECT request (app-owned)        */
    NSFSELITEM *items;          /* the app-side descriptor/interest array (ubuf)*/
    UINT        nitems;         /* item count (r->ulen)                         */
    TMR         tmr;            /* timeout (embedded; tmr_cancel mandatory)     */
} SELCB;

static SELCB g_selcb[NSFSEL_MAX];

/* ---- slot alloc / free ----------------------------------------------------- */

static SELCB *sel_alloc(void)
{
    UINT i;

    for (i = 0u; i < NSFSEL_MAX; i++) {
        if (!g_selcb[i].busy) {
            memset(&g_selcb[i], 0, sizeof(g_selcb[i]));
            g_selcb[i].busy = 1u;
            return &g_selcb[i];
        }
    }
    return NULL;                            /* > NSFSEL_MAX parked -> EMFILE     */
}

static void sel_free(SELCB *cb)
{
    tmr_cancel(&cb->tmr);                   /* idempotent + mandatory (spec 6.4) */
    cb->busy   = 0u;
    cb->timed  = 0u;
    cb->req    = NULL;
    cb->items  = NULL;
    cb->nitems = 0u;
}

/* ---- readiness scan (side-effect-free) ------------------------------------- */

/* Scan an item set against current socket readiness, writing each item's `ready`
 * subset; return the count of items with any ready bit. A closed/stale descriptor
 * (sock_lookup NULL) contributes nothing (a socket torn down WHILE parked takes the
 * SEL_DEAD path instead). Uses the protocol poll op, or the generic fallback
 * (read = rxq/acceptq non-empty, write = always) for a NULL poll (UDP / dummies). */
static UINT sel_scan(NSFSELITEM *items, UINT n)
{
    UINT i, count = 0u;

    for (i = 0u; i < n; i++) {
        SOCKCB *s   = sock_lookup(items[i].desc);
        int     want = (int)items[i].want;
        int     rdy  = 0;

        if (s != NULL) {
            if (s->ops != NULL && s->ops->poll != NULL) {
                rdy = s->ops->poll(s, want);
            } else {
                if ((want & (int)SEL_READ) != 0 &&
                    (!Q_EMPTY(&s->rxq) || !Q_EMPTY(&s->acceptq))) {
                    rdy |= (int)SEL_READ;
                }
                if ((want & (int)SEL_WRITE) != 0) {
                    rdy |= (int)SEL_WRITE;      /* generic: always writable      */
                }
            }
            rdy &= want;                        /* never report an unwanted bit  */
        }
        items[i].ready = (UCHAR)rdy;
        if (rdy != 0) {
            count++;
        }
    }
    return count;
}

static int sel_has_desc(const SELCB *cb, UINT desc)
{
    UINT i;

    for (i = 0u; i < cb->nitems; i++) {
        if (cb->items[i].desc == desc) {
            return 1;
        }
    }
    return 0;
}

/* ---- timeout ticks --------------------------------------------------------- */

/* Convert a {sec, usec} timeout to logical ticks (100 ms), rounding up so a
 * sub-tick timeout still waits at least one tick. The 0/0 poll case is handled by
 * the dispatcher before this is reached, so the clamp to >= 1 is for a tiny but
 * non-zero timeout only. */
static UINT sel_ticks(UINT sec, UINT usec)
{
    UINT ticks = sec * 10u + (usec + 99999u) / 100000u;

    return (ticks == 0u) ? 1u : ticks;
}

/* ---- completion (one exit path) -------------------------------------------- */

/* Complete a parked SELECT: free the slot (cancels the TMR), then POST the app.
 * Order matters -- capture the request before sel_free clears cb->req. */
static void sel_finish(SELCB *cb, INT retcode, INT errno_)
{
    NSFRQE *r = cb->req;

    sel_free(cb);
    soc_complete(r, retcode, errno_);
}

/* ---- the timeout TMR callback ---------------------------------------------- */

static void sel_timeout(void *arg)
{
    SELCB *cb = (SELCB *)arg;
    UINT   i;

    /* Timeout: RETCODE 0 with every mask zeroed (no socket is ready). The TMR is
     * already detached + IDLE (nsftmr_run does that before the callback), so
     * sel_free's tmr_cancel is a clean no-op. */
    for (i = 0u; i < cb->nitems; i++) {
        cb->items[i].ready = 0u;
    }
    sel_finish(cb, 0, 0);
}

/* ---- the RQ_SELECT handler (registered with NSFREQ) ------------------------ */

static void nsfsel_dispatch(NSFRQE *r)
{
    NSFSELITEM *items = (NSFSELITEM *)r->ubuf;
    UINT        n     = (items != NULL) ? r->ulen : 0u;
    UINT        count;
    SELCB      *cb;

    r->sockdesc = 0u;                          /* not parked on any pend_ slot   */

    count = sel_scan(items, n);
    if (count > 0u) {
        soc_complete(r, (INT)count, 0);        /* something already ready        */
        return;
    }
    /* Nothing ready. Poll form (a finite 0/0 timeout) returns immediately. */
    if ((r->p3 & SEL_F_TIMED) != 0u && r->p1 == 0u && r->p2 == 0u) {
        soc_complete(r, 0, 0);
        return;
    }
    /* Park it (bounded). */
    cb = sel_alloc();
    if (cb == NULL) {
        soc_complete(r, NSF_RETERR, NSF_EMFILE);   /* > NSFSEL_MAX parked        */
        return;
    }
    cb->req    = r;
    cb->items  = items;
    cb->nitems = n;
    if ((r->p3 & SEL_F_TIMED) != 0u) {
        cb->timed = 1u;
        tmr_start(&cb->tmr, sel_ticks(r->p1, r->p2), sel_timeout, cb);
    }
    /* else: block forever (no TMR) until a readiness poke or a teardown. */
}

/* ---- the readiness re-eval callback (registered with NSFSOC) --------------- */

static void nsfsel_on_notify(SOCKCB *s, UCHAR events)
{
    UINT i;

    if ((events & (UCHAR)SEL_DEAD) != 0u) {
        /* A socket in a parked SELECT's set is being destroyed: complete that
         * SELECT with NSF_ECONNABORTED (the parked-request teardown convention,
         * §10.5) rather than let it hang. soc_desc(s) still resolves -- the poke
         * runs BEFORE soc_destroy bumps the generation (ADR-0035). */
        UINT desc = soc_desc(s);

        for (i = 0u; i < NSFSEL_MAX; i++) {
            SELCB *cb = &g_selcb[i];
            if (cb->busy && sel_has_desc(cb, desc)) {
                sel_finish(cb, NSF_RETERR, NSF_ECONNABORTED);
            }
        }
        return;
    }

    /* Readiness change: re-scan every parked SELECT (bounded, cheap) and complete
     * any that now has a ready socket. Re-scanning ALL of them (not only ones
     * referencing s) keeps the coupling trivially correct; a scan reflects the
     * CURRENT socket state, which resolves the SELECT-vs-concurrent-RECV case. */
    (void)s;
    for (i = 0u; i < NSFSEL_MAX; i++) {
        SELCB *cb = &g_selcb[i];
        if (cb->busy) {
            UINT count = sel_scan(cb->items, cb->nitems);
            if (count > 0u) {
                sel_finish(cb, (INT)count, 0);
            }
        }
    }
}

/* ---- init / introspection -------------------------------------------------- */

void nsfsel_init(void)
{
    UINT i;

    for (i = 0u; i < NSFSEL_MAX; i++) {
        tmr_cancel(&g_selcb[i].tmr);           /* disarm any residue on re-init  */
        memset(&g_selcb[i], 0, sizeof(g_selcb[i]));
    }
    soc_set_select_notify(nsfsel_on_notify);
    nsfreq_register_select(nsfsel_dispatch);
}

#if NSF_DEBUG
UINT nsfsel_debug_inuse(void)
{
    UINT i, n = 0u;

    for (i = 0u; i < NSFSEL_MAX; i++) {
        if (g_selcb[i].busy) {
            n++;
        }
    }
    return n;
}
#endif
