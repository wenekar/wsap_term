#ifndef UTIL_NAME_H
#define UTIL_NAME_H

/*
 * The app ID of this process. The application owns this function, and it
 * reads the identity that the application set at startup. Shared modules
 * receive a struct app_identity instead of this call.
 */
const char *app_name(void);

#endif
