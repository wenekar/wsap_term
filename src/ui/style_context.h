#ifndef WSAP_UI_STYLE_CONTEXT_H
#define WSAP_UI_STYLE_CONTEXT_H

#include "ui/css.h"
#include <stdbool.h>
#include <stdint.h>

struct ui_widget;

struct ui_style_context {
    const struct css_sheet *sheet;
    const struct css_sheet *host_sheet;
    /* Rules above the host sheet, such as a theme sheet. */
    const struct css_sheet *overlay_sheet;
    struct css_sheet *owned_sheet;
    uint64_t generation;
    struct css_scale_context scale;
    struct css_element *path; /* Scratch: the ancestors of the widget at hand. */
    size_t path_count;
    size_t path_cap;
};

void ui_style_context_init(struct ui_style_context *context);
void ui_style_context_destroy(struct ui_style_context *context);
/* Sets the host sheet. A host sheet without the UI core rules gets them
   below its rules. */
void ui_style_context_set_sheet(struct ui_style_context *context,
                                const struct css_sheet *sheet);
/* Sets a sheet that applies above the host sheet. Its variables override the
   variables of the host sheet. NULL removes the overlay. */
void ui_style_context_set_overlay_sheet(struct ui_style_context *context,
                                        const struct css_sheet *sheet);
void ui_style_context_set_scale(struct ui_style_context *context,
                                struct css_scale_context scale);
const struct css_sheet *ui_style_context_sheet(
    const struct ui_style_context *context);
struct css_style ui_style_context_style(const struct ui_style_context *context,
                                        const char *class_name,
                                        unsigned pseudo_flags);
uint64_t ui_style_context_generation(const struct ui_style_context *context);

typedef uint32_t ui_part_id;
/* Returns the handle of `name`. The handle 0 means an empty name, or that the
   part table is out of memory. */
ui_part_id ui_part_intern(const char *name);
const char *ui_part_name(ui_part_id part);
void ui_part_shutdown(void);

void ui_style_context_apply(struct ui_style_context *context,
                            struct ui_widget *root);

const struct css_style *ui_widget_resolved_style(const struct ui_widget *widget);

#endif
