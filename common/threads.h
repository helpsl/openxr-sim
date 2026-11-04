// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The openxr-sim authors
//
// THE PART OF PTHREADS THIS PROJECT USES, on every platform: a mutex, a condition variable, once. POSIX has
// them; on Windows they are the same names over SRWLOCK, CONDITION_VARIABLE and INIT_ONCE, so the code that
// uses them is written once, against the POSIX names.
#ifndef OPENXR_SIM_THREADS_H
#define OPENXR_SIM_THREADS_H

#ifndef _WIN32
#include <pthread.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) { (void)attr; InitializeSRWLock(m); return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const void *attr) { (void)attr; InitializeConditionVariable(c); return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : -1; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(c); return 0; }

static BOOL CALLBACK oxs_once_trampoline(PINIT_ONCE once, PVOID fn, PVOID *ctx)
{
    (void)once; (void)ctx;
    ((void (*)(void))fn)();
    return TRUE;
}
static inline int pthread_once(pthread_once_t *o, void (*fn)(void))
{
    return InitOnceExecuteOnce(o, oxs_once_trampoline, (PVOID)fn, NULL) ? 0 : -1;
}
#endif

#endif
