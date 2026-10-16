#ifndef WSAP_WINDOW_UI_ACCESSIBILITY_H
#define WSAP_WINDOW_UI_ACCESSIBILITY_H

#include "ui/accessibility.h"

struct yasl_surface;

/* The provider for this platform. NULL means native accessibility is
 * unavailable. */
const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void);
/* The handle to give the provider's attach. The handle is borrowed from s. */
void *ui_accessibility_native_handle(struct yasl_surface *s);
/* Marks an attached provider as the menu popup at depth. Call it before the
 * first snapshot. A provider that was not marked is a toplevel. */
void ui_accessibility_set_menu_popup_depth(
    const struct ui_accessibility_provider *provider, int depth);

#endif
