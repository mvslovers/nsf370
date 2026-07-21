/*
 * nsfpssir.c -- M5 Stage-0a SSI thin router (NSFPSSIR).
 *
 * ADR-0036.  The SSVT function routine MVS invokes, via IEFSSREQ (branch entry),
 * for the NSFP probe subsystem.  It runs in the CALLER's address space, loaded
 * into CSA (SP=241) via __loadhi from the STC steplib.  It contains ZERO
 * business logic: it round-trips a single 32-bit token across the address-space
 * boundary to the STC and back.  No NSFRQE, no socket, no protocol.
 *
 * Built RENT (no writable statics -- all state on the stack or in CSA),
 * entry=NSFPSSIR, startup=false (project.toml): MVS enters this function
 * directly, NOT through the C runtime.
 *
 * Calling context set by MVS before invoking the SSVT routine:
 *   - supervisor state, PSW key 8 (IEFSSREQ does MODESET MODE=SUP, not KEY=ZERO)
 *   - R1 = address of the SSOB (a RAW pointer, NOT a C parameter list)
 *   - running in the CLIENT address space
 *
 * The R1=SSOB convention is incompatible with the C calling convention (which
 * expects R1 = plist).  nsfpssir() is declared void() and extracts the SSOB via
 * inline asm before the compiler emits any plist dereference -- else UFSD abend
 * #2 (S0C4).  Every state/key move below is inherited verbatim from
 * ufsd/src/ufsd#ssi.c; see ufsd/docs/cross-as-reference.md for the five abends.
 */
#include "nsfpssi.h"
#include <string.h>
#include <clibos.h>          /* __super/__prob/__ascb/__xmpost/__uinc/__udec    */
#include <clibecb.h>         /* ecb_timed_wait, ECB_VALUE_MASK                  */
#include <clibssct.h>        /* ssct_find                                       */
#include <iefssobh.h>        /* SSOB                                            */

void
nsfpssir(void)
{
    SSOB          *ssob;
    SSCT          *ssct;
    NSFP_ANCHOR   *anchor;
    NSFPSSOB      *ext;
    ECB            local_ecb;    /* key-8 stack ECB: the cross-AS reply target  */
    ECB           *ecbp;
    unsigned char  savekey;

    /* IEFSSREQ passes R1 = SSOB; capture it before any C code runs. */
    __asm__ __volatile__("LR %0,1" : "=r"(ssob));

    /* --- Validate the SSOB pointer and our extension --- */
    if (!ssob) return;

    ext = (NSFPSSOB *)ssob->SSOBINDV;
    if (!ext) {
        ssob->SSOBRETN = NSFP_RC_INVALID;
        return;
    }
    if (memcmp(ext->eye, NSFPSSOB_EYE, 4) != 0) {
        /* Not our request (e.g. an MVS system-internal SSI call routed here):
        ** reject cleanly without touching CSA. */
        ssob->SSOBRETN = NSFP_RC_INVALID;
        return;
    }

    /* --- Locate the CSA anchor via the SSCT --- */
    ssct = ssct_find(NSFP_SUBSYS);
    if (!ssct) {
        ssob->SSOBRETN = NSFP_RC_CORRUPT;
        return;
    }
    anchor = (NSFP_ANCHOR *)ssct->ssctsuse;
    if (!anchor || memcmp(anchor->eye, "NSFPANCR", 8) != 0) {
        ssob->SSOBRETN = NSFP_RC_CORRUPT;
        return;
    }
    if (!(anchor->flags & NSFP_ANCHOR_ACTIVE)) {
        ssob->SSOBRETN = NSFP_RC_CORRUPT;
        return;
    }

    /* --- Key-0 window for CSA writes + the cross-AS POST ---
    ** IEFSSREQ arrives here in supervisor state, PSW key 8.  The anchor is in
    ** CSA (SP=241, key 0); a store to key-0 storage from key 8 is a protection
    ** exception.  __super(PSWKEY0) switches to key 0 (the caller task is
    ** APF-authorised via clib_apf_setup, so MODESET KEY=ZERO succeeds).  The
    ** __xmpost that wakes the STC must issue from supervisor state (SVC 2
    ** cross-AS from problem state -> S102); the WAIT below must issue from
    ** problem state on a key-8 ECB (key-0 CSA ECB from problem state -> X'201').
    */
    if (__super(PSWKEY0, &savekey)) {
        ssob->SSOBRETN = NSFP_RC_CORRUPT;
        return;
    }

    /* --- Take the single request slot ---
    ** Single-task, sequential probe client: no free pool / CAS is needed.  A
    ** second concurrent client (which the probe never issues) is rejected
    ** rather than allowed to corrupt the slot. */
    if (anchor->req_state != NSFP_REQ_FREE) {
        ssob->SSOBRETN = NSFP_RC_NOREQ;
        __prob(savekey, NULL);
        return;
    }

    /* Mark ourselves in-flight: shutdown clears ANCHOR_ACTIVE and drains this
    ** to zero before it frees the router + CSA.  Incremented here (in the entry
    ** key-0 window) and decremented on every path out of the router below. */
    __uinc(&anchor->inflight);

    /* Stage the request.  Publish req_state = PENDING LAST, after every other
    ** field is set, so an STC wake for ANY reason never services a half-formed
    ** slot.  The reply ECB is our own stack local (key-8), never in CSA. */
    local_ecb        = 0;
    anchor->req_token = ext->token;
    anchor->req_ecb   = &local_ecb;
    anchor->req_ascb  = __ascb(0);          /* client ASCB: __xmpost uses this  */
    anchor->req_state = NSFP_REQ_PENDING;   /* publish */

    ecbp = &local_ecb;                      /* snapshot before leaving key 0    */

    /* Wake the STC (cross-AS POST, from supervisor state). */
    __xmpost(anchor->server_ascb, &anchor->server_ecb, 0);

    /* Back to problem state before the WAIT (SVC 1). */
    __prob(savekey, NULL);

    /* --- Timed WAIT loop with liveness check (mirrors UFSD) ---
    ** ecb_timed_wait arms a STIMER for NSFP_WAIT_INTERVAL hundredths; if it
    ** fires before the STC replies, the ECB is posted with NSFP_TIMEOUT_CODE.
    ** On timeout we revalidate the anchor: freed CSA (SP=241) is reused, not
    ** zeroed, so the ACTIVE flag alone cannot be trusted -- revalidate the eye
    ** catcher first, or a stale high bit loops forever on an ECB nobody posts.
    ** CSA has no fetch-protection, so anchor->* is readable from problem state. */
    for (;;) {
        ecb_timed_wait(ecbp, NSFP_WAIT_INTERVAL, NSFP_TIMEOUT_CODE);

        if ((*ecbp & ECB_VALUE_MASK) != NSFP_TIMEOUT_CODE)
            break;                          /* normal reply (STC posted code 0) */

        {
            int eye_ok = (memcmp(anchor->eye, "NSFPANCR", 8) == 0);

            if (!eye_ok || !(anchor->flags & NSFP_ANCHOR_ACTIVE)) {
                /* Server gone or quiescing.  If the anchor is still valid (a
                ** clean shutdown retains it), give our in-flight count back so
                ** the shutdown drain can complete -- a dedicated key-0 window,
                ** since we are in problem state here.  If the eye catcher is
                ** gone the anchor was already freed: the counter no longer
                ** exists, do NOT write to it. */
                if (eye_ok) {
                    unsigned char bkey;
                    if (!__super(PSWKEY0, &bkey)) {
                        __udec(&anchor->inflight);
                        __prob(bkey, NULL);
                    }
                }
                ext->rc        = NSFP_RC_CORRUPT;
                ssob->SSOBRETN = NSFP_RC_CORRUPT;
                return;
            }
        }

        *ecbp = 0;                          /* server alive, not replied: retry */
    }

    /* --- Reply: key-0 window to read the result + release the slot --- */
    if (!__super(PSWKEY0, &savekey)) {
        ext->token = anchor->req_token;     /* the STC's echo (token + 1)       */
        ext->seq   = anchor->served;
        ext->rc    = NSFP_RC_OK;

        anchor->req_state = NSFP_REQ_FREE;  /* DONE -> FREE (release)           */

        /* Decrement in-flight LAST, after every other CSA access, so a shutdown
        ** drain observes us gone only once we are done touching CSA. */
        __udec(&anchor->inflight);
        __prob(savekey, NULL);

        ssob->SSOBRETN = NSFP_RC_OK;
    } else {
        /* Re-entering supervisor state failed (it succeeded at entry, so this
        ** should be impossible).  We still owe the in-flight decrement so a
        ** shutdown drain can finish -- retry a minimal key-0 window for it. */
        if (!__super(PSWKEY0, &savekey)) {
            __udec(&anchor->inflight);
            __prob(savekey, NULL);
        }
        ssob->SSOBRETN = NSFP_RC_CORRUPT;
    }
}
