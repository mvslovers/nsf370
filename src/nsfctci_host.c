/*
 * nsfctci_host.c -- host stand-in for the CTCI channel + SVC 99 seam
 * (src/nsfctci.c), swapped in by the project.toml [host].replace map. Host test
 * builds only; never compiled by cc370.
 *
 * On the host there is no MVS device to allocate: the host channel shim
 * (src/nsfctcio_host.c) models the subchannel directly, so SVC 99 allocate /
 * unallocate is a success no-op. The OPEN/CLOSE run on the owning subtask via the
 * top-half shim (ctci_open_sub / ctci_close_sub in nsfctcio_host.c), so the
 * portable bottom half (src/nsfctcib.c) brings a device up the same way on host
 * and MVS. The SVC 99 wrappers (svc99_call / ctci_alloc_unit / ctci_free_ddn) are
 * MVS-only (TSTCTCM), so they are not provided here.
 */
#include "nsfctci.h"

int ctci_chan_alloc(CTCIDEV *d)
{
    /* No real allocation on the host; leave the DDNAMEs empty (the shim ignores
     * them). The subtask's ctci_open_sub shim succeeds regardless. */
    if (d != NULL) {
        d->rddn[0] = '\0';
        d->wddn[0] = '\0';
    }
    return 0;
}

int ctci_chan_unalloc(CTCIDEV *d)
{
    (void)d;
    return 0;
}

/* No real DCB/DEB/UCB on the host: the host halt (ctci_halt_read in
 * nsfctcio_host.c) completes the pending read via rscb directly and never uses a
 * UCB address. Return success with a dummy non-zero handle so ctci_op_start's
 * "UCB acquired" path is exercised identically on both worlds. */
int ctci_read_ucb(const void *rscb, USHORT cuu, UINT *ucb_out)
{
    (void)rscb;
    (void)cuu;
    if (ucb_out != NULL) {
        *ucb_out = 1u;                   /* opaque, never dereferenced on host */
    }
    return 0;
}
