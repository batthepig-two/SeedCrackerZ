/*
 * Fallback <pthread.h> for environments that ship no real pthread header.
 * This covers iOS a-Shell (wasm32-wasi) and similar minimal toolchains.
 *
 * The build uses `-idirafter compat`, so a real <pthread.h> in the system
 * search path is always preferred; this file is only reached when none exists.
 *
 * All "thread" operations run inline (single-core). All mutex operations are
 * no-ops. The answers produced are identical to a multi-threaded run; only
 * parallelism is lost.
 */
#ifndef SEEDCRACKERZ_PTHREAD_FALLBACK_H
#define SEEDCRACKERZ_PTHREAD_FALLBACK_H

#include <stddef.h>
#include <sys/types.h>

/* ── Mutex type + operations (no-ops — single-threaded, no contention) ── */

typedef struct { int _dummy; } pthread_mutex_t;
typedef struct { int _dummy; } pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

static inline int pthread_mutex_init(pthread_mutex_t *m,
                                     const pthread_mutexattr_t *a) {
    (void)m; (void)a; return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m)    { (void)m; return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m)  { (void)m; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

/* ── Thread operations (run inline — no real parallelism) ────────────── */

static inline int pthread_create(pthread_t *tid,
                                 const pthread_attr_t *attr,
                                 void *(*start)(void *),
                                 void *arg) {
    (void)attr;
    if (tid) *tid = (pthread_t)0;
    if (start) start(arg);
    return 0;
}

static inline int pthread_join(pthread_t tid, void **retval) {
    (void)tid;
    if (retval) *retval = NULL;
    return 0;
}

static inline void pthread_exit(void *retval) {
    (void)retval;
}

#endif /* SEEDCRACKERZ_PTHREAD_FALLBACK_H */
