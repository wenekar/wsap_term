#ifndef APP_IDENTITY_H
#define APP_IDENTITY_H

/*
 * Returns the app ID of this process, which the app sets at startup. The app
 * uses it as the config name and the log prefix. A shared module takes a
 * struct app_identity parameter instead of calling this function.
 */
const char *app_name(void);

#endif
