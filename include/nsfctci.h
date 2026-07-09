#ifndef NSFCTCI_H
#define NSFCTCI_H
/*
 * nsfctci.h -- the CTCI device driver (spec ch. 09.3; ADR-0019).
 *
 * NSFCTCI drives an emulated 3088 CTC read/write subchannel pair with plain
 * EXCP. It is split into two halves:
 *   - a HLASM top half (asm/nsfctcio.asm): the C-callable EXCP primitives that
 *     open a subchannel and start a READ/WRITE against it. There is NO
 *     I/O-completion exit and NO CHE appendage -- IOS POSTs the IOB ECB (the
 *     device ECB in the loop's ECBLIST) at channel-program termination
 *     (ADR-0019). The primitives operate on an opaque per-subchannel control
 *     block (CTCISC) the C layer supplies; ctci_scb_size() reports its size so
 *     the C layer allocates exactly enough from the NSF region.
 *   - a C bottom half (src/nsfctci.c): the lifecycle -- reserve region storage,
 *     SVC 99 allocate the CUU pair (via the libc370 seam), open both
 *     subchannels, then start/decode/close raw I/O. It knows nothing of the
 *     DEVOPS contract yet (that, plus the CTCIHDR/CTCISEG codec + PBUF
 *     conversion + sendq draining + MIH, is M1-4).
 *
 * M1-3 SCOPE: allocate + open the pair, EXCP a raw buffer each way, decode
 * completion. The CTCIHDR/CTCISEG codec and DEVOPS impl are M1-4; this header
 * only declares the WIRE FORMAT structs so a test can HAND-BUILD one block to
 * prove the channel path before the codec exists.
 *
 * ============  DEFERRED SEAM (blocked on hardware)  ============
 * The EXCP READ/WRITE path is UNVALIDATED on MVS: there is no CTCI device on
 * the Hercules side and the CUU pair is in no UCB yet. src/nsfctci.c + its asm
 * cross-link clean; their on-MVS runtime is owed a live run (see the PR
 * runbook). The one part proven under test-mvs today is the SVC 99 seam via a
 * deliberately-invalid unit (ctci_alloc_unit failing with a decoded S99 error).
 * ==============================================================
 */

#include "nsf.h"

/* --- Buffer / MTU bounds (spec 9.3) ---------------------------------------
 * A CTCI device I/O buffer is min(configured, 0xFFFF); 0xFFFF is only the CCW
 * count-field width, so the Hercules default 0x5000 (20 KB) is the binding
 * limit in practice. Each device owns TWO read buffers (ping-pong) + one write
 * buffer, obtained once at init and never freed. */
#define CTCI_BUF_DEFAULT   0x5000u    /* 20 KB (Hercules default)             */
#define CTCI_BUF_MIN       0x4000u    /* 16 KB                                */
#define CTCI_BUF_MAX       0xFFFFu    /* CCW count-field width (hard cap)     */
#define CTCI_MTU_MAX       9000u      /* Hercules discards frames above this  */

/* --- Wire format (spec 9.3; normative, verified vs Hercules ctc_ctci.c at
 * M1-1). All halfwords are big-endian = native S/370 order, so NSF reads and
 * builds them with no byte swapping. A block is [CTCIHDR] ([CTCISEG][IP]) ...;
 * blocks chain through hwOffset, the final block having hwOffset == 0. The
 * codec that walks/builds these is M1-4; the structs live here so the M1-3
 * test can craft one block by hand. */
typedef struct ctcihdr {
    USHORT hwOffset;             /* byte offset of NEXT block; 0x0000 = last  */
} CTCIHDR;                       /* 2 bytes */
NSF_SIZE_ASSERT(CTCIHDR, 2);

typedef struct ctciseg {
    USHORT hwLength;             /* segment length INCLUDING this 6-B header  */
    USHORT hwType;               /* frame type (CTCI_TYPE_IPV4)               */
    USHORT hwRsvd;               /* always 0x0000                             */
} CTCISEG;                       /* 6 bytes */
NSF_SIZE_ASSERT(CTCISEG, 6);

#define CTCI_TYPE_IPV4     0x0800u    /* CTCISEG.hwType for an IPv4 frame     */

/* Largest IP frame that fits one block of `buf` bytes (spec 9.3). */
#define CTCI_MAX_FRAME(buf) \
    ((UINT)(buf) - (UINT)sizeof(CTCIHDR) - (UINT)sizeof(CTCISEG) - 2u)

/* --- CTCI device state (NETDEV.priv points here in M1-4) -------------------
 * Allocated once at device init from the CTCIDEV pool (never freed). The two
 * subchannel control blocks (rscb/wscb) and the three I/O buffers come from
 * their own init-window pools; the two ECBs are IOS post targets, so they sit
 * here on natural (word) boundaries. 64 bytes on the S/370 target. */
#define CTCI_S_DOWN   0              /* allocated, not opened                  */
#define CTCI_S_UP     1              /* both subchannels opened                */

typedef struct ctcidev {
    USHORT  cuu;                 /*  0  read subchannel address                */
    USHORT  wcuu;               /*  2  write subchannel address (= cuu + 1)   */
    USHORT  mtu;                /*  4  configured interface MTU               */
    USHORT  state;              /*  6  CTCI_S_*                               */
    UINT    bufsize;            /*  8  bytes per I/O buffer                    */
    void   *rscb;               /* 12  read subchannel control block (CTCISC) */
    void   *wscb;               /* 16  write subchannel control block         */
    UCHAR  *rbuf0;              /* 20  ping-pong read buffer A                 */
    UCHAR  *rbuf1;              /* 24  ping-pong read buffer B                 */
    UCHAR  *wbuf;               /* 28  write buffer                            */
    UINT    cur;                /* 32  current read buffer (0/1); M1-4 uses it */
    UINT    recb;               /* 36  read  completion ECB (IOS post target) */
    UINT    wecb;               /* 40  write completion ECB (IOS post target) */
    char    rddn[9];            /* 44  read  subchannel DDNAME (NUL-term)      */
    char    wddn[9];            /* 53  write subchannel DDNAME (NUL-term)      */
    char    rsvd[2];            /* 62                                          */
} CTCIDEV;                       /* 64 bytes */
NSF_SIZE_ASSERT(CTCIDEV, 64);

/* asm() external-symbol aliases (CLAUDE.md §3). Two schemes, both unique
 * across the load module: the six HLASM top-half entries are NSFCI + verb
 * (their FUNHEAD entry names match character-for-character); the C bottom-half
 * functions are NSFCI + a distinct verb.
 *   asm:  ctci_scb_size NSFCISZ   ctci_open_sub NSFCIOPN   ctci_read NSFCIRD
 *         ctci_write NSFCIWR      ctci_status NSFCIST      ctci_close_sub NSFCICL
 *   C:    ctci_reserve NSFCIRSV   ctci_dev_open NSFCIOPD   ctci_dev_read NSFCIDRD
 *         ctci_dev_write NSFCIDWR ctci_dev_status NSFCIDST ctci_dev_close NSFCIDCL
 *         ctci_alloc_unit NSFCIALU  ctci_free_ddn NSFCIFDN  svc99_call NSFCISVC
 */

/* ============================ HLASM top half ============================== */

/* Bytes the C layer must allocate per subchannel control block (CTCISC). */
UINT ctci_scb_size(void) asm("NSFCISZ");

/* Copy the model DCB into scb, patch DDNAME, OPEN INPUT (forwrite==0) or OUTPUT
 * (forwrite!=0). Returns 0 if the DCB opened, non-zero otherwise. */
int  ctci_open_sub(void *scb, UINT forwrite, const char *ddname8) asm("NSFCIOPN");

/* Start an inbound READ (CCW X'02'+SLI) / outbound WRITE (CCW X'01') of `len`
 * bytes to/from `buf`; IOS POSTs *ecb at completion. Returns 0 (started). */
int  ctci_read (void *scb, void *buf, UINT len, UINT *ecb) asm("NSFCIRD");
int  ctci_write(void *scb, void *buf, UINT len, UINT *ecb) asm("NSFCIWR");

/* After *ecb posts: *postcode = IOB completion code (0x7F = normal),
 * *residual = CSW residual count (bytes NOT transferred). */
void ctci_status(void *scb, UINT *postcode, UINT *residual) asm("NSFCIST");

/* CLOSE a subchannel (direction-agnostic). Returns 0. */
int  ctci_close_sub(void *scb) asm("NSFCICL");

/* ============================ C bottom half =============================== */

/* Reserve NSFMM region storage for up to `ndev` CTCI devices with `bufsize`
 * I/O buffers. INIT-WINDOW ONLY (creates pools; call between mm_init and
 * mm_init_complete). Returns 0 on success, non-zero if a pool could not be
 * created. */
int      ctci_reserve(UINT ndev, UINT bufsize) asm("NSFCIRSV");

/* Bring up one CTCI device on the CUU pair (cuu = read, cuu+1 = write):
 * validate the MTU cap (mtu <= CTCI_MAX_FRAME(bufsize) and mtu <= CTCI_MTU_MAX),
 * SVC 99 allocate both subchannels, obtain the ping-pong read buffers + write
 * buffer, and open both subchannels. The I/O buffer size is the one fixed by
 * ctci_reserve. Returns the CTCIDEV*, or NULL on any failure (a NSF2xxE message
 * is emitted; nothing is left half-allocated). */
CTCIDEV *ctci_dev_open(USHORT cuu, USHORT mtu) asm("NSFCIOPD");

/* Start a raw READ into read buffer A (d->rbuf0); IOS posts d->recb. The
 * bottom half's re-drive-before-parse ping-pong is M1-4; here it is the single
 * explicit READ a caller drives. Returns 0 (started). */
int      ctci_dev_read(CTCIDEV *d) asm("NSFCIDRD");

/* Start a raw WRITE of `len` bytes from `buf` (typically d->wbuf); IOS posts
 * d->wecb. Returns 0 (started), non-zero if len exceeds the buffer. */
int      ctci_dev_write(CTCIDEV *d, const void *buf, UINT len) asm("NSFCIDWR");

/* Decode a completed request. iswrite selects the subchannel; reqlen is the
 * length that was requested. On return *len = reqlen - residual (bytes moved)
 * and *post = the completion code (0x7F normal). Returns 0 if *post == 0x7F,
 * non-zero otherwise. */
int      ctci_dev_status(CTCIDEV *d, int iswrite, UINT reqlen,
                         UINT *len, UCHAR *post) asm("NSFCIDST");

/* CLOSE both subchannels and unallocate both CUUs (SVC 99 S99VRBUN). */
int      ctci_dev_close(CTCIDEV *d) asm("NSFCIDCL");

/* --- Low-level SVC 99 seam (libc370), also the M1-3 test's proof-today path.
 * ctci_alloc_unit dynamically allocates the device named by the 4-char UNIT
 * `unit4` (e.g. "0E20"), asking the system to return a generated DDNAME into
 * `ddn8` (8 chars + NUL). Returns 0 on success; on failure returns non-zero and
 * fills *s99err / *s99info from the RB99 (S99ERROR / S99INFO). ctci_free_ddn
 * unallocates a DDNAME. Either s99err/s99info may be NULL. */
int      ctci_alloc_unit(const char *unit4, char *ddn8,
                         short *s99err, short *s99info) asm("NSFCIALU");
int      ctci_free_ddn(const char *ddn8) asm("NSFCIFDN");

/* The low-level RB99 wrapper behind ctci_alloc_unit / ctci_free_ddn, exported so
 * a test can drive SVC 99 directly -- both the failure AND the success paths --
 * over OUR request-block construction (not libc370's). `txt99` is a built
 * text-unit array (a libc370 `TXT99 **`, from svc99.h), passed opaquely as
 * `void *` so this header needs no libc370 type; svc99_call marks its last
 * entry, issues SVC 99 with `request` (S99VRBAL / S99VRBUN / ...), and on a
 * successful allocate copies the returned DDNAME into `ddn8` (8 chars + NUL). On
 * failure it fills *s99err / *s99info from the RB99. Returns the SVC 99 rc. */
int      svc99_call(void *txt99, unsigned char request, char *ddn8,
                    short *s99err, short *s99info) asm("NSFCISVC");

#endif /* NSFCTCI_H */
