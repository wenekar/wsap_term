#ifndef WINDOW_UI_H
#define WINDOW_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "paint/damage.h"
#include "ui/css.h"
#include "ui/widgets/menu.h"
#include "util/geom.h"

struct font_instance;
struct json_writer;
struct ui_surface;
struct ui_scene;
struct ui_widget;
struct window;
struct window_ui;
struct window_frame;

struct window_ui_frame {
    struct ui_scene *scene;
    struct ui_damage damage;
    struct font_instance *font;
    uint32_t background;
    bool repaint;
    bool gpu_off;
    bool suspended;
};

struct window_ui_spec {
    const char *name;
    const void *kind;
    struct ui_widget *root;
    int ui_font_size;
    void (*restyle)(struct window *w);
    void (*resize)(struct window *w, struct vector2i size);
    void (*suspended)(struct window *w, bool suspended);
    void (*debug_state)(struct window *w, struct json_writer *j);
    void (*destroy)(struct window *w);
    void *content;
};

bool window_ui_mount(struct window *w, const struct window_ui_spec *spec);
const void *window_ui_kind(const struct window *w);
struct window_ui *window_ui_of(const struct window *w);

void window_ui_prepare_frame(struct window *w, uint64_t now_ms);
bool window_ui_build_frame(struct window *w, struct yasl_surface *surface,
                           uint64_t now_ms, struct window_ui_frame *out);
void window_ui_frame_done(struct window *w, struct window_frame *frame,
                          uint64_t now_ms);

struct ui_surface *window_ui_key_surface(struct window *w);
struct css_scale_context window_ui_scale(const struct window *w);
void window_ui_restyle(struct window *w);
void window_ui_set_style(struct window *w, const struct css_sheet *sheet,
                         uint32_t background);
void window_ui_relayout(struct window *w);
void window_ui_hide_pointer(struct window *w);
void window_ui_update_cursor(struct window *w);

bool window_menu_open(struct window *w, const ui_menu_item *items, int n,
                      struct vector2i at, ui_action_fn fn,
                      ui_menu_submenu_fn submenu, void *userdata);
void window_menu_close(struct window *w);
void window_menu_sync(struct window *w);
bool window_menu_is_open(const struct window *w);

struct ui_surface *window_ui_surface(struct window *w);
struct ui_widget *window_ui_root(struct window *w);
struct font_instance *window_ui_font(const struct window *w);
void *window_ui_content(const struct window *w);

#endif
