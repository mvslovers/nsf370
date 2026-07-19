#ifndef NSFSEL_H
#define NSFSEL_H
/*
 * nsfsel.h -- NSFSEL: the SELECT multiplex engine (spec ch. 10.3, ADR-0035).
 *
 * SELECT is the one EZASOKET verb that is NOT one request bound to one socket: a
 * single NSFRQE waits on N sockets for a readiness edge on ANY of them. It cannot
 * ride the SOCKCB pend_* slots (one per kind per socket), so it gets its own small
 * engine -- a fixed static pool of parked-SELECT control blocks, each with an
 * EMBEDDED timeout TMR, re-evaluated whenever a socket's readiness could have
 * changed.
 *
 * REACHED ONLY THROUGH REGISTRATION SEAMS, so NSFSEL stays out of the tcp/udp/req
 * link graphs (mirrors nsfip_register_proto / evt_set_request):
 *   - nsfsel_init registers its re-eval callback with NSFSOC
 *     (soc_set_select_notify) -- protocol readiness pokes (soc_notify_ready) reach
 *     the parked SELECTs without any protocol depending on NSFSEL; and
 *   - nsfsel_init registers its RQ_SELECT handler with NSFREQ
 *     (nsfreq_register_select) -- a build WITHOUT nsfsel.c leaves RQ_SELECT at
 *     NSF_EOPNOTSUPP, unchanged.
 * A build that does NOT link nsfsel.c therefore sees no behaviour change at all.
 *
 * READINESS (side-effect-free) is a PROTOPS.poll probe (nsfsoc.h): TCP supplies a
 * precise tcp_poll; a NULL poll (UDP, the M3 dummies) uses the generic fallback
 * (read = rxq/acceptq non-empty, write = always). There is no EXCEPTION source in
 * NSF v1 (TAKESOCKET unsupported), so ERETMSK is always zero (the facade zeroes it).
 *
 * NUMBERING / PHASE. The EZASOKET masks are socket NUMBERS; only NSFEZA owns the
 * number->descriptor mapping, so the FACADE translates and passes a
 * descriptor/interest ITEM ARRAY (NSFSELITEM below) by the frozen NSFRQE's `ubuf`
 * (same-space in Phase 1, a keyed cross-memory move in Phase 2). The executive-side
 * engine works only in descriptors and writes each item's `ready` back; NSFEZA maps
 * that to the return masks. The NSFRQE layout is unchanged (ADR-0035).
 */

#include "nsf.h"

/* Concurrently parked SELECTs (spec 10.3 / ADR-0035: "a small fixed number"). One
 * SELECT per app task is IBM's effective model and v1 is single-registration, so
 * four covers the realistic fan-out; a fifth is rejected NSF_EMFILE. Named so the
 * bound is one edit. */
#define NSFSEL_MAX   4

/* The per-socket item the facade builds for a SELECT (ADR-0035). `desc` is the
 * internal (gen<<16)|id (the facade resolved it from the socket number); `want` is
 * the union of the read/write interest bits for that number (SEL_READ/SEL_WRITE,
 * nsfsoc.h); the EXECUTIVE writes `ready` = the ready subset. Pointer-free + fixed
 * size, so it embeds identically on host and target, and it is the `ubuf` payload
 * (same-space Phase 1; keyed move Phase 2). */
typedef struct nsfselitem {
    UINT  desc;                 /* internal (gen<<16)|id                        */
    UCHAR want;                 /* SEL_READ|SEL_WRITE requested                 */
    UCHAR ready;                /* SEL_* the executive found ready (output)     */
    UCHAR rsvd[2];              /* pad to a fullword                            */
} NSFSELITEM;                   /* 8 bytes */

/* NSFRQE param encoding for RQ_SELECT (ADR-0035): ubuf = the NSFSELITEM array,
 * ulen = its count, p1 = timeout seconds, p2 = timeout microseconds, p3 = flags.
 * SEL_F_TIMED present => a finite timeout (use p1/p2); absent => block forever.
 * With SEL_F_TIMED set, p1==0 && p2==0 is a poll (return immediately, no park). */
#define SEL_F_TIMED   0x0001u

/* asm() external-symbol aliases (CLAUDE.md §3, "External symbols"): scheme NSFSL*,
 * clear of NSFSO* (sockets) / NSFST* (stats) / NSFSTC (the STC). The engine's
 * dispatch + re-eval callbacks are STATIC (reached only through the registration
 * function pointers), so only the two externally-called entries need aliases:
 *   nsfsel_init NSFSLINI   nsfsel_debug_inuse NSFSLDBI
 */

/* Reset the parked-SELECT pool AND register the two seams (soc_set_select_notify +
 * nsfreq_register_select). Idempotent; safe to re-call between test scenarios.
 * MUST run after nsftmr_init (the SELCB timers link into NSFTMR's queue), after
 * soc_init and after nsfreq_init (the registrations). */
void nsfsel_init(void) asm("NSFSLINI");

#if NSF_DEBUG
/* Leak-gate diagnostic (host tests): parked SELECTs currently in use; returns to
 * baseline (0) once every SELECT is completed. Mirrors soc_debug_inuse. */
UINT nsfsel_debug_inuse(void) asm("NSFSLDBI");
#endif

#endif /* NSFSEL_H */
