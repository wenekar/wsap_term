#ifndef WSAP_UI_SURFACE_H
#define WSAP_UI_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/css.h"
#include "ui/event.h"
#include "ui/style_context.h"
#include "ui/widget.h"
#include "util/geom.h"

struct ui_scene;
struct ui_layout;
struct ui_dialog_frame;

struct ui_surface_host {
    void (*request_frame)(void *userdata);
    void (*copy_text)(void *userdata, const char *text);
    void (*paste_text)(void *userdata);
    void (*open_url)(void *userdata, const char *target);
    uint32_t (*double_click_interval_ms)(void *userdata);
};

#define UI_DIALOG_MAX_BUTTONS 4
#define UI_SURFACE_SCRATCH_CAP 16384

struct ui_dialog_button {
    const char *text;
    int action_id;
};

struct ui_dialog_desc {
    const char *title;
    const char *message;
    struct ui_dialog_button buttons[UI_DIALOG_MAX_BUTTONS];
    int button_count;
    ui_action_fn on_action;
    void *userdata;
};

struct ui_surface {
    struct {
        struct ui_surface_host host;
        void *host_userdata;
        struct ui_widget *root;
        struct ui_widget *focused;
        struct ui_widget *captured;
        struct ui_widget *hovered;
        struct vector2i pointer;
        struct ui_widget *modal;
        struct ui_widget *restore_focus;
        struct ui_widget *default_focus;
        struct ui_widget *paste_target, *context_target;
        bool key_text_kept;
        struct vector2i viewport;
        struct ui_style_context style;
        struct ui_layout *layout;
        uint64_t deadline;
        bool frame_requested;
        bool busy;
        struct ui_widget *dialog;
        uint64_t dialog_generation;
        struct ui_dialog_frame *dialog_storage;
        struct ui_dialog_frame *retired_dialogs;
        size_t scratch_used;
        uint8_t scratch_buf[UI_SURFACE_SCRATCH_CAP];
    } impl;
};

void ui_surface_init(struct ui_surface *surface,
                     const struct ui_surface_host *host, void *host_userdata);
void ui_surface_destroy(struct ui_surface *surface);

bool ui_surface_set_root(struct ui_surface *surface, struct ui_widget *root);
struct ui_widget *ui_surface_root(const struct ui_surface *surface);

void ui_surface_set_sheet(struct ui_surface *surface,
                          const struct css_sheet *sheet);
void ui_surface_set_scale(struct ui_surface *surface,
                          struct css_scale_context scale);
struct css_scale_context ui_surface_scale(const struct ui_surface *surface);
void ui_surface_configure(struct ui_surface *surface, struct vector2i viewport,
                            struct css_scale_context scale);
void ui_surface_resize(struct ui_surface *surface, struct vector2i viewport);
struct vector2i ui_surface_viewport(const struct ui_surface *surface);

bool ui_surface_dispatch(struct ui_surface *surface,
                         const struct ui_event *event);

void ui_surface_set_default_focus(struct ui_surface *surface,
                                  struct ui_widget *widget);
void ui_surface_keep_key_text(struct ui_surface *surface);
bool ui_surface_key_text_kept(const struct ui_surface *surface);

void ui_surface_focus(struct ui_surface *surface, struct ui_widget *widget);
struct ui_widget *ui_surface_focused(const struct ui_surface *surface);
struct ui_widget *ui_surface_captured(const struct ui_surface *surface);
struct ui_widget *ui_surface_context_target(const struct ui_surface *surface);
void ui_surface_set_context_target(struct ui_surface *surface, struct ui_widget *target);
bool ui_surface_focus_next(struct ui_surface *surface);
bool ui_surface_focus_prev(struct ui_surface *surface);
void ui_surface_cancel_capture(struct ui_surface *surface);
bool ui_surface_has_modal(const struct ui_surface *surface);

void *ui_surface_scratch_alloc(struct ui_surface *surface, size_t size);
void ui_surface_scratch_reset(struct ui_surface *surface);

struct ui_widget *ui_dialog_show(struct ui_surface *surface,
                                 const struct ui_dialog_desc *desc);

void ui_dialog_close(struct ui_surface *surface);
enum ui_cursor ui_surface_cursor(const struct ui_surface *surface);

bool ui_surface_tick(struct ui_surface *surface, uint64_t now_ms);
uint64_t ui_surface_next_deadline(const struct ui_surface *surface);
bool ui_surface_copy_text(struct ui_surface *surface, const char *text);
bool ui_surface_paste_text(struct ui_surface *surface);
bool ui_surface_open_url(struct ui_surface *surface, const char *target);
uint32_t ui_surface_double_click_interval_ms(
    const struct ui_surface *surface);
void ui_surface_request_frame(struct ui_surface *surface);
bool ui_surface_frame_pending(const struct ui_surface *surface);
bool ui_surface_take_frame_request(struct ui_surface *surface);

void ui_surface_prepare(struct ui_surface *surface);
bool ui_surface_build_scene(struct ui_surface *surface,
                            struct ui_scene *scene);

#endif
