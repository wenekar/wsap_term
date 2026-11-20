#ifndef WSAP_APP_TABS_TAB_PREVIEW_H
#define WSAP_APP_TABS_TAB_PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "paint/scene.h"
#include "ui/widget.h"
#include "util/geom.h"

struct ui_tab_preview_source {
    /* Fills the title and the optional thumbnail of tab `index` within `width`
     * and `max_height` device pixels. Returns false when the capture fails.
     * The preview copies the title and the image descriptor, so the source
     * must keep the pixels valid until the next capture. */
    bool (*capture)(void *userdata, int index, int width, int max_height,
                    bool dragging, const char **title,
                    const struct ui_image **image);
    void *userdata;
};

struct ui_tab_preview {
    struct ui_widget widget;
    struct {
        struct ui_tab_preview_source source;
        char *title;
        /* The thumbnail of the tab, held by value: a new capture compares
         * against it. No pixels means no image. */
        struct ui_image image;
        struct rect2i anchor;
        struct rect2i budget_box;
        struct vector2i follow_pos;
        uint64_t show_at;
        uint64_t refresh_at;
        int index;
        int pending;
        int budget_w;
        int budget_h;
        bool dragging;
        bool shown;
        bool suppressed;
        bool hidden_for_size;
        bool managed_anchor;
    } state;
};

void ui_tab_preview_init(struct ui_tab_preview *p);
/* Sets whether the parent layout updates anchor and size budget for `p`. */
void ui_tab_preview_set_managed_anchor(struct ui_tab_preview *p, bool managed);
/* Updates the anchor and the size budget of `p` during a parent layout pass.
 * During a drag or while `p` is suppressed, it returns false and changes
 * nothing. Returns true when the caller must measure `p` again. */
bool ui_tab_preview_sync_anchor(struct ui_tab_preview *p, int index,
                                struct rect2i anchor);
/* Sets the capture source of `p` and hides the preview. */
void ui_tab_preview_set_source(struct ui_tab_preview *p,
                               const struct ui_tab_preview_source *source);
/* Requests the preview of the tab at `index`, anchored at `anchor`.
 * A negative `index` hides the preview. Does nothing during a drag. */
void ui_tab_preview_hover(struct ui_tab_preview *p, int index,
                          struct rect2i anchor);
/* Suppresses preview display until the next `ui_tab_preview_hover` call with
 * a negative index. */
void ui_tab_preview_suppress(struct ui_tab_preview *p);
/* Sets the drag position of the preview of tab `index` to `at`.
 * A negative `index` hides the preview. */
void ui_tab_preview_follow(struct ui_tab_preview *p, int index,
                           struct vector2i at);
/* Hides and resets the tab preview popup. */
void ui_tab_preview_hide(struct ui_tab_preview *p);
/* Returns true when the tab preview popup is visible. */
bool ui_tab_preview_is_visible(const struct ui_tab_preview *p);
/* Returns the maximum thumbnail width in pixels for dragging or resting
 * mode. */
int ui_tab_preview_image_width(const struct ui_tab_preview *p, bool dragging);

/* Returns the base widget pointer of `p`, or NULL if `p` is NULL. */
static inline struct ui_widget *ui_tab_preview_widget(struct ui_tab_preview *p)
{
    return p ? &p->widget : NULL;
}

#endif
