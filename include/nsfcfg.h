#ifndef NSFCFG_H
#define NSFCFG_H
/*
 * nsfcfg.h -- the Configuration parser (spec ch. 14).
 *
 * Parses a PROFILE.TCPIP-compatible member at startup into one immutable,
 * fixed-size NSFCFG structure (spec 14.3). Validation is all-or-nothing: the
 * parser validates the whole member and rejects on ANY error, reporting an
 * NSF7xxE message with the 1-based line number (spec 14.1). There are no
 * partial configs and no silent defaults papering over a typo -- an unknown
 * KEYWORD inside a known statement is an error, and an unknown STATEMENT is an
 * error unless it is on the small explicit ignorable list (then: a counted
 * warning, and parsing continues).
 *
 * CHARSET TRANSPARENCY (the load-bearing property, spec 15.3). The parser
 * compares character and string *literals* only ('.', ';', "DEVICE", ...), so
 * the compiler emits EBCDIC under cc370 and ASCII on the host and the SAME
 * source parses a real EBCDIC PDS member and the ASCII host corpus. It never
 * hardcodes a byte value (no 0xC4 for 'D') and never assumes letters collate
 * contiguously: case folding and hex/decimal digit tests go through the small
 * cfg_* char helpers in nsfcfg.c, which are correct on both code pages because
 * they only lean on the sub-ranges that ARE contiguous in EBCDIC (0-9, A-I,
 * a-i -- hence A-F/a-f for hex, and a block-wise upcase). Payload is never
 * converted; only NSF's own config text is (spec 15.3).
 *
 * OUTPUT. NSFCFG is a single fixed-size struct of bounded arrays (named
 * maxima below), the TCP/UDPCONFIG scalars, the NSFPOOL pool sizes and the
 * NSFTRACE flags. No runtime allocation; read-only after a successful parse
 * (no locking, no reload in v1 -- operator VARY is Phase 2+). The M0-8 STC
 * startup is the first consumer: it feeds pool sizes to NSFMM/NSFBUF, trace
 * flags to NSFTRC, and the interface/routing tables to NSFDEV/NSFIP. Design
 * the consumers to read from here; do not add fields they would need without
 * an ADR. Cross-statement referential integrity (a LINK naming a defined
 * DEVICE, a HOME/GATEWAY naming a defined LINK) is deliberately NOT validated
 * here: spec 14.2 is silent on the ordering/reference rules it would require,
 * so that check belongs to the M0-8 consumer (or a future ADR), not the parser.
 *
 * IPv4 ENCODING. Every parsed address/mask/net/hop is a UINT with the FIRST
 * dotted octet in the most-significant byte: 10.1.2.3 -> 0x0A010203. This is
 * the natural big-endian S/370 order; consumers use it directly as a 32-bit
 * network address value.
 */

#include "nsf.h"

/* -- bounded array maxima (spec 14.3: fixed footprint, computable up front) -- *
 * Sized generously for a hobbyist stack yet tiny in bytes; array overflow is a
 * rejected config (NSF706E), never a crash. */
#define NSFCFG_MAX_DEVICES   4
#define NSFCFG_MAX_LINKS     4
#define NSFCFG_MAX_HOMES     8
#define NSFCFG_MAX_GATEWAYS  8
#define NSFCFG_MAX_PORTS    16
#define NSFCFG_MAX_POOLS     8
#define NSFCFG_MAX_TRACES    8

/* -- fixed field widths (NUL-padded; a name that exactly fills its field has
 * no NUL, and every read is length-bounded, mirroring nsfsts.c) -- */
#define NSFCFG_NAMELEN      16   /* DEVICE / LINK / interface name        */
#define NSFCFG_JOBLEN        8   /* MVS jobname (PORT reservation)        */
#define NSFCFG_POOLLEN       8   /* NSFPOOL pool name                     */
#define NSFCFG_COMPLEN       8   /* NSFTRACE component name               */
#define NSFCFG_MSGLEN       72   /* error message text incl. "NSF7xxE "   */

/* -- device / protocol type codes (stored, not the source keyword) -- */
#define NSFCFG_DEV_CTC       1   /* DEVICE ... CTC (the only v1 type)     */
#define NSFCFG_PROTO_TCP     1   /* PORT ... TCP                          */
#define NSFCFG_PROTO_UDP     2   /* PORT ... UDP                          */

/* -- parse result codes: 0 == success, else the NSF7xx number that is also
 * rendered into NSFCFGERR.msg. Consumers switch on NSFCFGERR.code; tests
 * assert on it for precision. Warnings (ignorable statement skipped) are
 * counted in NSFCFG.nwarn and never set an error. -- */
#define NSFCFG_OK            0
#define NSFCFG_E_SYNTAX    700   /* malformed statement / wrong operand count */
#define NSFCFG_E_IPADDR    701   /* malformed dotted-decimal IPv4 address     */
#define NSFCFG_E_MASK      702   /* malformed subnet mask                     */
#define NSFCFG_E_CUU       703   /* malformed device address (hex cuu)        */
#define NSFCFG_E_RANGE     704   /* numeric value out of range                */
#define NSFCFG_E_DUP       705   /* duplicate statement / name                */
#define NSFCFG_E_OVERFLOW  706   /* too many of a bounded statement           */
#define NSFCFG_E_MISSING   707   /* missing required statement (HOME)         */
#define NSFCFG_E_STMT      708   /* unknown, non-ignorable statement          */
#define NSFCFG_E_KEYWORD   709   /* unknown keyword within a known statement  */
#define NSFCFG_E_TOOBIG    710   /* member larger than the load buffer        */
#define NSFCFG_E_OPEN      711   /* cfg_load could not open the member        */
/* Ignorable statements are counted in NSFCFG.nwarn (below) rather than carrying
 * a code here; the operator WARNING message (NSF7xxW) is wired at M0-8 when the
 * WTO seam exists. */

/* Error report from the most recent parse. On success line == 0 and code == 0.
 * On failure code is the NSF7xx number, line is the 1-based physical line (or 0
 * for a whole-config error such as a missing HOME), and msg is the rendered
 * "NSF7xxE ..." text. */
typedef struct nsfcfgerr {
    UINT line;                       /*  4  1-based line, 0 if not line-specific */
    UINT code;                       /*  4  NSF7xx code, 0 on success            */
    char msg[NSFCFG_MSGLEN];         /* 72  "NSF7xxE ...", NUL-terminated        */
} NSFCFGERR;                         /* 80 bytes */
NSF_SIZE_ASSERT(NSFCFGERR, 80);

/* DEVICE devname CTC cuu */
typedef struct nsfcfgdev {
    char   name[NSFCFG_NAMELEN];     /* 16  device name                          */
    USHORT cuu;                      /*  2  hardware address (hex cuu)            */
    UCHAR  type;                     /*  1  NSFCFG_DEV_*                          */
    UCHAR  rsvd;                     /*  1  pad                                   */
} NSFCFGDEV;                         /* 20 bytes */
NSF_SIZE_ASSERT(NSFCFGDEV, 20);

/* LINK link CTC 0 devname */
typedef struct nsfcfglink {
    char   name[NSFCFG_NAMELEN];     /* 16  link name                            */
    char   devname[NSFCFG_NAMELEN];  /* 16  device this link binds to            */
    UCHAR  type;                     /*  1  NSFCFG_DEV_*                          */
    UCHAR  rsvd[3];                  /*  3  pad                                   */
} NSFCFGLINK;                        /* 36 bytes */
NSF_SIZE_ASSERT(NSFCFGLINK, 36);

/* HOME ipaddr link */
typedef struct nsfcfghome {
    UINT   ip;                       /*  4  home IPv4 address                     */
    char   link[NSFCFG_NAMELEN];     /* 16  link this address is bound to         */
} NSFCFGHOME;                        /* 20 bytes */
NSF_SIZE_ASSERT(NSFCFGHOME, 20);

/* GATEWAY (net|DEFAULTNET) firsthop link mtu (mask|0) */
typedef struct nsfcfggw {
    UINT   net;                      /*  4  destination network (0 if default)    */
    UINT   mask;                     /*  4  subnet mask (0 == none / default)     */
    UINT   firsthop;                 /*  4  next-hop IPv4 address                 */
    char   link[NSFCFG_NAMELEN];     /* 16  outbound link                         */
    USHORT mtu;                      /*  2  path MTU                              */
    UCHAR  is_default;               /*  1  1 for DEFAULTNET                      */
    UCHAR  rsvd;                     /*  1  pad                                   */
} NSFCFGGW;                          /* 32 bytes */
NSF_SIZE_ASSERT(NSFCFGGW, 32);

/* PORT port (TCP|UDP) jobname */
typedef struct nsfcfgport {
    USHORT port;                     /*  2  reserved port number                  */
    UCHAR  proto;                    /*  1  NSFCFG_PROTO_*                         */
    UCHAR  rsvd;                     /*  1  pad                                   */
    char   jobname[NSFCFG_JOBLEN];   /*  8  owning jobname                        */
} NSFCFGPORT;                        /* 12 bytes */
NSF_SIZE_ASSERT(NSFCFGPORT, 12);

/* NSFPOOL poolname count  (NSF extension: pool sizing, spec 14.2) */
typedef struct nsfcfgpool {
    char   name[NSFCFG_POOLLEN];     /*  8  pool name (matches an NSFMM pool)     */
    UINT   count;                    /*  4  object count requested                */
} NSFCFGPOOL;                        /* 12 bytes */
NSF_SIZE_ASSERT(NSFCFGPOOL, 12);

/* NSFTRACE comp ON|OFF  (NSF extension: startup trace, spec 14.2) */
typedef struct nsfcfgtrace {
    char   comp[NSFCFG_COMPLEN];     /*  8  component name                        */
    UCHAR  on;                       /*  1  1 == ON, 0 == OFF                      */
    UCHAR  rsvd[3];                  /*  3  pad                                   */
} NSFCFGTRACE;                       /* 12 bytes */
NSF_SIZE_ASSERT(NSFCFGTRACE, 12);

/* TCPCONFIG scalars. v1 keyword set: RECVBUFRSIZE, SENDBUFRSIZE (both a keyword
 * + a numeric value). KEEPALIVEOPTIONS (a block form in IBM syntax) is DEFERRED
 * -- until implemented it is rejected as an unknown keyword (NSF709E), so the
 * illustrative §14.2 line is not silently accepted. A size of 0 means "unset;
 * the component picks its default". */
typedef struct nsfcfgtcp {
    UINT   recvbufsize;              /*  4  RECVBUFRSIZE, 0 == default             */
    UINT   sendbufsize;              /*  4  SENDBUFRSIZE, 0 == default             */
    UCHAR  present;                  /*  1  a TCPCONFIG statement was seen         */
    UCHAR  rsvd[3];                  /*  3  pad                                   */
} NSFCFGTCP;                         /* 12 bytes */
NSF_SIZE_ASSERT(NSFCFGTCP, 12);

/* UDPCONFIG scalars. v1 keyword set: UDPCHKSUM / NOUDPCHKSUM. chksum defaults
 * ON (IBM default), so a config with no UDPCONFIG leaves chksum == 1. */
typedef struct nsfcfgudp {
    UCHAR  chksum;                   /*  1  1 == UDPCHKSUM (default), 0 == off     */
    UCHAR  present;                  /*  1  a UDPCONFIG statement was seen         */
    UCHAR  rsvd[2];                  /*  2  pad                                   */
} NSFCFGUDP;                         /* 4 bytes */
NSF_SIZE_ASSERT(NSFCFGUDP, 4);

/* The whole configuration. Fixed size, no pointers, no allocation. The eye
 * eyecatcher "NSFCFG  " is stamped ONLY on a successful parse (spec 14.1: no
 * partial configs) -- on any error the body is zeroed, only err is meaningful,
 * and a caller that (incorrectly) ignores the return code finds no eyecatcher
 * and cannot consume a half-built config. */
typedef struct nsfcfg {
    char        eye[8];              /*   8  "NSFCFG  " on success, else zero      */
    NSFCFGERR   err;                 /*  80  last-parse error report               */
    UINT        ndev;               /* number of DEVICE entries                    */
    UINT        nlink;              /* number of LINK entries                      */
    UINT        nhome;              /* number of HOME entries                      */
    UINT        ngw;                /* number of GATEWAY entries                   */
    UINT        nport;             /* number of PORT entries                      */
    UINT        npool;             /* number of NSFPOOL entries                   */
    UINT        ntrace;            /* number of NSFTRACE entries                  */
    UINT        nwarn;             /* ignorable statements skipped with a warning */
    NSFCFGDEV   dev[NSFCFG_MAX_DEVICES];
    NSFCFGLINK  link[NSFCFG_MAX_LINKS];
    NSFCFGHOME  home[NSFCFG_MAX_HOMES];
    NSFCFGGW    gw[NSFCFG_MAX_GATEWAYS];
    NSFCFGPORT  port[NSFCFG_MAX_PORTS];
    NSFCFGPOOL  pool[NSFCFG_MAX_POOLS];
    NSFCFGTRACE trace[NSFCFG_MAX_TRACES];
    NSFCFGTCP   tcp;
    NSFCFGUDP   udp;
} NSFCFG;                            /* 1160 bytes (pointer-free, host==target) */
NSF_SIZE_ASSERT(NSFCFG, 1160);

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module cfg_* pins a unique 8-char linker name so cc370's 8-char
 * external truncation (upcased, '_' -> '@') can never fold two into one on MVS.
 * Only these two functions are non-static; every parser helper is file-static.
 * Scheme NSFCF + verb:
 *   cfg_parse NSFCFPRS   cfg_load NSFCFLDR
 */

/* Parse `len` bytes of PROFILE.TCPIP text at `buf` into `out`. PURE C over a
 * caller-supplied buffer -- no I/O, fully host-testable, charset-transparent.
 * Returns 0 on success (out fully populated, eyecatcher stamped) or the NSF7xx
 * code on failure (out zeroed except out->err, which carries the code, the
 * 1-based line and the rendered message). Never allocates, never abends. */
INT  cfg_parse(const char *buf, UINT len, NSFCFG *out) asm("NSFCFPRS");

/* Thin wrapper: fopen/fread `name` (a host path, or an MVS DDNAME / dataset
 * member spec understood by libc370's fopen) into a fixed init-window buffer,
 * then cfg_parse it. Returns cfg_parse's result, or NSF711E if the member
 * cannot be opened, or NSF710E if it exceeds the load buffer. The buffer is a
 * file-scope BSS block used only during init (favouring a low stack footprint
 * over a large stack frame on the executive task); it is never a hot path. */
INT  cfg_load(const char *name, NSFCFG *out) asm("NSFCFLDR");

#endif /* NSFCFG_H */
