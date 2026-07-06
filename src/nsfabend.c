/*
 * nsfabend.c -- nsf_abend implementation (see nsfabend.h).
 *
 * Portable across host and MVS: the hook path lets host unit tests catch an
 * enforced-invariant violation, and the fallback is a loud abort(). The real
 * executive-task ESTAE/ABEND integration arrives with the recovery work in
 * M0-8 (spec ch. 17); until then abort() is the deliberate hard stop.
 */
#include "nsfabend.h"

#include <stdio.h>
#include <stdlib.h>

static nsf_abend_fn g_abend_hook = 0;

nsf_abend_fn nsf_abend_sethook(nsf_abend_fn hook)
{
    nsf_abend_fn prev = g_abend_hook;
    g_abend_hook = hook;
    return prev;
}

void nsf_abend(UINT code)
{
    if (g_abend_hook != 0) {
        g_abend_hook(code);
        /* A well-behaved hook never returns (it longjmps out of the caller).
         * If one does return, fall through to the hard stop rather than let
         * the caller continue as though nothing happened. */
    }
    fprintf(stderr, "NSF ABEND, user completion code U%04u\n", (unsigned)code);
    abort();
}
