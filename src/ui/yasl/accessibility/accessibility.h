#ifndef WSAP_WINDOW_UI_ACCESSIBILITY_H
#define WSAP_WINDOW_UI_ACCESSIBILITY_H

#include "ui/accessibility.h"

/* Attach these providers to a borrowed yasl_surface. */
const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void);
/* The callback can run on another thread. Detach waits for it to return. */
void ui_accessibility_set_wake_callback(
    const struct ui_accessibility_provider *provider,
    void (*wake)(void *userdata), void *userdata);

#endif
