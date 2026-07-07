/*
 * nsfstim_host.c -- host implementation of the NSFTMR platform arming seam
 * (see nsfstim.h). Swapped in for asm/nsfstim.asm on the native test build via
 * the project.toml [host].replace map; never compiled by cc370.
 *
 * The host has no STIMER: tests drive the clock by calling nsftmr_run(ticks)
 * directly, so arming is a no-op here. Under NSF_DEBUG the shim RECORDS each
 * arm/disarm so the host tests can assert that nsftmr_run re-arms for the head
 * delta and disarms when the queue drains -- behaviour that is otherwise
 * invisible on a platform with no real timer.
 */
#include "nsfstim.h"

/* A real ECB word so nsftmr_plat_ecb() returns a stable, writable address; the
 * host never WAITs on it. */
static UINT g_host_timer_ecb;

#if NSF_DEBUG
static UINT g_last_arm;              /* ticks passed to the most recent arm */
static UINT g_arm_count;
static UINT g_disarm_count;
#endif

void nsftmr_plat_arm(UINT ticks)
{
    g_host_timer_ecb = 0u;           /* mirror the MVS "clear ECB then arm" */
#if NSF_DEBUG
    g_last_arm = ticks;
    g_arm_count++;
#else
    (void)ticks;
#endif
}

void nsftmr_plat_disarm(void)
{
#if NSF_DEBUG
    g_disarm_count++;
#endif
}

UINT *nsftmr_plat_ecb(void)
{
    return &g_host_timer_ecb;
}

#if NSF_DEBUG
UINT nsftmr_plat_last_arm(void)     { return g_last_arm; }
UINT nsftmr_plat_arm_count(void)    { return g_arm_count; }
UINT nsftmr_plat_disarm_count(void) { return g_disarm_count; }

void nsftmr_plat_probe_reset(void)
{
    g_last_arm     = 0u;
    g_arm_count    = 0u;
    g_disarm_count = 0u;
}
#endif
