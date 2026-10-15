#ifndef NSFSTC_H
#define NSFSTC_H
/*
 * nsfstc.h -- STC startup sequencing and NSFCFG->component-init wiring (M0-8).
 *
 * The MVS main (src/nsfmain.c) loads a PROFILE.TCPIP member (NSFCFG, ch. 14) and
 * this module turns that immutable config into live component state, replacing
 * the hardcoded M0-2..M0-4 defaults (spec 14.4, "the M0-8 consumer"):
 *
 *   1. cross-statement referential integrity (LINK->DEVICE, HOME/GATEWAY->LINK)
 *      -- deliberately deferred here from the parser (spec 14.5);
 *   2. NSFTRACE flags -> nsftrc_flags;
 *   3. NSFPOOL counts -> the NSFBUF pool sizes (buf_init_counts);
 *   4. DEVICE/LINK/HOME/GATEWAY -> the interface/routing tables of NSFDEV/NSFIP
 *      -- those components do not exist until M1/M2, so the config is validated
 *      and remembered (nsf_active_cfg) but not yet fed downstream.
 *
 * All of this is PORTABLE C (no MVS services), so the mapping and the refuse-to-
 * start failure path are host-unit-tested (test/tststc.c); the MVS glue that
 * loads the member and drives the loop is the thin seam on top (src/nsfmain.c).
 */

#include "nsf.h"
#include "nsfcfg.h"

/* Referential-integrity / wiring error codes. Config range 700-799 (CLAUDE.md
 * §4), above the parser's 700-711 -- these are the M0-8 consumer's checks, not
 * the parser's. Rendered as "NSF7xxE ..." into the caller's errbuf. */
#define NSFSTC_OK          0
#define NSFSTC_E_LINKDEV 720   /* a LINK names an undefined DEVICE              */
#define NSFSTC_E_HOMELNK 721   /* a HOME names an undefined LINK                */
#define NSFSTC_E_GWLINK  722   /* a GATEWAY names an undefined LINK             */
#define NSFSTC_E_BUFPOOL 723   /* buffer pool creation failed (mm exhausted)   */

/* asm() external-symbol aliases (see CLAUDE.md §3, "External symbols"): every
 * cross-module function pins a unique 8-char linker name. Scheme NSFSC*
 * (distinct from NSFST* statistics):
 *   nsf_trace_flag NSFSCTRF   nsf_cfg_pool_count NSFSCPLC
 *   nsf_cfg_apply_trace NSFSCAPT   nsf_cfg_check_refs NSFSCCRF
 *   nsf_init_from_cfg NSFSCINI   nsf_active_cfg NSFSCCFG   nsf_shutdown NSFSCSHT
 *   nsf_recover_shutdown NSFSCRSD   nsf_set_io_guard NSFSCIOG
 */

/* Map a NSFTRACE / operator-TRACE component name ("IP","TCP",...,"ALL") to its
 * TRCF_* bit. Charset-transparent (literal compares only). 0 for an unknown
 * name. */
UINT nsf_trace_flag(const char *comp) asm("NSFSCTRF");

/* Resolve a pool's object count from the NSFPOOL statements, or `deflt` if the
 * config does not size it. Pure lookup; host-testable. */
UINT nsf_cfg_pool_count(const NSFCFG *cfg, const char *name, UINT deflt) asm("NSFSCPLC");

/* Apply the NSFTRACE statements into nsftrc_flags (ON -> enable, OFF -> disable).
 * Returns the number of statements applied (an unknown component name is skipped
 * -- the parse already accepted it as text). */
UINT nsf_cfg_apply_trace(const NSFCFG *cfg) asm("NSFSCAPT");

/* Cross-statement referential integrity (spec 14.5). Returns NSFSTC_OK if every
 * LINK names a defined DEVICE and every HOME/GATEWAY names a defined LINK, else
 * an NSFSTC_E_* code with an "NSF7xxE ..." line rendered into errbuf (naming the
 * offending statement). errbuf may be NULL (then only the code is returned). */
INT nsf_cfg_check_refs(const NSFCFG *cfg, char *errbuf, UINT errsize) asm("NSFSCCRF");

/* Wire a validated NSFCFG into the component inits. INIT-WINDOW ONLY (it calls
 * buf_init_counts -> mm_pool_create): run it after nsfevt_init and before
 * mm_init_complete. On success returns 0 and remembers cfg as the active config;
 * on any error returns an NSFSTC_E_* code and renders "NSF7xxE ..." into errbuf,
 * having made no partial state the caller must unwind past mm_shutdown. */
INT nsf_init_from_cfg(const NSFCFG *cfg, char *errbuf, UINT errsize) asm("NSFSCINI");

/* The config remembered by the last successful nsf_init_from_cfg, or NULL before
 * one. The operator DISPLAY reads it. */
const NSFCFG *nsf_active_cfg(void) asm("NSFSCCFG");

/* The device-I/O guard (#83).  `busy` answers how many devices may still have
 * I/O outstanding on pool storage (dev_busy_count); NULL removes the guard.
 * A function pointer and not a call into NSFDEV, so this component and its
 * tests stay free of the device layer.  The STC mains install it before
 * starting any device. */
typedef UINT (*nsf_io_busy_fn)(void);
void nsf_set_io_guard(nsf_io_busy_fn busy) asm("NSFSCIOG");

/* Orderly teardown (spec 17.1): disarm the platform timer, then release every
 * pool region (mm_shutdown) -- UNLESS the I/O guard reports a busy device.
 *
 * Returns 0 when the regions were released, or the number of busy devices, in
 * which case NONE was released.  A FREEMAIN of a region holding a buffer that
 * an outstanding EXCP has page-fixed abends A0A-2 ("PART OF AREA BEING FREED IS
 * STILL FIXED", IEAVGM00), so a region under live I/O is left to MVS, which
 * reclaims the private area at address-space end.  The caller reports a
 * non-zero return (NSF068W): skipping the free silently would read the same as
 * having done it (CLAUDE.md 8.5). */
UINT nsf_shutdown(void) asm("NSFSCSHT");

/* Recovery teardown, for the ESTAE exit only (#83): disarm the platform timer
 * and forget the active config, and release NOTHING.  The exit runs while the
 * device subtasks may still have an EXCP outstanding on pool storage, and the
 * pool regions are private-area storage MVS reclaims anyway -- the line #79's
 * audit draws: recovery releases only what nobody else will. */
void nsf_recover_shutdown(void) asm("NSFSCRSD");

#endif /* NSFSTC_H */
