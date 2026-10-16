#ifndef YASL_PRIVATE_POSIX_SYNC_H
#define YASL_PRIVATE_POSIX_SYNC_H

#include <pthread.h>

typedef pthread_mutex_t yasl_private_mutex;

static inline int yasl_private_mutex_init(yasl_private_mutex *mutex)
{
    return pthread_mutex_init(mutex, NULL);
}

static inline void yasl_private_mutex_destroy(yasl_private_mutex *mutex)
{
    pthread_mutex_destroy(mutex);
}

static inline void yasl_private_mutex_lock(yasl_private_mutex *mutex)
{
    pthread_mutex_lock(mutex);
}

static inline void yasl_private_mutex_unlock(yasl_private_mutex *mutex)
{
    pthread_mutex_unlock(mutex);
}

#endif
