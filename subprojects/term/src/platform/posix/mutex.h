#ifndef WSAP_VTCORE_PLATFORM_POSIX_MUTEX_H
#define WSAP_VTCORE_PLATFORM_POSIX_MUTEX_H

#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

typedef pthread_mutex_t wsap_vtcore_mutex;

#if !defined(PTHREAD_PRIO_INHERIT)
#define PTHREAD_PRIO_INHERIT 1
#endif

static inline bool wsap_vtcore_mutex_init(wsap_vtcore_mutex *m)
{
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return false;
    bool ok = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0 &&
              pthread_mutex_init(m, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    return ok;
#else
    return pthread_mutex_init(m, NULL) == 0;
#endif
}

static inline void wsap_vtcore_mutex_lock(wsap_vtcore_mutex *m)
{
    (void)pthread_mutex_lock(m);
}

static inline void wsap_vtcore_mutex_unlock(wsap_vtcore_mutex *m)
{
    (void)pthread_mutex_unlock(m);
}

static inline void wsap_vtcore_mutex_destroy(wsap_vtcore_mutex *m)
{
    (void)pthread_mutex_destroy(m);
}

#endif
