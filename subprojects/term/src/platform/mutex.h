#ifndef WSAP_VTCORE_PLATFORM_MUTEX_H
#define WSAP_VTCORE_PLATFORM_MUTEX_H

#if defined(_WIN32)
#include "platform/windows/mutex.h"
#else
#include "platform/posix/mutex.h"
#endif

#endif
