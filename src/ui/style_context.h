#ifndef WSAP_UI_STYLE_CONTEXT_H
#define WSAP_UI_STYLE_CONTEXT_H

#include "ui/css.h"
#include <stdbool.h>
#include <stdint.h>

struct ui_widget;

struct ui_style_context {
    const struct css_sheet *sheet;
    const struct css_sheet *host_sheet;
    struct css_sheet *owned_sheet;
    uint64_t generation;
    struct css_scale_context scale;
    uint32_t pad;
};

void ui_style_context_init(struct ui_style_context *context);
void ui_style_context_destroy(struct ui_style_context *context);
void ui_style_context_set_sheet(struct ui_style_context *context,
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
ui_part_id ui_part_intern(const char *name);
const char *ui_part_name(ui_part_id part);

void ui_style_context_apply(struct ui_style_context *context,
                            struct ui_widget *root);

const struct css_style *ui_widget_resolved_style(const struct ui_widget *widget);

#endif
