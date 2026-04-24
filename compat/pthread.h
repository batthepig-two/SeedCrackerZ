/*
 * Fallback <pthread.h> for environments that link against a libc which
 * already declares the pthread *types* (in <sys/types.h>) but does not
 * ship the <pthread.h> header itself. This is the situation on iOS
 * a-Shell, and on a few other minimal POSIX-ish toolchains.
 *
 * On any system that has a real <pthread.h>, that one is used instead:
 * the build references this directory with `-idirafter compat`, so the
 * compiler only falls back here when the system search path comes up
 * empty.
 *
 * Threads run *inline* (synchronously). Code that uses
 *   pthread_create / pthread_join / pthread_exit
 * still links and runs correctly, just on a single core. This loses
 * parallelism but never changes the answer.
 */
#ifndef SEEDCRACKERZ_PTHREAD_FALLBACK_H
#define SEEDCRACKERZ_PTHREAD_FALLBACK_H

#include <stddef.h>
#include <sys/types.h>

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
    /* Caller's "thread function" simply returns to its caller. */
}

#endif /* SEEDCRACKERZ_PTHREAD_FALLBACK_H */
