/*
 * nsfhost_plat.c -- the MVS placeholder for the HOST device driver (spec 9.4).
 *
 * There is no host driver on MVS: the concrete drivers there are CTCI/LCS. This
 * translation unit exists so a test that references nsfhost_ops (test/tstdev.c)
 * cross-compiles and links cleanly under cc370 -- the project.toml [host].replace
 * map swaps it for the real src/nsfhost.c on the native test build. On MVS
 * nsfhost_ops returns NULL, and the caller (a portable test) sees that the host
 * driver is unavailable and skips the host-loopback path; the device
 * abstraction itself is validated on MVS by the CTCI driver (M1-3+).
 *
 * Portable C, no host or MVS services -- so it assembles clean on cc370.
 */
#include "nsfhost.h"

DEVOPS *nsfhost_ops(void)
{
    return NULL;                        /* no host driver on MVS -- use CTCI/LCS */
}
