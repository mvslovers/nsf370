#ifndef NSFCTCIO_HOST_H
#define NSFCTCIO_HOST_H
/*
 * nsfctcio_host.h -- test hooks exported by the host CTCI channel shim
 * (src/nsfctcio_host.c). HOST-ONLY: these have no MVS counterpart (the real
 * device is driven by traffic, not injected), so they are kept out of the
 * driver interface (nsfctci.h) and used only by test/tstctci.c.
 */

#include "nsf.h"

/* A frame "arrives" at the read subchannel `rscb`: completes an outstanding READ
 * immediately, else queues the block for the next READ. `blk` must outlive the
 * READ that consumes it (the shim stores the pointer, not a copy). */
void         ctcio_host_inject(void *rscb, const UCHAR *blk, UINT len);

/* The last block the driver WROTE to the write subchannel `wscb`, and its length
 * in *len -- for asserting the CTCIHDR/CTCISEG framing the driver emitted. */
const UCHAR *ctcio_host_captured(void *wscb, UINT *len);

/* Force the next completion on `scb` to carry `post` / `residual` (an I/O error
 * or short read), exercising the driver's error path. */
void         ctcio_host_force(void *scb, UCHAR post, UINT residual);

/* 1 if a READ is currently outstanding on `rscb` (armed, no frame yet). */
int          ctcio_host_outstanding(void *rscb);

#endif /* NSFCTCIO_HOST_H */
