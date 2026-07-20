/*
 * nsfsoc.c -- the protocol-independent Socket Layer (see nsfsoc.h, spec ch. 10).
 *
 * Executive-side, single-task machinery: the socket table, the SOCKCB object, the
 * (gen<<16)|id descriptor with its stale-fd guard, the PROTOPS dispatch, the
 * parked-request pattern, and the ONE teardown checklist (soc_destroy). No
 * protocol lives here (UDP is M3-3, TCP is M4) and no request TRANSPORT lives here
 * (NSFREQ is M3-2): this is the socket object model those layers build on.
 *
 * GENERATION IS PER-SLOT (spec 10 refinement, changelog v1.24). The socket table
 * is an array of {SOCKCB* sock; USHORT gen} slots. `gen` is the STABLE identity of
 * a slot and survives the SOCKCB's free/realloc; SOCKCB.gen is only a mirror
 * stamped at alloc. This is what makes §10.5's "bump gen, mm_free" meaningful:
 * bumping a field inside the block you are about to free would do nothing, and a
 * slot reused by a later mm_alloc would otherwise hand back an old descriptor's
 * generation. With the slot owning `gen`, a descriptor for a closed OR reused slot
 * fails sock_lookup with EBADF -- never the wrong socket.
 */
#include "nsfsoc.h"
#include "nsfmm.h"
#include "nsfbuf.h"             /* PBUF, buf_free (rxq/acceptq flush)        */
#include "nsfthr.h"             /* nsfthr_post -- real same-AS POST of ecb   */
#include "nsfevtp.h"            /* NSFECB (the app completion ecb word)      */
#include "nsftrc.h"
#include <string.h>

/* ---- socket table + SOCKET pool -------------------------------------------- */

/* One table slot. `gen` is authoritative and persists across sock/free (see the
 * file header); `sock` is the live SOCKCB or NULL when the slot is free. */
typedef struct sockslot {
    SOCKCB *sock;
    USHORT  gen;
} SOCKSLOT;

static MMPOOL  *g_sockpool;                     /* SOCKET pool (SOCKCB objects) */
static SOCKSLOT g_socktab[NSFSOC_MAX_DEFAULT];
static UINT     g_sockmax = NSFSOC_MAX_DEFAULT; /* active table size            */

/* SELECT readiness re-eval callback (NSFSEL registers it; NULL when SELECT is not
 * linked -> every poke is a no-op). See the seam comment in nsfsoc.h. */
static SOCSELFN g_select_notify;

/* ---- statistics (spec 8, message range 600-699) ---------------------------- */
static STSCTR *soc_opens, *soc_closes, *soc_nofd;
static int     soc_stats_ready;

static void socc(STSCTR *c)
{
    if (c != NULL) {
        STS_INC(c);
    }
}

static void soc_stats_init(void)
{
    if (soc_stats_ready) {
        return;
    }
    soc_opens  = sts_register("NSFSOC", "opens");
    soc_closes = sts_register("NSFSOC", "closes");
    soc_nofd   = sts_register("NSFSOC", "nofd");    /* table/pool full (EMFILE)  */
    soc_stats_ready = 1;
}

/* ---- init ------------------------------------------------------------------ */

int soc_reserve(UINT count)
{
    if (count == 0u || count > NSFSOC_MAX_DEFAULT) {
        count = NSFSOC_MAX_DEFAULT;
    }
    g_sockmax  = count;
    /* Init-window only: mm_pool_create is legal solely between mm_init and
     * mm_init_complete (it ABENDs afterwards -- nsfmm.h). No operator message
     * here; the M3-2 startup path reports a reserve failure (refuse-to-start). */
    g_sockpool = mm_pool_create("SOCKET  ", (USHORT)sizeof(SOCKCB), (USHORT)count);
    return (g_sockpool == NULL) ? 1 : 0;
}

void soc_init(void)
{
    UINT i;

    soc_stats_init();
    if (g_sockmax == 0u) {
        g_sockmax = NSFSOC_MAX_DEFAULT;
    }
    /* Reseed every slot: free, generation 1 (so a slot-0 descriptor is never 0,
     * and descriptor 0 is always invalid). Call before any socket is created. */
    for (i = 0u; i < NSFSOC_MAX_DEFAULT; i++) {
        g_socktab[i].sock = NULL;
        g_socktab[i].gen  = 1u;
    }
}

/* ---- descriptor + table lookup --------------------------------------------- */

UINT soc_desc(const SOCKCB *s)
{
    return ((UINT)s->gen << 16) | (UINT)s->id;
}

SOCKCB *sock_lookup(UINT desc)
{
    UINT    id  = desc & 0xFFFFu;
    UINT    gen = (desc >> 16) & 0xFFFFu;
    SOCKCB *s;

    if (id >= g_sockmax) {
        return NULL;
    }
    s = g_socktab[id].sock;
    if (s == NULL) {
        return NULL;                            /* closed slot                  */
    }
    if ((UINT)g_socktab[id].gen != gen) {
        return NULL;                            /* stale / reused -> EBADF      */
    }
    return s;
}

/* ---- allocation ------------------------------------------------------------ */

SOCKCB *sock_alloc(void)
{
    SOCKCB *s;
    UINT    i;

    if (g_sockpool == NULL) {
        socc(soc_nofd);
        return NULL;
    }
    for (i = 0u; i < g_sockmax; i++) {
        if (g_socktab[i].sock == NULL) {
            break;
        }
    }
    if (i >= g_sockmax) {
        socc(soc_nofd);                         /* table full -> EMFILE         */
        return NULL;
    }
    s = (SOCKCB *)mm_alloc(g_sockpool);
    if (s == NULL) {
        socc(soc_nofd);                         /* pool exhausted -> EMFILE     */
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->id    = (USHORT)i;
    s->gen   = g_socktab[i].gen;                /* mirror the slot generation   */
    s->state = SOC_ST_OPEN;
    q_init(&s->rxq, NSFSOC_RXQ_MAX);
    q_init(&s->acceptq, NSFSOC_ACCEPTQ_MAX);
    g_socktab[i].sock = s;
    socc(soc_opens);
    return s;
}

SOCKCB *soc_create(UCHAR domain, UCHAR type, UCHAR proto, PROTOPS *ops)
{
    SOCKCB *s = sock_alloc();

    if (s == NULL) {
        return NULL;
    }
    s->domain = domain;
    s->type   = type;
    s->proto  = proto;
    s->ops    = ops;
    if (ops != NULL && ops->attach != NULL) {
        if (ops->attach(s) != 0) {
            soc_destroy(s);                     /* never leave a half-built sock */
            return NULL;
        }
    }
    TRC(SOCKET, "create fd=%08X dom=%u type=%u proto=%u",
        (unsigned)soc_desc(s), (unsigned)domain, (unsigned)type, (unsigned)proto);
    return s;
}

/* ---- request dispatch ------------------------------------------------------ */

int soc_dispatch(SOCKCB *s, NSFRQE *r)
{
    if (s == NULL || r == NULL) {
        return NSF_EINVAL;
    }
    switch (r->fn) {
    case RQ_BIND:
        if (s->ops == NULL || s->ops->bind == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->bind(s);
    case RQ_CONNECT:
        if (s->ops == NULL || s->ops->connect == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->connect(s, r);
    case RQ_LISTEN:
        if (s->ops == NULL || s->ops->listen == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->listen(s, (int)r->p1);
    case RQ_ACCEPT:
        if (s->ops == NULL || s->ops->accept == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->accept(s, r);
    case RQ_SEND:
    case RQ_SENDTO:
        if (s->ops == NULL || s->ops->send == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->send(s, r);
    case RQ_RECV:
    case RQ_RECVFROM:
        if (s->ops == NULL || s->ops->recv == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->recv(s, r);
    case RQ_CLOSE:
        if (s->ops == NULL || s->ops->close == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->close(s, r);
    case RQ_SHUTDOWN:
        if (s->ops == NULL || s->ops->shutdown == NULL) {
            return NSF_EOPNOTSUPP;
        }
        return s->ops->shutdown(s, r);          /* the op completes r (M4-5)    */
    default:
        return NSF_EINVAL;                      /* unknown / not-yet-wired fn   */
    }
}

/* ---- park / complete (spec 10.3) ------------------------------------------- */

int soc_park(SOCKCB *s, NSFRQE *r, UINT which)
{
    NSFRQE **slot;

    if (s == NULL || r == NULL) {
        return NSF_EINVAL;
    }
    switch (which) {
    case SOC_PEND_RECV:    slot = &s->pend_recv;    break;
    case SOC_PEND_ACCEPT:  slot = &s->pend_accept;  break;
    case SOC_PEND_CONNECT: slot = &s->pend_connect; break;
    case SOC_PEND_SEND:    slot = &s->pend_send;    break;
    default:               return NSF_EINVAL;
    }
    if (*slot != NULL) {
        return NSF_EINVAL;                      /* one outstanding op per kind  */
    }
    *slot = r;                                  /* socket now owns r until done  */
    return 0;
}

void soc_complete(NSFRQE *r, INT retcode, INT errno_)
{
    SOCKCB *s;

    if (r == NULL) {
        return;
    }
    r->retcode = retcode;
    r->errno_  = errno_;

    /* Clear the pend slot on r's socket, if r was parked (the immediate /
     * non-blocking path never parked). This resolves r's socket via its
     * descriptor, which is still valid here -- inside soc_destroy this runs
     * BEFORE the generation bump, so the lookup succeeds. */
    s = sock_lookup(r->sockdesc);
    if (s != NULL) {
        if (s->pend_recv == r) {
            s->pend_recv = NULL;
        }
        if (s->pend_accept == r) {
            s->pend_accept = NULL;
        }
        if (s->pend_connect == r) {
            s->pend_connect = NULL;
        }
        if (s->pend_send == r) {
            s->pend_send = NULL;
        }
    }

    /* Same-address-space plain POST of the app's completion ecb (Phase 1). Use
     * the thread seam (nsfthr_post = a real SVC 2 POST on MVS), NOT the evt-loop
     * self-wake seam (nsfevt_plat_post only sets the bit on MVS -- correct for
     * the loop, whose liveness the STIMER heartbeat guarantees, but it would
     * never wake an app task WAITing solely on this ecb). One call site so M3-2
     * can swap the seam. Cross-AS POST is M5/Phase 2 (ADR-0022), not this. */
    nsfthr_post((NSFECB *)&r->ecb, 0u);
}

/* ---- SELECT readiness poke seam (spec 10.3 / ADR-0035) --------------------- */

void soc_set_select_notify(SOCSELFN fn)
{
    g_select_notify = fn;
}

void soc_notify_ready(SOCKCB *s, UCHAR events)
{
    if (g_select_notify != NULL && s != NULL) {
        g_select_notify(s, events);
    }
}

/* ---- teardown: the ONE checklist (spec 10.5) ------------------------------- */

void soc_destroy(SOCKCB *s)
{
    QELEM *e;
    UINT   id;

    if (s == NULL) {
        return;
    }

    /* 0. Poke the SELECT engine BEFORE any teardown, while soc_desc(s) still
     *    resolves (the generation is bumped in step 6): a parked SELECT whose set
     *    includes this socket completes with NSF_ECONNABORTED rather than hanging
     *    once the socket vanishes. Forget-proof -- one line in the ONE checklist,
     *    so no close/reset/shutdown path can skip it (ADR-0035). No-op when SELECT
     *    is not linked. */
    soc_notify_ready(s, (UCHAR)SEL_DEAD);

    s->state = SOC_ST_CLOSING;

    /* 1. Detach the pcb -- the protocol cancels its timers and frees its own pcb
     *    storage (final resource release). NULL-safe on a half-built socket. */
    if (s->ops != NULL && s->ops->detach != NULL) {
        (void)s->ops->detach(s);
    }

    /* 2. Flush the receive queue (single owner: we free every PBUF). */
    while ((e = q_deq(&s->rxq)) != NULL) {
        buf_free(Q_ENTRY(e, PBUF, q));
    }

    /* 3. Drain the accept queue. It is PROTOCOL-OWNED (M4-2 fix): TCP fills it
     *    with established, un-ACCEPTed CHILD sockets linked through TCB.acceptlink
     *    -- NOT PBUFs. The protocol detach in step 1 (tcp_detach -> tcp_destroy of
     *    a listener) tears every child down through soc_destroy and empties this
     *    queue, so it is normally already empty here. Drain any residue by
     *    UNLINKING ONLY -- never buf_free, which on a TCB acceptlink would use the
     *    wrong container base and corrupt the heap (the M3-1 placeholder freed it
     *    as a PBUF -- a latent landmine now removed, spec 10.2). UDP never uses
     *    acceptq, so this is a no-op there. */
    while (q_deq(&s->acceptq) != NULL) {
        /* unlink only; each child was owned + destroyed by the protocol detach */
    }

    /* 4. COMPLETE every parked NSFRQE with an error, so no app task WAITs
     *    forever -- the parked-request analog of an uncancelled timer. Must run
     *    BEFORE the generation bump (soc_complete resolves r's socket to clear
     *    its slot). */
    if (s->pend_recv != NULL) {
        soc_complete(s->pend_recv, NSF_RETERR, NSF_ECONNABORTED);
    }
    if (s->pend_accept != NULL) {
        soc_complete(s->pend_accept, NSF_RETERR, NSF_ECONNABORTED);
    }
    if (s->pend_connect != NULL) {
        soc_complete(s->pend_connect, NSF_RETERR, NSF_ECONNABORTED);
    }
    if (s->pend_send != NULL) {
        soc_complete(s->pend_send, NSF_RETERR, NSF_ECONNABORTED);
    }

    /* 5. Release the pcb reference (detach freed the storage). */
    s->pcb = NULL;
    s->ops = NULL;

    /* 6. Bump the SLOT generation (persists across the free) and vacate the
     *    slot: an old descriptor for this slot now fails sock_lookup, and a later
     *    alloc into it yields a different descriptor. Never wrap to 0. */
    id = s->id;
    g_socktab[id].gen = (USHORT)(g_socktab[id].gen + 1u);
    if (g_socktab[id].gen == 0u) {
        g_socktab[id].gen = 1u;
    }
    g_socktab[id].sock = NULL;

    /* 7. Free the SOCKCB. */
    TRC(SOCKET, "destroy fd=%08X", (unsigned)(((UINT)s->gen << 16) | (UINT)id));
    s->state = SOC_ST_FREE;
    mm_free(g_sockpool, s);
    socc(soc_closes);
}

/* ---- introspection --------------------------------------------------------- */

UINT soc_count(void)
{
    UINT i, n = 0u;

    for (i = 0u; i < g_sockmax; i++) {
        if (g_socktab[i].sock != NULL) {
            n++;
        }
    }
    return n;
}

void soc_foreach(void (*fn)(SOCKCB *s, void *arg), void *arg)
{
    UINT i;

    if (fn == NULL) {
        return;
    }
    /* Ascending slot index: a callback that soc_destroys the current socket
     * vacates slot i, which is never revisited, so a later slot is untouched. */
    for (i = 0u; i < g_sockmax; i++) {
        SOCKCB *s = g_socktab[i].sock;
        if (s != NULL) {
            fn(s, arg);
        }
    }
}

#if NSF_DEBUG
UINT soc_debug_inuse(void)
{
    MMSTATS st;

    if (g_sockpool == NULL) {
        return 0u;
    }
    mm_stats(g_sockpool, &st);
    return st.inuse;
}
#endif
