/*
 * nsfmsg_plat.c -- MVS emit seam for nsfmsg (see nsfmsg.h).
 *
 * One finished line -> one WTO via libc370 wto(). wto() itself splits a line
 * longer than 124 columns across WTO segments, so nsfmsg's NSFMSG_LINE buffer
 * needs no further wrapping here. The text is already EBCDIC (cc370 encoded the
 * literals + vsnprintf'd the values), which is exactly what wto() expects.
 *
 * Swapped for src/nsfmsg_host.c on the host by [host].replace; never compiled
 * by cc370 on the host.
 */
#include "nsfmsg.h"
#include <clibwto.h>            /* libc370 wto() */

void nsfmsg_emit(const char *line)
{
    wto((char *)line);          /* wto takes a non-const buffer; it only reads */
}
