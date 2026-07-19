/*
 * nsfeza.c -- NSFEZA: the EZASOKET API core + C API (see nsfeza.h, spec ch. 15,
 * ADR-0029).
 *
 * The surface-neutral core every facade funnels through. Each C-API function
 * builds an NSFRQE on the CALLER'S stack, nsfreq_call()s it (submit + WAIT on
 * r->ecb -- the M3-2 request transport), and maps the completion (retcode /
 * errno_) into the IBM EZASOKET convention. No transport knowledge, no protocol
 * knowledge: NSFEZA only translates calling conventions and keeps the
 * per-application socket-number mapping table the halfword API demands.
 *
 * PHASE 1: NSFEZA links into the application address space (like nsfreq.c's app
 * side) and runs on an APP task (the same TCB that issues the call), NOT the
 * executive. nsfreq_call submits to the executive and blocks on the request's
 * ecb; the executive dispatches and completes it. Module state (the registration
 * + mapping table) is single-registration per address space in v1 -- IBM's
 * per-subtask multitasking is deferred (docs/ezasoket-conformance.md 3).
 *
 * ADDRESS/PORT byte order: the caller's sockaddr_in carries sin_addr / sin_port
 * in NETWORK byte order. sa_get/sa_put move them BYTE BY BYTE to/from the NSF
 * internal form (addr = UINT octet-1-in-MSB, port = host-order USHORT -- the
 * NSFIP convention, spec 11.1) so the one source is correct on the big-endian
 * target AND the little-endian host.
 */
#include "nsfeza.h"
#include "nsfreq.h"             /* RQ_*, NSF_E*, NSFRQE, NSFRQE_EYE, nsfreq_call */
#include "nsfsoc.h"             /* NSF_AF_INET, NSF_SOCK_*, SEL_*, SHUT_*        */
#include "nsfsel.h"             /* NSFSELITEM, SEL_F_TIMED (the SELECT payload)  */
#include <string.h>

/* The mapping table is sized by the SOCKET pool limit; keep them locked. */
#if NSFEZA_MAXSOC != NSFSOC_MAX_DEFAULT
#error "NSFEZA_MAXSOC must equal NSFSOC_MAX_DEFAULT (the SOCKET pool limit)"
#endif

/* Default protocol numbers a SOCKET of proto 0 resolves to (spec 15.2). */
#define EZA_IPPROTO_TCP   6
#define EZA_IPPROTO_UDP   17

/* ---- per-application module state (Phase 1, single AS) --------------------- */
static UINT  g_apptok;                      /* 0 = not registered              */
static INT   g_maxsoc;                      /* effective (clamped) # of numbers */
static UINT  g_sockmap[NSFEZA_MAXSOC];      /* socket number -> desc; 0 = free  */
static UCHAR g_socknb[NSFEZA_MAXSOC];       /* per-number persistent non-block  */
static INT   g_eza_errno;                   /* last errno (the BSD idiom)       */

/* ---- helpers --------------------------------------------------------------- */

static void eza_rqe(NSFRQE *r, UINT fn, UINT desc)
{
    memset(r, 0, sizeof(*r));               /* also clears r->ecb (reset-before- */
    memcpy(r->eye, NSFRQE_EYE, 4);          /* WAIT, CLAUDE.md 3)                */
    r->fn       = (USHORT)fn;
    r->sockdesc = desc;
}

/* Socket number -> internal descriptor, or 0 if the number is out of range or
 * its mapping entry is free (closed / never opened) -> the caller maps 0 to
 * EBADF. Descriptors are never 0 (nsfsoc gen seeds to 1), so 0 is a safe free
 * marker. */
static UINT eza_desc(INT s)
{
    if (s < 0 || s >= g_maxsoc) {
        return 0u;
    }
    return g_sockmap[s];
}

/* Extract (addr, port) from a caller sockaddr_in, byte-wise (network order). */
static void sa_get(const NSF_SOCKADDR_IN *sa, UINT *addr, USHORT *port)
{
    const UCHAR *a = (const UCHAR *)&sa->sin_addr;
    const UCHAR *p = (const UCHAR *)&sa->sin_port;

    *addr = ((UINT)a[0] << 24) | ((UINT)a[1] << 16) |
            ((UINT)a[2] << 8)  |  (UINT)a[3];
    *port = (USHORT)(((USHORT)p[0] << 8) | (USHORT)p[1]);
}

/* Fill a caller sockaddr_in from the NSF internal (addr, port), byte-wise. */
static void sa_put(NSF_SOCKADDR_IN *sa, UINT addr, USHORT port)
{
    UCHAR *a = (UCHAR *)&sa->sin_addr;
    UCHAR *p = (UCHAR *)&sa->sin_port;

    memset(sa, 0, sizeof(*sa));
    sa->sin_family = (USHORT)NSF_AF_INET;   /* host order (BSD) */
    a[0] = (UCHAR)(addr >> 24); a[1] = (UCHAR)(addr >> 16);
    a[2] = (UCHAR)(addr >> 8);  a[3] = (UCHAR)addr;
    p[0] = (UCHAR)(port >> 8);  p[1] = (UCHAR)port;
}

/* sin_family is host order (not network) -- a plain read is right on both. */
static int sa_family_ok(const NSF_SOCKADDR_IN *sa)
{
    return sa->sin_family == (USHORT)NSF_AF_INET;
}

/* The persistent non-blocking request flag for socket number `s` (0 unless FIONBIO
 * / F_SETFL O_NONBLOCK set it). `s` is a valid number (the caller resolved it via
 * eza_desc first, so 0 <= s < g_maxsoc). */
static USHORT eza_nbflag(INT s)
{
    return (g_socknb[s] != 0u) ? (USHORT)RQ_F_NONBLOCK : (USHORT)0;
}

/* ---- SELECT fullword bit masks: numbered RIGHT TO LEFT, byte-wise ----------- *
 * Descriptor N is bit (N mod 32) from the LEAST-significant end of fullword
 * (N div 32): 0x00000001 in the first fullword = number 0, 0x80000000 = number 31,
 * the second fullword covers 32..63 (conformance §2.2 / ADR-0035). Each fullword is
 * read/written as 4 big-endian bytes (byte 0 = bits 24..31, byte 3 = bits 0..7), so
 * the ONE source is correct on the big-endian target AND the little-endian host --
 * exactly the M2 discipline. `n` is always < NSFEZA_MAXSOC (64), so at most two
 * fullwords are touched. */
static int mask_get(const UCHAR *mask, int n)
{
    const UCHAR *fw = mask + (n >> 5) * 4;      /* the fullword holding bit n     */
    int          b  = n & 31;                   /* bit index from the LSB         */

    return (fw[3 - (b >> 3)] >> (b & 7)) & 1;   /* byte 3 = LSB end               */
}

static void mask_set(UCHAR *mask, int n)
{
    UCHAR *fw = mask + (n >> 5) * 4;
    int    b  = n & 31;

    fw[3 - (b >> 3)] |= (UCHAR)(1u << (b & 7));
}

static void mask_clear(UCHAR *mask, int nwords)
{
    if (nwords > 0) {
        memset(mask, 0, (size_t)nwords * 4u);
    }
}

/* ---- the C API ------------------------------------------------------------- */

void nsfeza_init(void)
{
    UINT i;

    g_apptok    = 0u;
    g_maxsoc    = 0;
    g_eza_errno = 0;
    for (i = 0u; i < NSFEZA_MAXSOC; i++) {
        g_sockmap[i] = 0u;
        g_socknb[i]  = 0u;
    }
}

INT nsf_initapi(INT maxsoc, const char *tcpname, const char *adsname,
                const char *subtask, INT *maxsno)
{
    NSFRQE r;
    INT    eff;
    UINT   i;

    (void)tcpname; (void)adsname; (void)subtask;    /* Phase 1: single AS       */

    if (g_apptok != 0u) {                           /* one INITAPI per AS in v1 */
        g_eza_errno = NSF_EINVAL;                   /* (IBM 10197 spirit)       */
        return NSF_RETERR;
    }
    /* Accept the caller's MAXSOC, clamp to the pool limit; <=0 requests the
     * default (the full pool). MAXSNO reports the clamped reality (0-based). */
    eff = (maxsoc <= 0) ? NSFEZA_MAXSOC
                        : (maxsoc > NSFEZA_MAXSOC ? NSFEZA_MAXSOC : maxsoc);

    eza_rqe(&r, RQ_INITAPI, 0u);
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) {
        g_eza_errno = r.errno_;
        return NSF_RETERR;
    }
    g_apptok = r.apptok;
    g_maxsoc = eff;
    for (i = 0u; i < NSFEZA_MAXSOC; i++) {          /* fresh mapping table      */
        g_sockmap[i] = 0u;
        g_socknb[i]  = 0u;
    }
    if (maxsno != NULL) {
        *maxsno = eff - 1;                          /* highest assignable number */
    }
    return NSF_RETOK;
}

INT nsf_socket(INT af, INT type, INT proto)
{
    NSFRQE r;
    INT    n, i;

    if (g_apptok == 0u) {                           /* implicit INITAPI         */
        if (nsf_initapi(0, NULL, NULL, NULL, NULL) != NSF_RETOK) {
            return NSF_RETERR;                      /* g_eza_errno already set  */
        }
    }
    if (proto == 0) {                               /* default protocol         */
        proto = (type == NSF_SOCK_DGRAM)  ? EZA_IPPROTO_UDP
              : (type == NSF_SOCK_STREAM) ? EZA_IPPROTO_TCP : 0;
    }
    /* Reserve the lowest free socket number BEFORE creating the socket, so a
     * full mapping table never orphans a created socket. */
    n = -1;
    for (i = 0; i < g_maxsoc; i++) {
        if (g_sockmap[i] == 0u) { n = i; break; }
    }
    if (n < 0) {
        g_eza_errno = NSF_EMFILE;
        return NSF_RETERR;
    }
    eza_rqe(&r, RQ_SOCKET, 0u);
    r.apptok = g_apptok;
    r.p1 = (UINT)af; r.p2 = (UINT)type; r.p3 = (UINT)proto;
    nsfreq_call(&r);
    if (r.retcode < 0) {
        g_eza_errno = r.errno_;
        return NSF_RETERR;
    }
    g_sockmap[n] = (UINT)r.retcode;                 /* number -> internal desc  */
    g_socknb[n]  = 0u;                              /* default: blocking        */
    return n;                                       /* the 0-based number       */
}

INT nsf_bind(INT s, const NSF_SOCKADDR_IN *name, INT namelen)
{
    NSFRQE r;
    UINT   desc, addr;
    USHORT port;

    desc = eza_desc(s);
    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }
    if (name == NULL || namelen < (INT)sizeof(NSF_SOCKADDR_IN)) {
        g_eza_errno = NSF_EINVAL; return NSF_RETERR;
    }
    if (!sa_family_ok(name)) { g_eza_errno = NSF_EAFNOSUPPORT; return NSF_RETERR; }

    sa_get(name, &addr, &port);
    eza_rqe(&r, RQ_BIND, desc);
    r.p1 = addr; r.p2 = (UINT)port;
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return NSF_RETOK;
}

INT nsf_sendto(INT s, const void *buf, INT len, INT flags,
               const NSF_SOCKADDR_IN *name, INT namelen)
{
    NSFRQE r;
    UINT   desc, addr;
    USHORT port;

    desc = eza_desc(s);
    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }
    if (name == NULL || namelen < (INT)sizeof(NSF_SOCKADDR_IN)) {
        g_eza_errno = NSF_EINVAL; return NSF_RETERR;
    }
    if (!sa_family_ok(name)) { g_eza_errno = NSF_EAFNOSUPPORT; return NSF_RETERR; }
    if (len < 0)             { g_eza_errno = NSF_EINVAL;       return NSF_RETERR; }

    sa_get(name, &addr, &port);
    eza_rqe(&r, RQ_SENDTO, desc);
    r.ubuf = (void *)buf;               /* Phase 1 SENDTO reads it (read-only)  */
    r.ulen = (UINT)len;
    r.p1 = addr; r.p2 = (UINT)port;
    if ((flags & NSF_MSG_DONTWAIT) != 0) {
        r.flags = (USHORT)RQ_F_NONBLOCK;
    }
    nsfreq_call(&r);
    if (r.retcode < 0) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return r.retcode;                   /* byte count sent                      */
}

INT nsf_recvfrom(INT s, void *buf, INT len, INT flags,
                 NSF_SOCKADDR_IN *name, INT *namelen)
{
    NSFRQE r;
    UINT   desc;

    desc = eza_desc(s);
    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }
    if (len < 0)    { g_eza_errno = NSF_EINVAL; return NSF_RETERR; }

    eza_rqe(&r, RQ_RECVFROM, desc);
    r.ubuf = buf;
    r.ulen = (UINT)len;
    if ((flags & NSF_MSG_DONTWAIT) != 0) {
        r.flags = (USHORT)RQ_F_NONBLOCK;
    }
    nsfreq_call(&r);
    if (r.retcode < 0) { g_eza_errno = r.errno_; return NSF_RETERR; }
    if (name != NULL) {
        sa_put(name, r.p1, (USHORT)r.p2);           /* the sender's addr/port   */
        if (namelen != NULL) {
            *namelen = (INT)sizeof(NSF_SOCKADDR_IN);
        }
    }
    return r.retcode;                   /* byte count received                  */
}

INT nsf_close(INT s)
{
    NSFRQE r;
    UINT   desc;

    desc = eza_desc(s);
    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }

    eza_rqe(&r, RQ_CLOSE, desc);
    nsfreq_call(&r);
    g_sockmap[s] = 0u;                  /* the number is closed either way      */
    g_socknb[s]  = 0u;
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return NSF_RETOK;
}

INT nsf_getsockname(INT s, NSF_SOCKADDR_IN *name, INT *namelen)
{
    NSFRQE r;
    UINT   desc;

    desc = eza_desc(s);
    if (desc == 0u)     { g_eza_errno = NSF_EBADF;  return NSF_RETERR; }
    if (name == NULL)   { g_eza_errno = NSF_EINVAL; return NSF_RETERR; }

    eza_rqe(&r, RQ_GETSOCKNAME, desc);
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    sa_put(name, r.p1, (USHORT)r.p2);
    if (namelen != NULL) {
        *namelen = (INT)sizeof(NSF_SOCKADDR_IN);
    }
    return NSF_RETOK;
}

/* ---- the M4-5 verb set (ADR-0035) ------------------------------------------ */

INT nsf_connect(INT s, const NSF_SOCKADDR_IN *name, INT namelen)
{
    NSFRQE r;
    UINT   desc, addr;
    USHORT port;

    desc = eza_desc(s);
    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }
    if (name == NULL || namelen < (INT)sizeof(NSF_SOCKADDR_IN)) {
        g_eza_errno = NSF_EINVAL; return NSF_RETERR;
    }
    if (!sa_family_ok(name)) { g_eza_errno = NSF_EAFNOSUPPORT; return NSF_RETERR; }

    sa_get(name, &addr, &port);
    eza_rqe(&r, RQ_CONNECT, desc);
    r.p1 = addr; r.p2 = (UINT)port;
    r.flags = eza_nbflag(s);                        /* non-blocking -> EINPROGRESS */
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return NSF_RETOK;
}

INT nsf_listen(INT s, INT backlog)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }

    eza_rqe(&r, RQ_LISTEN, desc);
    r.p1 = (UINT)((backlog < 0) ? 0 : backlog);
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return NSF_RETOK;
}

INT nsf_accept(INT s, NSF_SOCKADDR_IN *name, INT *namelen)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);
    INT    n, i;

    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }

    /* Reserve a free socket number for the new connection BEFORE issuing ACCEPT, so
     * a full mapping table never strands an accepted child in the stack. */
    n = -1;
    for (i = 0; i < g_maxsoc; i++) {
        if (g_sockmap[i] == 0u) { n = i; break; }
    }
    if (n < 0) { g_eza_errno = NSF_EMFILE; return NSF_RETERR; }

    eza_rqe(&r, RQ_ACCEPT, desc);
    r.flags = eza_nbflag(s);                        /* non-blocking -> EWOULDBLOCK */
    nsfreq_call(&r);
    if (r.retcode < 0) { g_eza_errno = r.errno_; return NSF_RETERR; }

    g_sockmap[n] = (UINT)r.retcode;                 /* new number -> child desc  */
    g_socknb[n]  = 0u;                              /* the new socket is blocking */
    if (name != NULL) {
        sa_put(name, r.p1, (USHORT)r.p2);           /* the peer's addr/port      */
        if (namelen != NULL) {
            *namelen = (INT)sizeof(NSF_SOCKADDR_IN);
        }
    }
    return n;                                       /* the new 0-based number    */
}

INT nsf_send(INT s, const void *buf, INT len, INT flags)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u) { g_eza_errno = NSF_EBADF;  return NSF_RETERR; }
    if (len < 0)    { g_eza_errno = NSF_EINVAL; return NSF_RETERR; }

    eza_rqe(&r, RQ_SEND, desc);
    r.ubuf  = (void *)buf;
    r.ulen  = (UINT)len;
    r.flags = (USHORT)(eza_nbflag(s) |
              (((flags & NSF_MSG_DONTWAIT) != 0) ? (USHORT)RQ_F_NONBLOCK : (USHORT)0));
    nsfreq_call(&r);
    if (r.retcode < 0) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return r.retcode;                               /* byte count sent           */
}

INT nsf_recv(INT s, void *buf, INT len, INT flags)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u) { g_eza_errno = NSF_EBADF;  return NSF_RETERR; }
    if (len < 0)    { g_eza_errno = NSF_EINVAL; return NSF_RETERR; }

    eza_rqe(&r, RQ_RECV, desc);
    r.ubuf  = buf;
    r.ulen  = (UINT)len;
    r.flags = (USHORT)(eza_nbflag(s) |
              (((flags & NSF_MSG_DONTWAIT) != 0) ? (USHORT)RQ_F_NONBLOCK : (USHORT)0));
    nsfreq_call(&r);
    if (r.retcode < 0) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return r.retcode;                               /* byte count, or 0 at EOF   */
}

INT nsf_shutdown(INT s, INT how)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }
    if (how < SHUT_RD || how > SHUT_RDWR) {
        g_eza_errno = NSF_EINVAL; return NSF_RETERR;
    }
    eza_rqe(&r, RQ_SHUTDOWN, desc);
    r.p1 = (UINT)how;
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return NSF_RETOK;
}

INT nsf_getpeername(INT s, NSF_SOCKADDR_IN *name, INT *namelen)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u)   { g_eza_errno = NSF_EBADF;  return NSF_RETERR; }
    if (name == NULL) { g_eza_errno = NSF_EINVAL; return NSF_RETERR; }

    eza_rqe(&r, RQ_GETPEERNAME, desc);
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    sa_put(name, r.p1, (USHORT)r.p2);
    if (namelen != NULL) {
        *namelen = (INT)sizeof(NSF_SOCKADDR_IN);
    }
    return NSF_RETOK;
}

INT nsf_select(INT maxsoc, void *rmask, void *wmask, void *emask,
               INT tv_sec, INT tv_usec)
{
    NSFRQE     r;
    NSFSELITEM items[NSFEZA_MAXSOC];    /* one per socket number set in a mask   */
    UCHAR     *rm = (UCHAR *)rmask, *wm = (UCHAR *)wmask, *em = (UCHAR *)emask;
    UINT       nitems = 0u, k;
    int        n, nwords;

    if (maxsoc > g_maxsoc) { maxsoc = g_maxsoc; }   /* only real numbers exist   */
    if (maxsoc < 0)        { maxsoc = 0; }
    nwords = (maxsoc + 31) / 32;                    /* fullwords covering 0..max-1*/

    /* Build the descriptor/interest item array from the send masks. The socket
     * NUMBER is stashed in the item's rsvd[0] (the executive only writes `ready`,
     * never rsvd), so the output pass maps a ready item straight back to its bit. */
    for (n = 0; n < maxsoc; n++) {
        UCHAR want = 0u;
        UINT  desc;

        if (rm != NULL && mask_get(rm, n)) { want |= (UCHAR)SEL_READ;  }
        if (wm != NULL && mask_get(wm, n)) { want |= (UCHAR)SEL_WRITE; }
        /* emask (exception) is read but never acted on -- TAKESOCKET unsupported. */
        if (want == 0u) { continue; }
        desc = g_sockmap[n];
        if (desc == 0u) { continue; }               /* number set but not a socket*/

        items[nitems].desc    = desc;
        items[nitems].want    = want;
        items[nitems].ready   = 0u;
        items[nitems].rsvd[0] = (UCHAR)n;
        items[nitems].rsvd[1] = 0u;
        nitems++;
    }

    eza_rqe(&r, RQ_SELECT, 0u);
    r.ubuf = (nitems > 0u) ? (void *)items : NULL;
    r.ulen = nitems;
    r.p1   = (UINT)((tv_sec < 0) ? 0 : tv_sec);
    r.p2   = (UINT)((tv_sec < 0) ? 0 : tv_usec);
    r.p3   = (tv_sec < 0) ? 0u : (UINT)SEL_F_TIMED; /* absent flag => wait forever*/
    nsfreq_call(&r);
    if (r.retcode < 0) { g_eza_errno = r.errno_; return NSF_RETERR; }

    /* Rewrite the masks in place: clear, then set the ready bits (emask -> zero). */
    if (rm != NULL) { mask_clear(rm, nwords); }
    if (wm != NULL) { mask_clear(wm, nwords); }
    if (em != NULL) { mask_clear(em, nwords); }
    for (k = 0u; k < nitems; k++) {
        n = (int)items[k].rsvd[0];
        if ((items[k].ready & (UCHAR)SEL_READ)  != 0u && rm != NULL) { mask_set(rm, n); }
        if ((items[k].ready & (UCHAR)SEL_WRITE) != 0u && wm != NULL) { mask_set(wm, n); }
    }
    return r.retcode;                               /* ready count (0 on timeout)*/
}

INT nsf_setsockopt(INT s, INT level, INT optname, const void *optval, INT optlen)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }

    eza_rqe(&r, RQ_SETSOCKOPT, desc);
    r.p1 = (UINT)level;
    r.p2 = (UINT)optname;
    r.p3 = (optval != NULL && optlen >= (INT)sizeof(INT))
         ? (UINT)*(const INT *)optval : 0u;         /* same-space value (int)    */
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    return NSF_RETOK;
}

INT nsf_getsockopt(INT s, INT level, INT optname, void *optval, INT *optlen)
{
    NSFRQE r;
    UINT   desc = eza_desc(s);

    if (desc == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }
    if (optval == NULL || optlen == NULL || *optlen < (INT)sizeof(INT)) {
        g_eza_errno = NSF_EINVAL; return NSF_RETERR;
    }
    eza_rqe(&r, RQ_GETSOCKOPT, desc);
    r.p1 = (UINT)level;
    r.p2 = (UINT)optname;
    nsfreq_call(&r);
    if (r.retcode != NSF_RETOK) { g_eza_errno = r.errno_; return NSF_RETERR; }
    *(INT *)optval = (INT)r.p3;
    *optlen = (INT)sizeof(INT);
    return NSF_RETOK;
}

INT nsf_fcntl(INT s, INT cmd, INT arg)
{
    if (eza_desc(s) == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }

    if (cmd == NSF_F_GETFL) {
        return (g_socknb[s] != 0u) ? NSF_O_NONBLOCK : 0;
    }
    if (cmd == NSF_F_SETFL) {
        g_socknb[s] = ((arg & NSF_O_NONBLOCK) != 0) ? 1u : 0u;
        return NSF_RETOK;
    }
    g_eza_errno = NSF_EOPNOTSUPP;
    return NSF_RETERR;
}

INT nsf_ioctl(INT s, INT cmd, void *arg)
{
    if (eza_desc(s) == 0u) { g_eza_errno = NSF_EBADF; return NSF_RETERR; }

    if ((UINT)cmd == NSF_FIONBIO) {
        INT on = (arg != NULL) ? *(const INT *)arg : 0;
        g_socknb[s] = (on != 0) ? 1u : 0u;
        return NSF_RETOK;
    }
    g_eza_errno = NSF_EOPNOTSUPP;
    return NSF_RETERR;
}

INT nsf_termapi(void)
{
    NSFRQE r;
    INT    i;

    if (g_apptok == 0u) {
        return NSF_RETOK;                           /* nothing registered       */
    }
    /* Close every live socket of this app (RQ_CLOSE per mapping entry -- clears
     * each entry), then RQ_TERMAPI (mass-teardown safety net + drops the app
     * registration), then empty NSFEZA's own state. */
    for (i = 0; i < g_maxsoc; i++) {
        if (g_sockmap[i] != 0u) {
            eza_rqe(&r, RQ_CLOSE, g_sockmap[i]);
            nsfreq_call(&r);
            g_sockmap[i] = 0u;
            g_socknb[i]  = 0u;
        }
    }
    eza_rqe(&r, RQ_TERMAPI, 0u);
    r.apptok = g_apptok;
    nsfreq_call(&r);
    g_apptok = 0u;
    g_maxsoc = 0;
    return NSF_RETOK;
}

INT nsf_lasterrno(void)
{
    return g_eza_errno;
}

/* ==========================================================================
 * The EZASOH03 plist decoder (ADR-0029). The HLASM facade EZASOH03 is a thin
 * veneer that hands its R1 plist straight here; ALL function-code decode and
 * plist marshalling live in this portable, host-testable C -- so the only
 * MVS-only-unverified code is the veneer's linkage.
 *
 * The plist overlays the EZASOH03 argument list (SC31-7187-03 / Shelby's
 * EZASOH03 ABI, pinned in docs/ezasoket-conformance.md 6):
 *     +0  A(4-char EBCDIC function code)
 *     +4  A(ERRNO,   fullword)         <- valid only when RETCODE < 0
 *     +8  A(RETCODE, fullword)
 *     +12.. A(function-specific parameters), in the CALL 'EZASOKET' order
 * On the target each slot is a 4-byte A() address; on the host they are natural
 * pointers -- the struct is accessed BY FIELD, so both are correct. A value
 * parameter is dereferenced (fullword or halfword); a struct/buffer parameter
 * (NAME/BUF/IDENT) is used as the address directly.
 *
 * TERMAPI carries NO ERRNO/RETCODE (only +0); every other code has them at
 * +4/+8. Every path stores RETCODE/ERRNO where they exist and returns 0 -- the
 * ABI's "R15 is always 0"; a real error lives in RETCODE/ERRNO, never R15, and
 * an unsupported/unknown code completes cleanly with RETCODE -1, ERRNO 45
 * (EOPNOTSUPP), never an abend.
 * ========================================================================== */

typedef struct ezapl {
    const char *func;                   /* +0  A(4-char code)                   */
    INT        *errnop;                 /* +4  A(ERRNO)                         */
    INT        *retcodep;               /* +8  A(RETCODE)                       */
    void       *arg[8];                 /* +12.. A(function-specific params)    */
} EZAPL;

/* Charset-transparent 4-char code compare (char literals, never byte values --
 * so the same source matches EBCDIC on target and ASCII on host). */
#define EQ4(f, a, b, c, d) \
    ((f)[0] == (a) && (f)[1] == (b) && (f)[2] == (c) && (f)[3] == (d))

/* Parameter accessors: fullword value, halfword value, raw address. */
static INT   av(const EZAPL *pl, int i) { return *(const INT   *)pl->arg[i]; }
static INT   ah(const EZAPL *pl, int i) { return (INT)*(const USHORT *)pl->arg[i]; }
static void *ap(const EZAPL *pl, int i) { return pl->arg[i]; }

/* Store RETCODE/ERRNO into the plist (both slots present for every code but
 * TERMAPI, which is handled before this is reached). */
static void eza_ret(const EZAPL *pl, INT rc, INT er)
{
    if (pl->retcodep != NULL) { *pl->retcodep = rc; }
    if (pl->errnop   != NULL) { *pl->errnop   = er; }
}

/* ERRNO for a completed C-API result: the last errno on failure, 0 on success. */
static INT eza_errno_of(INT rc)
{
    return (rc < 0) ? nsf_lasterrno() : 0;
}

/* SELECT via the EZASOH03 plist (SELECT MAXSOC, TIMEOUT, RSNDMSK, WSNDMSK, ESNDMSK,
 * RRETMSK, WRETMSK, ERETMSK). The EZASMI surface keeps the input (send) masks and
 * output (return) masks separate, while nsf_select rewrites in place -- so copy the
 * send masks into the return masks and run the in-place select on THOSE. TIMEOUT is
 * A(2 fullwords: sec, usec); a NULL TIMEOUT pointer waits forever. ESNDMSK is read
 * but never acted on and ERETMSK is always cleared (no exception source in v1).
 * Returns the ready count / 0 (timeout) / -1 (error). */
static INT eza_oh03_select(const EZAPL *pl)
{
    INT        maxsoc = av(pl, 0);
    const INT *tv     = (const INT *)ap(pl, 1);
    INT        sec    = (tv != NULL) ? tv[0] : -1;      /* no timeout => forever  */
    INT        usec   = (tv != NULL) ? tv[1] : 0;
    UCHAR     *rsnd = (UCHAR *)ap(pl, 2), *wsnd = (UCHAR *)ap(pl, 3);
    UCHAR     *rret = (UCHAR *)ap(pl, 5), *wret = (UCHAR *)ap(pl, 6);
    UCHAR     *eret = (UCHAR *)ap(pl, 7);
    int        nwords, mbytes;

    if (maxsoc > g_maxsoc) { maxsoc = g_maxsoc; }
    if (maxsoc < 0)        { maxsoc = 0; }
    nwords = (maxsoc + 31) / 32;
    mbytes = nwords * 4;

    if (rret != NULL) {
        if (rsnd != NULL) { memcpy(rret, rsnd, (size_t)mbytes); }
        else              { memset(rret, 0, (size_t)mbytes); }
    }
    if (wret != NULL) {
        if (wsnd != NULL) { memcpy(wret, wsnd, (size_t)mbytes); }
        else              { memset(wret, 0, (size_t)mbytes); }
    }
    if (eret != NULL) { memset(eret, 0, (size_t)mbytes); }   /* exception => empty */

    return nsf_select(maxsoc, rret, wret, NULL, sec, usec);
}

INT nsf_ezasoh03(void *plist)
{
    EZAPL      *pl = (EZAPL *)plist;
    const char *f;
    INT         rc;
    INT         namelen;

    if (pl == NULL || pl->func == NULL) {
        return 0;                                   /* R15 always 0             */
    }
    f = pl->func;

    if (EQ4(f, 'I', 'N', 'I', 'T')) {               /* INITAPI                  */
        const char *ident = (const char *)ap(pl, 1);
        rc = nsf_initapi(av(pl, 0),                 /* MAXSOC                    */
                         ident,                     /* TCPNAME (IDENT+0)        */
                         (ident != NULL) ? ident + 8 : NULL,  /* ADSNAME (+8)   */
                         (const char *)ap(pl, 2),   /* SUBTASK                  */
                         (INT *)ap(pl, 3));         /* MAXSNO (out)             */
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'S', 'O', 'C', 'K')) {        /* SOCKET                   */
        rc = nsf_socket(av(pl, 0), av(pl, 1), av(pl, 2));   /* AF, TYPE, PROTO  */
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'B', 'I', 'N', 'D')) {        /* BIND                     */
        rc = nsf_bind(ah(pl, 0),                    /* S (halfword)             */
                      (const NSF_SOCKADDR_IN *)ap(pl, 1),
                      (INT)sizeof(NSF_SOCKADDR_IN));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'S', 'N', 'D', 'T')) {        /* SENDTO                   */
        rc = nsf_sendto(ah(pl, 0), ap(pl, 3), av(pl, 2), av(pl, 1),
                        (const NSF_SOCKADDR_IN *)ap(pl, 4),
                        (INT)sizeof(NSF_SOCKADDR_IN));   /* S,BUF,NBYTE,FLAGS,NAME */
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'R', 'C', 'V', 'F')) {        /* RECVFROM                 */
        namelen = (INT)sizeof(NSF_SOCKADDR_IN);
        rc = nsf_recvfrom(ah(pl, 0), ap(pl, 3), av(pl, 2), av(pl, 1),
                          (NSF_SOCKADDR_IN *)ap(pl, 4), &namelen);
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'C', 'L', 'O', 'S')) {        /* CLOSE                    */
        rc = nsf_close(ah(pl, 0));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'G', 'E', 'T', 'S')) {        /* GETSOCKNAME              */
        namelen = (INT)sizeof(NSF_SOCKADDR_IN);
        rc = nsf_getsockname(ah(pl, 0), (NSF_SOCKADDR_IN *)ap(pl, 1), &namelen);
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'T', 'E', 'R', 'M')) {        /* TERMAPI (no ERRNO/RETCODE)*/
        (void)nsf_termapi();
    /* -- the M4-5 verb set (ADR-0035); Shelby's codes reused, SOPT/GOPT new -- */
    } else if (EQ4(f, 'C', 'O', 'N', 'N')) {        /* CONNECT: S, NAME          */
        rc = nsf_connect(ah(pl, 0), (const NSF_SOCKADDR_IN *)ap(pl, 1),
                         (INT)sizeof(NSF_SOCKADDR_IN));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'L', 'I', 'S', 'T')) {        /* LISTEN: S, BACKLOG        */
        rc = nsf_listen(ah(pl, 0), av(pl, 1));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'A', 'C', 'C', 'E')) {        /* ACCEPT: S, NAME           */
        namelen = (INT)sizeof(NSF_SOCKADDR_IN);
        rc = nsf_accept(ah(pl, 0), (NSF_SOCKADDR_IN *)ap(pl, 1), &namelen);
        eza_ret(pl, rc, eza_errno_of(rc));          /* RETCODE = new socket #    */
    } else if (EQ4(f, 'S', 'E', 'N', 'D')) {        /* SEND: S, FLAGS, NBYTE, BUF*/
        rc = nsf_send(ah(pl, 0), ap(pl, 3), av(pl, 2), av(pl, 1));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'R', 'E', 'C', 'V')) {        /* RECV: S, FLAGS, NBYTE, BUF*/
        rc = nsf_recv(ah(pl, 0), ap(pl, 3), av(pl, 2), av(pl, 1));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'S', 'H', 'U', 'T')) {        /* SHUTDOWN: S, HOW          */
        rc = nsf_shutdown(ah(pl, 0), av(pl, 1));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'G', 'E', 'T', 'P')) {        /* GETPEERNAME: S, NAME      */
        namelen = (INT)sizeof(NSF_SOCKADDR_IN);
        rc = nsf_getpeername(ah(pl, 0), (NSF_SOCKADDR_IN *)ap(pl, 1), &namelen);
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'S', 'E', 'L', 'E')) {        /* SELECT (masks, ADR-0035)  */
        rc = eza_oh03_select(pl);
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'S', 'O', 'P', 'T')) {        /* SETSOCKOPT: S,OPT,VAL,LEN */
        /* No LEVEL in the EZASMI plist -- IBM encodes it in OPTNAME; NSF's minimal
         * set defaults SOL_SOCKET (the C API is the level-aware surface). */
        rc = nsf_setsockopt(ah(pl, 0), NSF_SOL_SOCKET, av(pl, 1),
                            ap(pl, 2), av(pl, 3));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'G', 'O', 'P', 'T')) {        /* GETSOCKOPT: S,OPT,VAL,LEN */
        rc = nsf_getsockopt(ah(pl, 0), NSF_SOL_SOCKET, av(pl, 1),
                            ap(pl, 2), (INT *)ap(pl, 3));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else if (EQ4(f, 'I', 'O', 'C', 'T')) {        /* IOCTL: S, COMMAND, REQARG */
        rc = nsf_ioctl(ah(pl, 0), av(pl, 1), ap(pl, 2));
        eza_ret(pl, rc, eza_errno_of(rc));
    } else {
        /* Any remaining unimplemented code (GETH/GETA/NTOP/PTON/TASK/FCNT/...) and
         * any unknown code: complete cleanly, never abend (ADR-0029). FCNTL has no
         * EZASMI TYPE -- FIONBIO rides IOCT above. */
        eza_ret(pl, NSF_RETERR, NSF_EOPNOTSUPP);
    }
    return 0;                                       /* R15 is ALWAYS 0          */
}
