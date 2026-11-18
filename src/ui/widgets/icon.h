#ifndef WSAP_UI_WIDGETS_ICON_H
#define WSAP_UI_WIDGETS_ICON_H

#include <stdbool.h>

#include "paint/scene.h"
#include "ui/widget.h"

/* An image or icon. An image draws at its own pixel size. Without an image,
 * the widget draws its label, such as one glyph from an icon font. */
struct ui_icon {
    struct ui_widget widget;
    struct {
        struct ui_image image;
        char *label;
        int action_id;
    } state;
};

void ui_icon_init(struct ui_icon *icon);
/* Set the image. The caller keeps the pixels valid while the widget paints
 * them. Pass NULL to draw the label instead. */
void ui_icon_set_image(struct ui_icon *icon, const struct ui_image *image);
bool ui_icon_has_image(const struct ui_icon *icon);
/* Copy the label. The label is also the accessible name.
 * Pass NULL to clear it. A failure keeps the previous label. */
bool ui_icon_set_label(struct ui_icon *icon, const char *label);
const char *ui_icon_label(const struct ui_icon *icon);
void ui_icon_set_action_id(struct ui_icon *icon, int id);

static inline struct ui_widget *ui_icon_widget(struct ui_icon *icon)
{
    if (icon && !icon->widget.ops)
        ui_icon_init(icon);
    return icon ? &icon->widget : NULL;
}

#endif
