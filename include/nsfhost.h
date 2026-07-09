#ifndef NSFHOST_H
#define NSFHOST_H
/*
 * nsfhost.h -- the HOST device driver (spec ch. 9.4).
 *
 * A DEVOPS driver implemented on the development host (Linux/macOS), so the
 * whole NSF stack runs and is unit-tested in CI with no MVS and no real
 * networking (spec 9.4: "this is what makes the full stack runnable in CI").
 * It moves RAW IP packets -- there is no CTCI framing here (that is CTCI-only,
 * M1-4); a host TUN device already presents raw IP.
 *
 * Modes (HOSTCFG.mode):
 *   NSFHOST_LOOPBACK  in-memory loopback (the default): a transmitted PBUF is
 *                     handed back inbound, so a full send -> receive cycle runs
 *                     with no OS networking. This is the unit-test path.
 *   NSFHOST_TUN       a real host TUN interface (Linux /dev/net/tun, macOS
 *                     utun) for live traffic. Compiled only with -DNSFHOST_TUN;
 *                     otherwise dev_register fails for this mode.
 *   NSFHOST_PCAP      reserved for a pcap capture source (not yet implemented).
 *
 * THE ASYNC PRODUCER (spec 9.3, host analog of the CTCI I/O-completion exit).
 * Inbound frames are delivered by a reader THREAD, not synchronously: it takes
 * a received PBUF, hands it to the device doneq (xq_push -- lock-free) and POSTs
 * the device ECB -- exactly the push+post the MVS CTCI exit will do (M1-3). The
 * executive loop then drains the doneq up to EV_PACKET_RECEIVED. So the whole
 * doneq -> EV_PACKET_RECEIVED integration is validated on the host now, and
 * M1-3 swaps only the producer. In loopback mode the reader relays the frames
 * the send side fed to an internal wire; in TUN mode it reads the tun fd.
 *
 * HOST-ONLY. This header describes an interface implemented by src/nsfhost.c
 * (host) and stubbed by src/nsfhost_plat.c (the MVS build, where nsfhost_ops
 * returns NULL: there is no host driver on MVS -- use CTCI/LCS). No host code
 * compiles into the MVS load modules and no MVS code into the host build
 * (spec 9.4).
 */

#include "nsf.h"
#include "nsfdev.h"             /* DEVOPS, DEVCFG */

/* HOSTCFG.mode values. */
#define NSFHOST_LOOPBACK  0
#define NSFHOST_TUN       1
#define NSFHOST_PCAP      2

/* Driver-specific configuration, pointed at by DEVCFG.drvcfg. Pass NULL for the
 * loopback default. */
typedef struct hostcfg {
    UCHAR  mode;                /* NSFHOST_LOOPBACK / _TUN / _PCAP */
    UCHAR  rsvd[3];
    char   ifname[16];         /* TUN interface name (mode TUN), NUL-padded */
} HOSTCFG;

/* asm() external-symbol alias (CLAUDE.md §3): the MVS placeholder
 * (src/nsfhost_plat.c) compiles this symbol into a cross-built test, so pin its
 * 8-char linker name (scheme NSFH):
 *   nsfhost_ops NSFHOPS
 */

/* The DEVOPS vtable for the host driver, to hand dev_register together with a
 * DEVCFG whose drvcfg points at a HOSTCFG (or NULL for loopback). Returns NULL
 * on a platform with no host driver (the MVS build): the caller then knows the
 * host driver is unavailable and does not register it. */
DEVOPS *nsfhost_ops(void) asm("NSFHOPS");

#endif /* NSFHOST_H */
