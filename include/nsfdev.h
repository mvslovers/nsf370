#ifndef NSFDEV_H
#define NSFDEV_H
/*
 * nsfdev.h -- the Device Abstraction (spec ch. 09).
 *
 * NSFDEV defines the driver contract (DEVOPS) and owns the device table. A
 * driver moves RAW frames between some transport (MVS channel I/O, or a host
 * TUN/loopback) and PBUFs; it knows nothing above IP, and IP knows nothing
 * below the NETDEV. Concrete drivers implement DEVOPS: NSFCTCI (HLASM top /
 * C bottom, M1-3/M1-4) and NSFHOST (host-only loopback/TUN, M1-2, see
 * nsfhost.h). The abstraction is DRIVER-AGNOSTIC: the executive main loop
 * (NSFEVT) drives every device only through NSFDEV, never naming a concrete
 * driver -- so the whole stack runs in CI over the host loopback before any
 * Hercules device exists.
 *
 * THE ASYNC COMPLETION MODEL (spec 5.3, 9.3). Inbound frames arrive on an
 * asynchronous producer -- the MVS CTCI I/O-completion exit, or (on the host)
 * the NSFHOST reader thread. That producer does the absolute minimum: it hands
 * a received PBUF to the device's `doneq` (an interrupt-safe NSFXQ handoff:
 * xq_push, lock-free) and POSTs the device `ecb`. Everything real happens later
 * on the executive task: the loop WAITs on the device ECB(s), and each pass
 * nsfdev_poll_input() drains every doneq and dispatches the PBUFs up as
 * EV_PACKET_RECEIVED. Because the producer only pushes + posts, M1-3 swaps the
 * host reader thread for the real CTCI exit with no change above this seam.
 *
 * OWNERSHIP (spec 9.2, single-owner PBUFs, ADR-0008). dev_send() takes the
 * PBUF UNCONDITIONALLY: either it is queued (ownership passes to the device) or
 * it is dropped on an immediate error (the send path frees it and counts). An
 * inbound PBUF handed up as EV_PACKET_RECEIVED is owned by the handler (IP
 * input, M2), which frees it. No PBUF is ever owned twice or neither.
 */

#include "nsf.h"
#include "nsfque.h"             /* QUEUE sendq (bounded outbound)          */
#include "nsfxq.h"              /* XQ doneq (exit->mainline completed I/O) */
#include "nsfbuf.h"             /* PBUF                                      */
#include "nsfsts.h"             /* STSCTR (per-device counters)             */
#include "nsfevtp.h"            /* NSFECB (the device ECB type)             */

/* Forward: the per-device control block, defined below. */
typedef struct netdev NETDEV;

/* Driver configuration passed to DEVOPS.init (spec 9.2). Transient init-time
 * data, not a pooled object -- so no NSF_SIZE_ASSERT. The common interface
 * fields come from PROFILE (DEVICE/LINK/HOME); `drvcfg` carries driver-private
 * extras (NSFHOST points it at a HOSTCFG selecting loopback/TUN, see
 * nsfhost.h). CTCI would point it at its buffer-size / CUU-pair extras. */
typedef struct devcfg {
    char    name[8];            /* LINK name from PROFILE (NUL/blank padded) */
    USHORT  cuu;                /* device address (read subchannel for CTCI) */
    UCHAR   type;               /* NSFDEV_T_*                                */
    UCHAR   rsvd;
    UINT    ipaddr;             /* HOME address, network order               */
    USHORT  mtu;                /* interface MTU (0 => driver default)       */
    USHORT  flags;              /* reserved                                  */
    void   *drvcfg;             /* driver-specific config, or NULL           */
} DEVCFG;

/* The driver contract (spec 9.2). Exactly four ops; adding a fifth would break
 * the normative contract (an LCS/ARP driver must implement only these -- §9.5).
 *   init     -- one-time setup of the driver private block from cfg.
 *   start    -- bring the device up; drive the first READ / start the reader.
 *   send     -- transmit one PBUF; TAKES OWNERSHIP of b unconditionally.
 *   shutdown -- halt I/O, stop the producer, release driver-private storage. */
typedef struct devops {
    int (*init)    (NETDEV *dev, const DEVCFG *cfg);
    int (*start)   (NETDEV *dev);
    int (*send)    (NETDEV *dev, PBUF *b);
    int (*shutdown)(NETDEV *dev);
} DEVOPS;

/* NETDEV type codes (NETDEV.type). */
#define NSFDEV_T_CTCI   1
#define NSFDEV_T_LCS    2
#define NSFDEV_T_HOST   3

/* NETDEV lifecycle states (NETDEV.state). DOWN must be 0 so a zeroed table slot
 * reads as an unused (down) device. */
#define NSFDEV_S_DOWN       0   /* registered, not started (or stopped)      */
#define NSFDEV_S_STARTING   1   /* dev_start in progress                     */
#define NSFDEV_S_UP         2   /* started; sends transmit, reads run        */
#define NSFDEV_S_QUIESCING  3   /* dev_shutdown in progress                  */

/* Bound of the outbound queue (spec 9.2 "bounded outbound queue"; §3 invariant
 * "queues are bounded by default -- reject rather than grow"). A modest depth:
 * on backpressure dev_send rejects (frees + counts) rather than growing. */
#define NSFDEV_SENDQ_MAX    32

/* Device-table capacity. The table is a fixed BSS array (no runtime alloc, §3):
 * a real system carries a handful of interfaces. */
#define NSFDEV_MAX          8

/* The per-device control block (spec 9.2). Lives in the NSFDEV static table,
 * NOT an NSFMM pool -- it holds pointers and is never allocated at runtime.
 * 64 bytes on the S/370 target (4-byte pointers); the layout is packed with no
 * padding (every field is naturally aligned, max alignment 4). The size assert
 * is enforced only under cc370 (__MVS__); a host build has 8-byte pointers so
 * the target-size check is a no-op there (see NSF_SIZE_ASSERT). */
struct netdev {
    char     name[8];           /*  0  LINK name (NUL/blank padded)          */
    DEVOPS  *ops;               /*  8  driver operations                     */
    USHORT   cuu;               /* 12  device address                        */
    UCHAR    type;              /* 14  NSFDEV_T_*                            */
    UCHAR    state;             /* 15  NSFDEV_S_*                            */
    UINT     ipaddr;            /* 16  HOME address (network order)          */
    USHORT   mtu;               /* 20  interface MTU                         */
    USHORT   flags;             /* 22  reserved                              */
    QUEUE    sendq;             /* 24  bounded outbound queue (12 B)         */
    XQ       doneq;             /* 36  exit->mainline completed inbound I/O  */
    UINT     ecb;               /* 40  device ECB (joined to the main list)  */
    STSCTR  *ctr_in;            /* 44  frames delivered up (EV_PACKET_RECV)  */
    STSCTR  *ctr_out;           /* 48  frames transmitted                    */
    STSCTR  *ctr_ierr;          /* 52  inbound drops (no EVT / no PBUF)      */
    STSCTR  *ctr_oerr;          /* 56  outbound drops (queue full / tx err)  */
    void    *priv;              /* 60  driver private block                  */
};                              /* 64 bytes */
NSF_SIZE_ASSERT(NETDEV, 64);

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"):
 * cc370/ld370 fold an external name to 8 characters after upcasing and mapping
 * '_' -> '@', so every cross-module NSFDEV function pins a unique 8-char linker
 * name (scheme NSFD + verb) so no two collide on MVS:
 *   dev_init NSFDINIT   dev_register NSFDREG   dev_find NSFDFND
 *   dev_find_cuu NSFDFCU   dev_by_index NSFDBYIX   dev_foreach NSFDFOR
 *   dev_count NSFDCNT   dev_start NSFDSTRT   dev_send NSFDSEND
 *   dev_shutdown NSFDSHUT   nsfdev_collect_ecbs NSFDECBS
 *   nsfdev_poll_input NSFDPOLL   nsfdev_kick_output NSFDKICK
 */

/* Reset the device table to empty. Call once before registering devices (the
 * STC startup, or a test setup). Safe to re-call (it re-zeroes the table); it
 * does NOT quiesce live devices -- shut them down first. */
void    dev_init(void) asm("NSFDINIT");

/* Register a device from cfg with the driver ops. Claims a free table slot,
 * initializes the common fields (state = DOWN, empty bounded sendq, empty
 * doneq, cleared ecb), registers the four per-device counters, and calls
 * ops->init(dev, cfg). Returns the NETDEV*, or NULL if the table is full or the
 * driver's init failed (the slot is released on init failure). Also wires the
 * device seam into the NSFEVT loop on the first registration (evt_set_devices),
 * so the loop begins servicing device ECBs. */
NETDEV *dev_register(const DEVCFG *cfg, DEVOPS *ops) asm("NSFDREG");

/* Find a registered device by LINK name (exact, up to 8 chars), or NULL. */
NETDEV *dev_find(const char *name) asm("NSFDFND");

/* Find a registered device by device address (cuu), or NULL. */
NETDEV *dev_find_cuu(USHORT cuu) asm("NSFDFCU");

/* The device in table slot `idx` (the EV_PACKET_RECEIVED u1 carries this
 * index), or NULL if idx is out of range or the slot is unused. */
NETDEV *dev_by_index(UINT idx) asm("NSFDBYIX");

/* Call fn(dev, arg) for each registered device, in slot order. */
void    dev_foreach(void (*fn)(NETDEV *dev, void *arg), void *arg) asm("NSFDFOR");

/* Number of registered devices. */
UINT    dev_count(void) asm("NSFDCNT");

/* Bring a device up: state DOWN -> STARTING -> ops->start -> UP. Returns 0 on
 * success (state UP), non-zero on failure (state left DOWN). */
int     dev_start(NETDEV *dev) asm("NSFDSTRT");

/* Queue a PBUF for transmission. TAKES OWNERSHIP of b unconditionally: on
 * success b is on the bounded sendq (transmitted later by nsfdev_kick_output);
 * on any immediate rejection (device not UP, or sendq full) b is freed and
 * ctr_oerr counted. Returns 0 if queued, non-zero if dropped. Wakes the loop so
 * a pending send is kicked even when issued from outside the loop. */
int     dev_send(NETDEV *dev, PBUF *b) asm("NSFDSEND");

/* Quiesce a device: state -> QUIESCING -> ops->shutdown (stop the producer,
 * release driver storage) -> drain the sendq and doneq (freeing any PBUFs still
 * held) -> state DOWN. After this the device's queues hold no PBUFs (leak
 * gate). Idempotent on an already-DOWN device. */
int     dev_shutdown(NETDEV *dev) asm("NSFDSHUT");

/* --- NSFEVT main-loop integration (spec 5.3) ---------------------------------
 * These are called only by the executive loop, through the function pointers it
 * received via evt_set_devices (registered lazily by the first dev_register).
 * They keep NSFEVT decoupled from NSFDEV (no direct symbol dependency), exactly
 * as evt_set_operator decouples the operator seam. */

/* Append the device ECB pointers to the loop's ECBLIST (up to `max`), returning
 * the number appended. The loop WAITs on these so a device completion wakes it.
 * Devices registered after the loop started are not added (the loop builds its
 * list once at entry); M1-2 registers all devices before evt_mainloop. */
int     nsfdev_collect_ecbs(NSFECB **list, int max) asm("NSFDECBS");

/* Drain every device's doneq and post each inbound PBUF up as
 * EV_PACKET_RECEIVED (p1 = PBUF*, u1 = device index). Runs once per loop pass,
 * before dispatch, so freshly received frames are handled the same pass. The
 * device ECB is cleared before its doneq is drained (lost-wakeup safe: a
 * producer that pushes+posts in the race window leaves the ECB set, so the next
 * WAIT returns). On EVT-pool exhaustion the PBUF is freed and ctr_ierr counted
 * (drop + count -- never an ABEND). */
void    nsfdev_poll_input(void) asm("NSFDPOLL");

/* Start pending output: drain each UP device's sendq through ops->send. Runs at
 * §5.3 step 5. ctr_out counts a transmitted frame; an immediate ops->send error
 * has already freed the PBUF and counted ctr_oerr (send-ownership contract). */
void    nsfdev_kick_output(void) asm("NSFDKICK");

#endif /* NSFDEV_H */
