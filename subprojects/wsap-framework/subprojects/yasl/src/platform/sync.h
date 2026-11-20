#ifndef YASL_PRIVATE_PLATFORM_SYNC_H
#define YASL_PRIVATE_PLATFORM_SYNC_H

/* The OS support directory selected by Meson provides yasl_private_mutex and the
 * yasl_private_mutex_init/destroy/lock/unlock operations. Init returns zero on success
 * or an errno value on failure. The other operations require an initialized
 * mutex. Native types and calls stay in the selected implementation header. */
#include "yasl_platform_sync.h" // IWYU pragma: export

#endif
