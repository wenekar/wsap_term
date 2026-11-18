#ifndef APP_IDENTITY_H
#define APP_IDENTITY_H

/*
 * The app ID of this process. The application owns this function, and it
 * reads the identity that the application set at startup. A shared module
 * receives a struct app_identity instead of this call.
 */
const char *app_name(void);

#endif
