/*
 * nsfctci_host.c -- host stand-in for the CTCI channel + SVC 99 seam
 * (src/nsfctci.c), swapped in by the project.toml [host].replace map. Host test
 * builds only; never compiled by cc370.
 *
 * On the host there is no MVS device to allocate and no DCB to OPEN: the host
 * channel shim (src/nsfctcio_host.c) models the subchannel directly, so bringing
 * a device up is a success no-op. The portable bottom half (src/nsfctcib.c)
 * therefore drives ctci_chan_open/close identically on host and MVS -- on MVS
 * they do the real SVC 99 allocate + OPEN (src/nsfctci.c). The SVC 99 wrappers
 * (svc99_call / ctci_alloc_unit / ctci_free_ddn) are MVS-only (TSTCTCM), so they
 * are not provided here.
 */
#include "nsfctci.h"

int ctci_chan_open(CTCIDEV *d)
{
    if (d != NULL) {
        d->state = CTCI_S_UP;
    }
    return 0;
}

int ctci_chan_close(CTCIDEV *d)
{
    if (d != NULL) {
        d->state = CTCI_S_DOWN;
    }
    return 0;
}
