#ifndef YASL_PRIVATE_COMMON_DESKTOP_H
#define YASL_PRIVATE_COMMON_DESKTOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Operating-system desktop services behind yasl/desktop.h. Common code
 * validates arguments first. Path and locale functions write a NUL-terminated
 * string to buf and return its length, or zero on failure or when it does not
 * fit in cap. */
bool desktop_open_uri(const char *uri);
bool desktop_notify(const char *app_id, const char *title, const char *body);
uint32_t desktop_appearance(void);
size_t desktop_executable_path(char *buf, size_t cap);
size_t desktop_resource_dir(char *buf, size_t cap);
size_t desktop_default_locale(char *buf, size_t cap);

#endif
