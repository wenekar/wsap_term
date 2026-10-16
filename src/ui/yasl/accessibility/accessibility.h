#ifndef WSAP_WINDOW_UI_ACCESSIBILITY_H
#define WSAP_WINDOW_UI_ACCESSIBILITY_H

#include "ui/accessibility.h"

struct yasl_surface;

const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void);
void *ui_accessibility_native_handle(struct yasl_surface *s);
void ui_accessibility_set_menu_popup_depth(
    const struct ui_accessibility_provider *provider, int depth);

#endif
