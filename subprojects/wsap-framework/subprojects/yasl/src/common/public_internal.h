#ifndef YASL_PUBLIC_INTERNAL_H
#define YASL_PUBLIC_INTERNAL_H
#include "yasl/yasl.h"
#include "common/backend.h"
struct yasl_private_display *yasl_public_display(void);
/* Initializes with defaults when needed; false with errno otherwise. */
bool yasl_public_initialize(void);
bool yasl_public_owner(void);
/* Validates a live surface on the owner thread; mutation also refuses inside a refresh callback. */
bool yasl_public_surface(yasl_surface *, bool mutation);
/* yasl_public_surface plus a connected display, for requests sent to the server. */
bool yasl_public_connected(yasl_surface *);
bool yasl_public_fail(int error);
int yasl_public_status(bool success);
int yasl_public_error(int error);
void yasl_private_surface_notify_refresh(struct yasl_surface *);
#endif
