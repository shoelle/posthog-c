#include "ph_thread.h"

#include <stdlib.h>

/* A tiny heap box so the OS trampoline can recover the C callback + arg.
 * Freed by the trampoline once it has copied the two fields out. */
typedef struct ph_thread_start_ctx {
    ph_thread_fn fn;
    void *arg;
} ph_thread_start_ctx;

#if defined(_WIN32)

void ph_mutex_init(ph_mutex *m) { InitializeCriticalSection(&m->cs); }
void ph_mutex_destroy(ph_mutex *m) { DeleteCriticalSection(&m->cs); }
void ph_mutex_lock(ph_mutex *m) { EnterCriticalSection(&m->cs); }
void ph_mutex_unlock(ph_mutex *m) { LeaveCriticalSection(&m->cs); }

void ph_cond_init(ph_cond *c) { InitializeConditionVariable(&c->cv); }
void ph_cond_destroy(ph_cond *c) { (void)c; /* no teardown needed on Win32 */ }
void ph_cond_signal(ph_cond *c) { WakeConditionVariable(&c->cv); }
void ph_cond_broadcast(ph_cond *c) { WakeAllConditionVariable(&c->cv); }

int ph_cond_timedwait(ph_cond *c, ph_mutex *m, int timeout_ms) {
    DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    if (SleepConditionVariableCS(&c->cv, &m->cs, ms)) return 0; /* woken */
    return 1;                                                   /* timed out */
}

static DWORD WINAPI ph_thread_trampoline(LPVOID p) {
    ph_thread_start_ctx *ctx = (ph_thread_start_ctx *)p;
    ph_thread_fn fn = ctx->fn;
    void *arg = ctx->arg;
    free(ctx);
    fn(arg);
    return 0;
}

int ph_thread_start(ph_thread *t, ph_thread_fn fn, void *arg) {
    ph_thread_start_ctx *ctx = (ph_thread_start_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    t->h = CreateThread(NULL, 0, ph_thread_trampoline, ctx, 0, NULL);
    if (!t->h) {
        free(ctx);
        return -1;
    }
    return 0;
}

void ph_thread_join(ph_thread *t) {
    if (!t->h) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    t->h = NULL;
}

void ph_sleep_ms(int ms) {
    if (ms > 0) Sleep((DWORD)ms);
}

#else /* POSIX */

#include <errno.h>
#include <time.h>

void ph_mutex_init(ph_mutex *m) { pthread_mutex_init(&m->m, NULL); }
void ph_mutex_destroy(ph_mutex *m) { pthread_mutex_destroy(&m->m); }
void ph_mutex_lock(ph_mutex *m) { pthread_mutex_lock(&m->m); }
void ph_mutex_unlock(ph_mutex *m) { pthread_mutex_unlock(&m->m); }

void ph_cond_init(ph_cond *c) { pthread_cond_init(&c->c, NULL); }
void ph_cond_destroy(ph_cond *c) { pthread_cond_destroy(&c->c); }
void ph_cond_signal(ph_cond *c) { pthread_cond_signal(&c->c); }
void ph_cond_broadcast(ph_cond *c) { pthread_cond_broadcast(&c->c); }

int ph_cond_timedwait(ph_cond *c, ph_mutex *m, int timeout_ms) {
    struct timespec ts;
    long add_ns;
    int rc;

    if (timeout_ms < 0) {
        pthread_cond_wait(&c->c, &m->m);
        return 0;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    add_ns = (long)(timeout_ms % 1000) * 1000000L;
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += add_ns;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec += 1;
    }
    rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
    return (rc == ETIMEDOUT) ? 1 : 0;
}

static void *ph_thread_trampoline(void *p) {
    ph_thread_start_ctx *ctx = (ph_thread_start_ctx *)p;
    ph_thread_fn fn = ctx->fn;
    void *arg = ctx->arg;
    free(ctx);
    fn(arg);
    return NULL;
}

int ph_thread_start(ph_thread *t, ph_thread_fn fn, void *arg) {
    ph_thread_start_ctx *ctx = (ph_thread_start_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    if (pthread_create(&t->t, NULL, ph_thread_trampoline, ctx) != 0) {
        free(ctx);
        t->started = 0;
        return -1;
    }
    t->started = 1;
    return 0;
}

void ph_thread_join(ph_thread *t) {
    if (!t->started) return;
    pthread_join(t->t, NULL);
    t->started = 0;
}

void ph_sleep_ms(int ms) {
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif
