#ifndef TERMINAL_TERMINAL_H
#define TERMINAL_TERMINAL_H

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>
#include "util/geom.h"
#include "ui/yasl/window.h"

struct session;
struct tab;
struct window;
struct window_host;
struct font_instance;
struct wsap_vtcore_snapshot;
struct ui_image;
struct ui_scene;

typedef bool (*terminal_preview_capture_fn)(void *ud, void **state,
                                             struct font_instance *font,
                                             struct wsap_vtcore *term, int width,
                                             struct ui_image *image);
typedef void (*terminal_preview_release_fn)(void *state);
typedef bool (*terminal_scene_paint_fn)(void *ud, struct font_instance *font,
                                        uint32_t *pixels,
                                        struct vector2i size,
                                        const struct ui_scene *scene);

struct terminal_frame_source {
    struct wsap_vtcore_snapshot *snapshot;
    struct font_instance *font;
    struct rect2i rect;
    int origin_y;
    bool cursor_visible;
};

struct terminal_config {
    char *command;
    int scrollback;
    enum wsap_vtcore_cursor_style cursor_style;
    bool cursor_blink;
    bool visual_bell;
};

struct terminal_theme_picker {
    int (*count)(void *ud);
    const char *(*label)(void *ud, int index);
    bool (*selected)(void *ud, int index);
    int (*previewed)(void *ud);
    bool (*preview)(void *ud, int index);
    void (*commit)(void *ud);
    void (*cancel)(void *ud);
    int (*rescan)(void *ud);
    void *ud;
};

struct terminal_spec {
    const struct terminal_config *config;
    const struct wsap_vtcore_theme *theme;
    struct terminal_theme_picker theme_picker;
    struct tab *tab;
    const char *cwd;
    bool empty;
    struct window *(*open_window)(void *ud, struct tab *tab, const char *cwd,
                                  struct vector2i size);
    terminal_preview_capture_fn preview_capture;
    terminal_preview_release_fn preview_release;
    void *preview_ud;
    terminal_scene_paint_fn scene_paint;
    void *scene_paint_ud;
    void *ud;
};
bool terminal_mount(struct window *w, const struct terminal_spec *spec);
bool terminal_is(const struct window *w);
void terminal_prepare(struct window *w, uint64_t now_ms);
bool terminal_frame_source(struct window *w, uint64_t now_ms,
                           struct terminal_frame_source *out);
void terminal_set_theme(struct window *w, const struct wsap_vtcore_theme *theme);

bool terminal_new_tab(struct window *w);
struct window *terminal_new_window(struct window *from);
void terminal_close_tab(struct window *w, int index);
void terminal_select_tab(struct window *w, int index);
void terminal_reorder_tab(struct window *w, int from, int to);
struct tab *terminal_remove_tab(struct window *w, int index);
bool terminal_attach(struct window *w, struct tab *t, int slot);
struct window *terminal_detach_tab(struct window *w, int index);

int terminal_tab_count(const struct window *w);
int terminal_active_index(const struct window *w);
struct session *terminal_active_session(const struct window *w);
int terminal_tabbar_height(const struct window *w);

#endif
