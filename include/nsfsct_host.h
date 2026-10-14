/*----------------------------------------------------------------------------
 * nsfsct_host.h -- host-only test hooks for the d2 rendezvous shim.
 *
 * NOT built on MVS, and the test that uses it is `mvs = false`.  The struct
 * mirrors NSFV_ANCHOR's first fields BY NAME so the host test drives
 * nsfsct_classify through the same call shape the MVS half uses; it does NOT
 * mirror the offsets, and no claim about them is made here.
 *
 * THE THREE HOOKS BELOW ALL MANGLE TO `NSFSCT@H`, and that is fine ONLY
 * because nothing here is ever compiled by cc370.  Eight characters, upcased,
 * `_`->`@`: nsfsct_host_set_jobname, nsfsct_host_reset and nsfsct_host_count
 * are one symbol as far as ld370 is concerned (CLAUDE.md 3).  Host builds have
 * no such limit and never see it -- which is exactly the blindness that rule
 * exists for, so it is written down here rather than left to be rediscovered.
 * If any of these ever needs to exist on MVS, it needs an explicit 8-char
 * asm() alias first.
 *--------------------------------------------------------------------------*/
#ifndef NSFSCT_HOST_H
#define NSFSCT_HOST_H

#include "nsf.h"
#include "nsfsct.h"

typedef struct {
    char eye[8];
    UINT version;
    char jobname_tail[NSFSCT_TAILLEN];
} NSFSCT_HOST_ANCHOR;

void nsfsct_host_set_jobname(const char *job8);
void nsfsct_host_reset(void);
int  nsfsct_host_count(void);

#endif /* NSFSCT_HOST_H */
