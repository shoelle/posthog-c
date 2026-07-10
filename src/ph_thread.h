/*
 * ph_thread.h - the thin threading layer the native sender needs.
 *
 * The design assumes the host has no async runtime (a game engine won't host
 * one), so delivery runs on one plain background thread guarded by a mutex +
 * condition variable. We wrap Win32 and pthreads rather than depend on C11
 * <threads.h>, which MSVC only shipped recently.
 *
 * The concrete platform types are exposed here (not opaque) so ph_ctx can
 * embed a mutex/cond/thread by value. That pulls <windows.h>/<pthread.h> into
 * the internal TUs that include this - acceptable for an internal header.
 */
#ifndef PH_THREAD_H
#define PH_THREAD_H

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef struct ph_mutex { CRITICAL_SECTION cs; } ph_mutex;
typedef struct ph_cond { CONDITION_VARIABLE cv; } ph_cond;
typedef struct ph_thread { HANDLE h; DWORD id; } ph_thread;

#else
#include <pthread.h>

typedef struct ph_mutex { pthread_mutex_t m; } ph_mutex;
typedef struct ph_cond { pthread_cond_t c; } ph_cond;
typedef struct ph_thread { pthread_t t; int started; } ph_thread;

#endif

void ph_mutex_init(ph_mutex *m);
void ph_mutex_destroy(ph_mutex *m);
void ph_mutex_lock(ph_mutex *m);
void ph_mutex_unlock(ph_mutex *m);

void ph_cond_init(ph_cond *c);
void ph_cond_destroy(ph_cond *c);
/* Wake one waiter. Unused by the SDK today (the sender broadcasts); kept to
 * round out the primitive set. */
void ph_cond_signal(ph_cond *c);
void ph_cond_broadcast(ph_cond *c);
/* Wait until signaled or `timeout_ms` elapses. Returns 1 if the wait timed
 * out, 0 if it was (possibly spuriously) woken. Caller holds `m`. */
int ph_cond_timedwait(ph_cond *c, ph_mutex *m, int timeout_ms);

typedef void (*ph_thread_fn)(void *arg);
/* Start `fn(arg)` on a new thread. Returns 0 on success. */
int ph_thread_start(ph_thread *t, ph_thread_fn fn, void *arg);
void ph_thread_join(ph_thread *t);
int ph_thread_is_current(const ph_thread *t);

/* Sleep the calling thread for `ms` milliseconds (no-op if ms <= 0). */
void ph_sleep_ms(int ms);

#endif /* PH_THREAD_H */
