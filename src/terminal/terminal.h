#ifndef TERMINAL_TERMINAL_H
#define TERMINAL_TERMINAL_H

#include <stdbool.h>
#include <stdint.h>

#include <wsap_vtcore/wsap_vtcore.h>
#include "input/input.h"
#include "util/geom.h"

struct session;
struct window;
struct font_instance;
struct ui_widget;
struct wsap_vtcore_snapshot;
struct terminal_held_keys;

struct terminal;

/* The frame data of one content. It stays valid until the next prepare or
 * the destruction of the content. */
struct terminal_frame_source {
    struct wsap_vtcore_snapshot *snapshot;
    struct font_instance *font;
    struct rect2i rect;
    bool cursor_visible;
    bool text_blink_on;
};

struct terminal_config {
    char *command;
    char *locale;
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

/* The host reports to the owner of the content. The owner sets it once, at
 * creation, and finds the content by identity, never by an array index. */
struct terminal_host {
    void (*output)(void *ud, struct terminal *terminal);
    void (*title_changed)(void *ud, struct terminal *terminal);
    void (*exited)(void *ud, struct terminal *terminal);
    /* The held keys of the window of `terminal`, or NULL. Every terminal
     * of one window shares them, so the repeats and the release of a key
     * follow its press after a tab switch. Each terminal also keeps its
     * own record, which follows the terminal to another window. */
    struct terminal_held_keys *(*held_keys)(void *ud, struct terminal *terminal);
    void *ud;
};

struct terminal_spec {
    const struct terminal_config *config;
    const struct wsap_vtcore_theme *theme;
    struct terminal_theme_picker theme_picker;
    struct terminal_host host;
    const char *cwd;
    /* The content size in device pixels. It excludes any tab bar. A zero
     * size uses the default window size. */
    struct vector2i content_size;
    /* The font size in points. Zero uses the font size of the window. */
    int font_size;
};

/* The window is borrowed. It gives native services, such as clipboard and
 * redraw. The content keeps no ownership of it. */
struct terminal *terminal_create(struct window *window,
                                 const struct terminal_spec *spec);
void terminal_destroy(struct terminal *terminal);
/* Binds the content to another window. A null window is valid while the
 * content is in transfer. The bind applies the font settings and the font
 * scale of the new window before input resumes. */
void terminal_set_window(struct terminal *terminal, struct window *window);

/*
 * The tab content adapter state of the terminal. The terminal keeps the
 * pointer and frees nothing. A tab content adapter stores its state here,
 * so the content of the tab is the terminal itself.
 */
void *terminal_content_state(const struct terminal *terminal);
void terminal_set_content_state(struct terminal *terminal, void *state);

/* The widget that shows the content. The owner places it in its UI. Its laid
 * out size is the one source of the terminal geometry. */
struct ui_widget *terminal_widget(struct terminal *terminal);
/* The widget that receives the terminal input. */
struct ui_widget *terminal_input_widget(struct terminal *terminal);

struct session *terminal_session(struct terminal *terminal);
struct font_instance *terminal_font(const struct terminal *terminal);
const char *terminal_title(const struct terminal *terminal);
uint32_t terminal_background(const struct terminal *terminal);
int terminal_font_size(const struct terminal *terminal);
bool terminal_exited(const struct terminal *terminal);

/* Resizes the session to the cell grid of the content widget. An unchanged
 * grid is a no-op. This is the only session resize of this layer. */
void terminal_commit_geometry(struct terminal *terminal);
/* Updates the snapshot for the current frame and notes whether any cell
 * blinks. terminal_frame_source() reuses that update in the same frame. */
void terminal_prepare(struct terminal *terminal, uint64_t now_ms);
bool terminal_frame_source(struct terminal *terminal, uint64_t now_ms,
                           struct terminal_frame_source *out);

void terminal_set_theme(struct terminal *terminal,
                        const struct wsap_vtcore_theme *theme);
/* Applies the font family, the font size, and the cursor settings of the
 * window to the content. */
void terminal_restyle(struct terminal *terminal);
void terminal_adjust_font_size(struct terminal *terminal, int delta_pt);

void terminal_focus(struct terminal *terminal, bool focused);
/* Shows the scrollbar of the content for a moment. */
void terminal_show_scrollbar(struct terminal *terminal);
void terminal_hide_scrollbar(struct terminal *terminal);
bool terminal_scrollbar_visible(const struct terminal *terminal);
void terminal_set_drop_hover(struct terminal *terminal, bool over);
void terminal_paste(struct terminal *terminal, const char *text, size_t len);
/* Runs the terminal commands of a shortcut. The owner runs the tab commands
 * and leaves the rest to this function. */
void terminal_handle_shortcut(struct terminal *terminal, input_command_id sc);

/*
 * The cursor as a window overlay. The window then shows and hides the
 * cursor at each blink, and the app draws no frame for it.
 *
 * - `rect` is the cursor in canvas pixels. `color` is the cursor color.
 * - `solid_only` means: the overlay is `rect` filled with `color`. If it is
 *   false, the overlay is the pixels of the cells
 *   [span_begin, span_begin + span_cols) with a block cursor on them.
 * - `cell_key` names those pixels. Two frames with the same key have the
 *   same pixels. 0 means: not known. Make the pixels again.
 */
struct terminal_cursor_overlay {
    bool visible;
    bool solid_only;
    struct rect2i rect;
    uint32_t color;
    int span_begin;
    int span_cols;
    uint64_t cell_key;
};
/* Fills `out` for the frame of `source`. Returns false when the frame draws
 * the cursor in the grid: no focus, a preedit, a drop highlight, blinking
 * text, a bell, a hidden or non-blinking cursor, a wide row, or a cursor
 * that is not fully inside the grid. */
bool terminal_cursor_overlay_prepare(const struct terminal *terminal,
                                     const struct terminal_frame_source *source,
                                     struct vector2i canvas_size, uint64_t now_ms,
                                     struct terminal_cursor_overlay *out);
/* Returns true when the cursor blink is the only change due at `now_ms`, and
 * writes the cursor state and the time of the next wake. Returns false when
 * the window needs a frame. */
bool terminal_cursor_blink_only(struct terminal *terminal, uint64_t now_ms,
                                bool *out_visible, uint64_t *out_next_ms);

#endif
