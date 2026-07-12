/*
 * nsfevt_plat.c -- MVS WAIT/POST seam for the NSFEVT loop (see nsfevtp.h).
 *
 * WAIT via libc370 ecb_waitlist (WAIT ECBLIST, SVC 1). POST just sets the ECB
 * word: at M0-6 the only self-posted ECB is the stop request, which the loop
 * notices at its next timer wake (the async STIMER exit posts timerECB every
 * ~100 ms); a real cross-task operator POST is an M0-8 concern. The timer ECB
 * itself is posted by the STIMER exit in asm/nsfstim.asm.
 *
 * Swapped for src/nsfevt_plat_host.c on the host by [host].replace; never
 * compiled by cc370 on the host.
 */
#include "nsfevtp.h"

/* libc370: wait for any ECB in a VL-terminated list to be posted. Declared here
 * (rather than via <clibecb.h>) so the seam has no header dependency. */
int ecb_waitlist(NSFECB **ecblist) asm("@@ECBWL");

/* Local copy capacity: MUST be >= the loop's EVT_ECBLIST_MAX (nsfevt.c). If it
 * is smaller, a full list is silently truncated here and the ECBs at the END --
 * the cib and stop ECBs -- are dropped from the WAIT, so the loop can never wake
 * for an operator command or a stop (an every-device-active hang). Kept at 16 to
 * match EVT_ECBLIST_MAX. */
#define NSFEVT_WAIT_MAX  16

void nsfevt_plat_wait(NSFECB **ecblist, int count)
{
    NSFECB *list[NSFEVT_WAIT_MAX];
    int     i;

    if (count > (int)(sizeof(list) / sizeof(list[0]))) {
        count = (int)(sizeof(list) / sizeof(list[0]));
    }
    for (i = 0; i < count; i++) {
        list[i] = ecblist[i];
    }
    /* The last ECB address carries the VL bit (WAIT ECBLIST terminator). */
    list[count - 1] = (NSFECB *)((UINT)list[count - 1] | NSFECB_LAST);
    (void)ecb_waitlist(list);
}

void nsfevt_plat_post(NSFECB *ecb)
{
    *ecb |= NSFECB_POSTED;
}
