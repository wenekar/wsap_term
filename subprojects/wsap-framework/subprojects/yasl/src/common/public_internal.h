#ifndef YASL_PUBLIC_INTERNAL_H
#define YASL_PUBLIC_INTERNAL_H
#include "yasl/yasl.h"
#include "common/backend.h"
struct yasl_private_display *yasl_public_display(void);
/* Creates the display with default options when none exists; false with errno
 * set on failure. */
bool yasl_public_initialize(void);
bool yasl_public_owner(void);
/* Checks that s belongs to the live display on the owner thread; a mutation also fails
 * with EBUSY inside a refresh callback. */
bool yasl_public_surface(yasl_surface *, bool mutation);
/* yasl_public_surface plus a connected display, for requests that reach the server. */
bool yasl_public_connected(yasl_surface *);
bool yasl_public_fail(int error);
int yasl_public_status(bool success);
int yasl_public_error(int error);
void yasl_private_surface_notify_refresh(struct yasl_surface *);
#endif
