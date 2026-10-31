#ifndef WSAP_VTCORE_PLATFORM_WINDOWS_MUTEX_H
#define WSAP_VTCORE_PLATFORM_WINDOWS_MUTEX_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdbool.h>

typedef SRWLOCK wsap_vtcore_mutex;

static inline bool wsap_vtcore_mutex_init(wsap_vtcore_mutex *m)
{
    InitializeSRWLock(m);
    return true;
}

static inline void wsap_vtcore_mutex_lock(wsap_vtcore_mutex *m)
{
    AcquireSRWLockExclusive(m);
}

static inline void wsap_vtcore_mutex_unlock(wsap_vtcore_mutex *m)
{
    ReleaseSRWLockExclusive(m);
}

static inline void wsap_vtcore_mutex_destroy(wsap_vtcore_mutex *m)
{
    (void)m;
}

#endif
