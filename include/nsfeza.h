#ifndef NSFEZA_H
#define NSFEZA_H
/*
 * nsfeza.h -- NSFEZA: the EZASOKET API layer (spec ch. 15, ADR-0029).
 *
 * NSFEZA is the application-visible socket API. It is a THIN surface-neutral
 * CORE that every facade funnels through: build an NSFRQE, submit + WAIT
 * (nsfreq_call), and map the completion (retcode/errno_) back into the IBM
 * EZASOKET RETCODE/ERRNO convention (spec 15.1). No facade contains socket
 * logic; each only marshals its own calling convention into this core:
 *
 *   EZASMI macros (Shelby's, unmodified) --> EZASOH03 facade (asm/ezasoh03) --.
 *   C API (this header, @@NS* alias namespace) --------------------------------+--> NSFEZA core
 *   CALL 'EZASOKET' CL16 facade (M6, when a caller needs it) ------------------'    (build RQE ->
 *                                                                                    submit -> WAIT ->
 *                                                                                    map RETCODE/ERRNO)
 *
 * The C API lives in its OWN 8-char asm() alias namespace, @@NS*, deliberately
 * DISJOINT from libc370's dyn75 @@75* surface (socket()/recv()/...) so both link
 * side by side with no collision (ADR-0029; the M6 relink-only story re-points
 * @@75* at this core). Every cross-module function pins a unique alias
 * (CLAUDE.md 3, "External symbols"): cc370 folds an external name to 8 chars, so
 * two names agreeing in the first 8 mangled chars would bind to one definition.
 *   nsf_initapi     @@NSINIT   nsf_socket      @@NSSOCK
 *   nsf_bind        @@NSBIND   nsf_sendto      @@NSSNDT
 *   nsf_recvfrom    @@NSRCVF   nsf_close       @@NSCLOS
 *   nsf_getsockname @@NSGSKN   nsf_termapi     @@NSTERM
 *   nsf_lasterrno   @@NSERNO   nsfeza_init     @@NSEZAI
 *   nsf_ezasoh03    @@NSOH03   (the EZASOH03 plist decoder; see src/nsfeza.c)
 *
 * SOCKET NUMBERS ARE HALFWORD, 0-BASED (SC31-7187-03 5.4.16: "The lowest socket
 * number is 0. If you have 50 sockets, they are numbered from 0 to 49"). The
 * internal descriptor is a fullword (gen<<16)|id with a generation guard and
 * CANNOT be surfaced, so the core keeps a per-application MAPPING TABLE
 * (socket number -> internal descriptor), anchored at the INITAPI registration
 * (RQ_INITAPI apptok, M3-2). CLOSE clears an entry -> any later use of that
 * number draws EBADF (9) at the facade; the core's gen-check still catches
 * internal reuse -- stale-handle protection end to end.
 *
 * PHASE 1: NSFEZA links into the APPLICATION (like nsfreq.c's app side), same
 * address space, problem state. Its state (the registration + the mapping table)
 * is module-global -- one INITAPI per address space in v1. IBM's per-subtask
 * multitasking (MF=, caller-cleared task storage) and the thread-safety of the
 * global ERRNO are deferred; see docs/ezasoket-conformance.md 3.
 *
 * ERRNO values are the classic IBM Table 67 (BSD-derived) numbers pinned in
 * nsfreq.h (NSF_E*); a failed call returns RETCODE -1 and sets the errno the
 * facade reads with nsf_lasterrno(). Unsupported functions return RETCODE -1,
 * ERRNO 45 (EOPNOTSUPP) -- never an abend (ADR-0029).
 */

#include "nsf.h"

/* The BSD sockaddr_in NSF exposes at the API boundary -- 16 bytes, the classic
 * IBM/MVS TCP/IP layout (no 4.4BSD sa_len). sin_port and sin_addr are NETWORK
 * byte order in memory (the application uses htons/htonl), exactly as BSD
 * requires; sin_family is host order. NSFEZA reads/writes sin_port/sin_addr
 * BYTE BY BYTE (the M2 big-endian discipline, spec 11.1) so the one source is
 * correct on the big-endian S/370 target AND the little-endian host test build:
 * a native read already yields the right value on target, and the byte-wise
 * path makes the host agree. Pointer-free and fixed size, so it embeds
 * identically on host and target. */
typedef struct nsf_sockaddr_in {
    USHORT sin_family;          /*  2  @0   address family (host order)         */
    USHORT sin_port;            /*  2  @2   port, network byte order            */
    UINT   sin_addr;            /*  4  @4   IPv4 address, network byte order     */
    UCHAR  sin_zero[8];         /*  8  @8   padding to 16 bytes                 */
} NSF_SOCKADDR_IN;              /* 16 bytes */

/* recv/send flags (the EZASOKET FLAGS argument). M3-4 honours only DONTWAIT
 * (the BSD MSG_DONTWAIT bit, value 0x40): it maps to RQ_F_NONBLOCK for one
 * non-blocking call. FCNTL/FIONBIO (a persistent socket attribute) is M4, so
 * DONTWAIT bridges the gap for a bounded non-blocking RECVFROM in v1
 * (docs/ezasoket-conformance.md). Other MSG_* bits are accepted and ignored. */
#define NSF_MSG_DONTWAIT   0x40

/* Highest socket number the mapping table can assign; equals the SOCKET pool
 * limit (nsfsoc.h NSFSOC_MAX_DEFAULT). nsfeza.c #errors if the two drift. */
#define NSFEZA_MAXSOC      64

/* ==========================================================================
 * The C API. IBM-compatible semantics with BSD-shaped signatures. Every call
 * builds an NSFRQE on the caller's stack, nsfreq_call()s it (submit + WAIT on
 * r->ecb), and maps completion to the IBM convention: RETCODE 0 or a byte count
 * on success, -1 on error; SOCKET returns the descriptor (>= 0) in RETCODE.
 * ERRNO (nsf_lasterrno()) is valid only when RETCODE is negative.
 * ========================================================================== */

/* Reset NSFEZA module state (drop the registration + empty the mapping table).
 * Idempotent; call at application startup (and between test scenarios). Does NOT
 * touch NSFSOC/NSFREQ -- those have their own soc_init/nsfreq_init. */
void nsfeza_init(void) asm("@@NSEZAI");

/* INITAPI: register this application with the stack. maxsoc is accepted and
 * CLAMPED to the pool limit (NSFEZA_MAXSOC); *maxsno (if non-NULL) returns the
 * highest assignable socket number (clamped_maxsoc - 1). tcpname/adsname/subtask
 * are the IBM identity strings -- ignored in Phase 1 (single address space).
 * Returns 0 on success, -1 on error. Passing maxsoc <= 0 requests the default
 * (the full pool). */
INT nsf_initapi(INT maxsoc, const char *tcpname, const char *adsname,
                const char *subtask, INT *maxsno) asm("@@NSINIT");

/* SOCKET: create a socket. af = NSF_AF_INET (2); type = NSF_SOCK_STREAM/DGRAM;
 * proto = 0 selects the default (dgram->UDP 17, stream->TCP 6). Returns the
 * 0-based socket number (>= 0) on success, -1 on error. AUTO-REGISTERS the
 * application (implicit INITAPI with defaults) when no INITAPI preceded it
 * (SC31-7187-03; ADR-0029). */
INT nsf_socket(INT af, INT type, INT proto) asm("@@NSSOCK");

/* BIND: assign the local name. name is a sockaddr_in; namelen its length
 * (>= 16). Returns 0 on success, -1 on error. */
INT nsf_bind(INT s, const NSF_SOCKADDR_IN *name, INT namelen) asm("@@NSBIND");

/* SENDTO: send a datagram to name. flags may carry NSF_MSG_DONTWAIT. Returns the
 * byte count sent on success, -1 on error. */
INT nsf_sendto(INT s, const void *buf, INT len, INT flags,
               const NSF_SOCKADDR_IN *name, INT namelen) asm("@@NSSNDT");

/* RECVFROM: receive a datagram. On success *name (if non-NULL) gets the sender
 * address and *namelen is set to 16. flags may carry NSF_MSG_DONTWAIT (return -1
 * / EWOULDBLOCK when no datagram is queued). Returns the byte count on success,
 * -1 on error. */
INT nsf_recvfrom(INT s, void *buf, INT len, INT flags,
                 NSF_SOCKADDR_IN *name, INT *namelen) asm("@@NSRCVF");

/* CLOSE: close socket s and clear its mapping entry. A later use of s returns
 * EBADF. Returns 0 on success, -1 on error. */
INT nsf_close(INT s) asm("@@NSCLOS");

/* GETSOCKNAME: return the local name in *name (*namelen set to 16). Returns 0 on
 * success, -1 on error. */
INT nsf_getsockname(INT s, NSF_SOCKADDR_IN *name, INT *namelen) asm("@@NSGSKN");

/* TERMAPI: close every socket of this application (RQ_CLOSE per live mapping
 * entry, then RQ_TERMAPI) and drop the registration. At the EZASOKET surface
 * TERMAPI has no RETCODE/ERRNO; the C API returns an int for diagnostics (0 ok),
 * which the facades discard. */
INT nsf_termapi(void) asm("@@NSTERM");

/* The ERRNO of the last failed C-API call on this address space (the BSD idiom).
 * Valid only after a call that returned -1. The facades store it into their
 * ERRNO slot. (Module-global in v1: not multi-subtask-safe -- see the header
 * comment / conformance doc.) */
INT nsf_lasterrno(void) asm("@@NSERNO");

/* The EZASOH03 plist decoder (ADR-0029). The HLASM facade EZASOH03 is a thin
 * veneer that passes its R1 plist straight here; this function decodes the
 * 4-char EBCDIC function code, marshals the plist parameters into the C API
 * above, and stores RETCODE/ERRNO back through the plist -- all in portable,
 * host-testable C. Always returns 0 (the EZASOH03 ABI: R15 is always 0; real
 * errors live in RETCODE/ERRNO). `plist` overlays the EZASOH03 argument list:
 * fullword A() slots on the target, natural pointers on the host. */
INT nsf_ezasoh03(void *plist) asm("@@NSOH03");

#endif /* NSFEZA_H */
