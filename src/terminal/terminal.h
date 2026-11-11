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
 * content is in transfer. The bind sets the font scale and the native
 * targets before input resumes. */
void terminal_set_window(struct terminal *terminal, struct window *window);

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
/* Captures the snapshot for the frame. Call it after the geometry commit. */
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
/* Hides the scrollbar of the content. An inactive content uses it, so a
 * flash does not come back after a tab change. */
void terminal_hide_scrollbar(struct terminal *terminal);
/* True while the content shows its scrollbar. */
bool terminal_scrollbar_visible(const struct terminal *terminal);
/* Shows or hides the file drop highlight of the content. */
void terminal_set_drop_hover(struct terminal *terminal, bool over);
void terminal_paste(struct terminal *terminal, const char *text, size_t len);
/* Runs the terminal commands of a shortcut. The owner runs the tab commands
 * and leaves the rest to this function. */
void terminal_handle_shortcut(struct terminal *terminal, enum input_shortcut sc);

#endif
