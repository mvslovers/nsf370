/*
 * nsfevt_plat_host.c -- host WAIT/POST seam for the NSFEVT loop (see nsfevtp.h).
 *
 * A pthread mutex + condition variable stands in for WAIT ECBLIST / POST, so
 * the loop blocks and a simulated exit (another thread) or a stop request can
 * wake it. Host test builds only; never compiled by cc370. pthread is sanctioned
 * here because this is host-native test scaffolding, not target code (the target
 * WAIT is the MVS SVC in src/nsfevt_plat.c).
 */
#include "nsfevtp.h"
#include <pthread.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;

static int any_posted(NSFECB **ecblist, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (*ecblist[i] & NSFECB_POSTED) {
            return 1;
        }
    }
    return 0;
}

void nsfevt_plat_wait(NSFECB **ecblist, int count)
{
    pthread_mutex_lock(&g_mtx);
    while (!any_posted(ecblist, count)) {
        pthread_cond_wait(&g_cv, &g_mtx);
    }
    pthread_mutex_unlock(&g_mtx);
}

void nsfevt_plat_post(NSFECB *ecb)
{
    pthread_mutex_lock(&g_mtx);
    *ecb |= NSFECB_POSTED;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
}
