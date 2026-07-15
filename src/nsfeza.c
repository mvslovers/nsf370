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
#include "nsfsoc.h"             /* NSF_AF_INET, NSF_SOCK_*, NSFSOC_MAX_DEFAULT   */
#include <string.h>

/* The mapping table is sized by the SOCKET pool limit; keep them locked. */
#if NSFEZA_MAXSOC != NSFSOC_MAX_DEFAULT
#error "NSFEZA_MAXSOC must equal NSFSOC_MAX_DEFAULT (the SOCKET pool limit)"
#endif

/* Default protocol numbers a SOCKET of proto 0 resolves to (spec 15.2). */
#define EZA_IPPROTO_TCP   6
#define EZA_IPPROTO_UDP   17

/* ---- per-application module state (Phase 1, single AS) --------------------- */
static UINT g_apptok;                       /* 0 = not registered              */
static INT  g_maxsoc;                       /* effective (clamped) # of numbers */
static UINT g_sockmap[NSFEZA_MAXSOC];       /* socket number -> desc; 0 = free  */
static INT  g_eza_errno;                    /* last errno (the BSD idiom)       */

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

/* ---- the C API ------------------------------------------------------------- */

void nsfeza_init(void)
{
    UINT i;

    g_apptok    = 0u;
    g_maxsoc    = 0;
    g_eza_errno = 0;
    for (i = 0u; i < NSFEZA_MAXSOC; i++) {
        g_sockmap[i] = 0u;
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
    } else {
        /* Every code NSF does not implement in M3-4 (ACCE/CONN/LIST/RECV/SELE/
         * SEND/SHUT/IOCT/GETH/GETP/GETA/NTOP/PTON/TASK) and any unknown code:
         * complete cleanly, never abend (ADR-0029). */
        eza_ret(pl, NSF_RETERR, NSF_EOPNOTSUPP);
    }
    return 0;                                       /* R15 is ALWAYS 0          */
}
