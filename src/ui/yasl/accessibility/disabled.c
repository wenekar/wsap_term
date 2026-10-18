#include "ui/yasl/accessibility/accessibility.h"

const struct ui_accessibility_provider_ops *ui_accessibility_provider_ops(void)
{
    return NULL;
}

void ui_accessibility_set_wake_callback(
    const struct ui_accessibility_provider *provider,
    void (*wake)(void *userdata), void *userdata)
{
    (void)provider;
    (void)wake;
    (void)userdata;
}
