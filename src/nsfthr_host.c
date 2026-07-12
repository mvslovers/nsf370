/*
 * nsfthr_host.c -- host stand-in for the I/O-subtask threading seam (nsfthr.h),
 * over a pthread + a condition variable. Host test builds only; never compiled
 * by cc370. It swaps for src/nsfthr.c (libc370 cthread) via [host].replace, so
 * the SAME CTCI subtask functions (src/nsfctcib.c read_sub / write_sub) run on
 * the host -- exercising the real subtask/handoff logic across a thread boundary
 * under `make test-host`, exactly as NSFHOST's reader thread did at M1-2.
 *
 * ECB MODEL. One global mutex+condvar guards every subtask-waited ECB word
 * (recb / wecb / returnecb / txgoecb): nsfthr_post sets the posted bit and
 * broadcasts; a waiter re-checks its own word (spurious wakes are fine). Because
 * the executive's WAIT (nsfevt_plat_host) has its OWN condvar, nsfthr_post ALSO
 * calls nsfevt_plat_post -- so a subtask's nsfthr_post(&dev->ecb) wakes the
 * executive's WAIT ECBLIST too (harmless for subtask-only ECBs, which are not in
 * the executive's list). So on host every CTCI ECB post MUST go through
 * nsfthr_post (the channel shim posts recb/wecb through it as well).
 */
#include "nsfthr.h"
#include "nsfevtp.h"            /* nsfevt_plat_post -- also wake the executive */

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

struct nsfthr {
    pthread_t     tid;
    int           started;
    int         (*fn)(void *arg);
    void         *arg;
    NSFECB        termecb;                 /* posted when fn() returns */
};

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv  = PTHREAD_COND_INITIALIZER;

int nsfthr_setup(void)
{
    return 0;                              /* no IDENTIFY on the host */
}

void nsfthr_post(NSFECB *ecb, UINT code)
{
    pthread_mutex_lock(&g_mtx);
    *ecb |= NSFECB_POSTED | (code & 0x3FFFFFFFu);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    /* Also wake the executive's separate WAIT condvar (essential for dev->ecb). */
    nsfevt_plat_post(ecb);
}

void nsfthr_wait(NSFECB *ecb)
{
    pthread_mutex_lock(&g_mtx);
    while (!(*ecb & NSFECB_POSTED)) {
        pthread_cond_wait(&g_cv, &g_mtx);
    }
    pthread_mutex_unlock(&g_mtx);
}

void nsfthr_timed_wait(NSFECB *ecb, UINT ticks)
{
    struct timespec ts;
    long            add_ns;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ticks / 10u);                 /* 10 ticks = 1 s */
    add_ns      = (long)(ticks % 10u) * 100000000L;      /* 100 ms per tick */
    ts.tv_nsec += add_ns;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec  += 1;
    }

    pthread_mutex_lock(&g_mtx);
    while (!(*ecb & NSFECB_POSTED)) {
        if (pthread_cond_timedwait(&g_cv, &g_mtx, &ts) == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

static void *thr_trampoline(void *p)
{
    NSFTHR *t = (NSFTHR *)p;

    (void)t->fn(t->arg);
    nsfthr_post(&t->termecb, 0u);          /* signal termination to a joiner */
    return NULL;
}

NSFTHR *nsfthr_create(int (*fn)(void *arg), void *arg)
{
    NSFTHR *t = (NSFTHR *)calloc(1u, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }
    t->fn      = fn;
    t->arg     = arg;
    t->termecb = 0u;
    if (pthread_create(&t->tid, NULL, thr_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    t->started = 1;
    return t;
}

int nsfthr_join(NSFTHR *t, UINT ticks)
{
    if (t == NULL) {
        return 0;
    }
    nsfthr_timed_wait(&t->termecb, ticks);
    if (!(t->termecb & NSFECB_POSTED)) {
        return 1;                          /* still running -- do not free */
    }
    if (t->started) {
        pthread_join(t->tid, NULL);
    }
    free(t);
    return 0;
}
