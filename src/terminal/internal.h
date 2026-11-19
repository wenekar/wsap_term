#ifndef TERMINAL_INTERNAL_H
#define TERMINAL_INTERNAL_H

#include <stdalign.h>

#include "font/font.h"
#include "input/input.h"
#include "input/keybind.h"
#include "session/session.h"
#include "terminal/selection.h"
#include "terminal/terminal.h"
#include "ui/accessibility.h"
#include "ui/widgets/menu.h"
#include "ui/widgets/scrollview.h"
#include "ui/widget_ops.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"
#include <wsap_vtcore/wsap_vtcore.h>

static inline struct wsap_vtcore_vector2i term_vec_from_geom(struct vector2i v)
{
    return (struct wsap_vtcore_vector2i){ .x = v.x, .y = v.y };
}

struct terminal_preedit {
    char text[256];
    int cursor_begin, cursor_end;
    struct rect2i caret;
    uint32_t fg;
    uint32_t bg;
};

/* The byte lengths and the offsets of the accessibility document. A node
 * reports the whole document, and a read starts at its own range. */
struct accessibility_document {
    size_t history_length;
    size_t length;
    size_t caret_offset;
    size_t selection_start;
    size_t selection_end;
    uint64_t version;
    bool has_selection;
};

struct terminal_view {
    struct ui_widget widget;
    /* The snapshot view version of the scan that set blink_cells. */
    uint64_t blink_version;
    uint64_t visual_bell_until_ms;
    float scroll_remainder;
    bool hover_link;
    bool drop_hover;
    bool opening_link;
    bool blink_cells;
    /* The key ID of the press that the view declined to the app command, or
     * 0. The repeats and the release of that key follow the press. */
    unsigned declined_key;

    /* The byte offset of each logical history line in the accessibility
     * document, its byte length, and the history rows that it covers. The
     * provider reads a range from the document and keeps no text. */
    struct terminal_text_line {
        size_t offset;
        size_t bytes;
        size_t row;
        size_t rows;
    } *accessibility_lines;
    size_t accessibility_line_count;
    size_t accessibility_line_capacity;
    /* The byte length of the history part, the history rows and the first
     * line that it covers, its generation, width, and whether its last line
     * continues into the screen. That open line keeps its trailing blanks in
     * the index. open_blanks counts them: they drop when the rows below hold
     * no text. */
    size_t accessibility_history_length;
    size_t accessibility_open_blanks;
    size_t accessibility_rows;
    uint64_t accessibility_generation;
    uint64_t accessibility_first_line;
    int accessibility_cols;
    bool accessibility_open_line;
    bool accessibility_valid;
    struct ui_accessibility_text_provider accessibility_provider;
    /* The document cache of one provider publish. The first lock call takes
     * one snapshot and one measure, and every length and read call reuses
     * them. The unlock at depth zero frees them. The term lock is held for
     * the whole cache life, so other threads cannot change the term. The
     * lock depth and the cache change and are read only under the term
     * lock. The holder thread can still change the term, because the term
     * lock nests. A length or read call then compares the history
     * generation and the history rows with the values of the fill, and
     * fills the cache again when one changed. A change of the screen rows
     * alone shows at the next provider lock. */
    struct wsap_vtcore_snapshot *accessibility_snapshot;
    struct accessibility_document accessibility_document;
    uint64_t accessibility_document_generation;
    int accessibility_document_rows;
    int accessibility_lock_depth;
    bool accessibility_document_valid;
    /* The time of the last text build, the timer that sends the text of a
     * burst, and the node values of that build. A build runs at most once
     * each ACCESSIBILITY_TEXT_INTERVAL_MS. */
    uint64_t accessibility_built_ms;
    uint64_t accessibility_timer;
    uint64_t accessibility_text_version;
    size_t accessibility_caret_offset;
    size_t accessibility_selection_start;
    size_t accessibility_selection_end;
    struct wsap_vtcore *accessibility_text_term;
    bool accessibility_text_valid;

    struct terminal_preedit preedit;
    bool blink_suppressed;
};

enum ctx_action {
    CTX_ACTION_NONE = 0,
    CTX_ACTION_COPY,
    CTX_ACTION_PASTE,
    CTX_ACTION_SELECT_ALL,
    CTX_ACTION_OPEN_LINK,
    CTX_ACTION_RESET_VIEW,
    CTX_ACTION_THEMES,
    CTX_ACTION_SETTINGS,
    CTX_ACTION_THEME_BASE = 100,
};

/* One terminal content: one session and its widgets. It has no tab state. */
struct terminal {
    struct window *window;
    /* The loop of the window at creation. The content keeps it, because a
     * detached content still owns timers. */
    struct yasl_loop *loop;
    const struct terminal_config *config;
    struct terminal_host host;
    struct font_instance *font;

    /* The state of the tab content adapter of the app. A terminal that no
     * tab owns leaves it NULL. */
    void *content_state;

    struct wsap_vtcore_theme theme;
    struct terminal_theme_picker theme_picker;
    char *applied_font_family;
    struct ui_list_item *menu_theme_items;
    struct vector2i ctx_click;
    enum wsap_vtcore_color_scheme host_scheme;
    int applied_font_size;
    enum wsap_vtcore_cursor_style applied_cursor_style;
    int font_size;

    struct session session;
    struct text_selection selection;
    /* The last grid that the content widget gave the session. */
    struct vector2i grid;
    struct wsap_vtcore_snapshot *snapshot;
    /* terminal_prepare() updates the snapshot once for each frame. It hands
     * that update to terminal_frame_source() in the same frame. These fields
     * hold the frame serial and the terminal of the waiting update.
     * snapshot_frame is zero when no update waits. */
    uint64_t frame_serial;
    uint64_t snapshot_frame;
    const struct wsap_vtcore *snapshot_term;
    struct rect2i ime_caret;

    struct ui_scrollview scrollview;
    struct terminal_view view;

    bool applied_cursor_blink;
    bool exited;
    bool focused;
};

static inline struct terminal *terminal_of_view(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct terminal,
                           view.widget);
}

static inline const struct font_metrics *terminal_font_metrics(
    const struct terminal *terminal)
{
    return font_instance_metrics(terminal && terminal->font
                                     ? terminal->font
                                     : font_instance_default());
}

/* Returns the cell width of row `row`, doubled on a wide line. */
int terminal_row_cell_width(const struct terminal *terminal, int row);
/* Returns the column count of row `row`, halved on a wide line. */
int terminal_row_cols(const struct terminal *terminal, int row);

void terminal_view_init(struct terminal *terminal);
void terminal_view_attach(struct terminal *terminal);
struct rect2i terminal_view_rect(const struct terminal *terminal);
struct vector2i terminal_cell_at(const struct terminal *terminal,
                                 struct vector2i pos);
bool terminal_pointer_selects_text(const struct wsap_vtcore *t);
void terminal_view_drop_hover(struct terminal *terminal, bool over, void *ud);
void terminal_view_set_preedit(struct terminal *terminal, const char *text,
                               int cursor_begin, int cursor_end,
                               struct rect2i caret, uint32_t fg, uint32_t bg);

/* The draw stage of the frame. */
bool terminal_cursor_visible(const struct terminal *terminal, uint64_t now_ms);
void terminal_update_ime(struct terminal *terminal,
                         const struct wsap_vtcore *term);
void terminal_scan_blink(struct terminal_view *view,
                         const struct wsap_vtcore_snapshot *snap);

struct wsap_vtcore_modes terminal_input_modes(const struct terminal *terminal);
void terminal_send_input(struct terminal *terminal, const char *data,
                         size_t len);
bool terminal_key(struct terminal *terminal,
                  const struct input_key_event *key);
void terminal_text(struct terminal *terminal, const char *text, size_t len);
void terminal_copy(struct terminal *terminal);
void terminal_select_all(struct terminal *terminal);

struct ui_scroll_metrics terminal_scroll_metrics(const struct wsap_vtcore *term);

void terminal_open_ctx_menu(struct terminal *terminal, struct vector2i at);
void terminal_ctx_menu_perform(struct terminal *terminal, int action);

uint64_t terminal_next_blink_ms(struct terminal *terminal, uint64_t now_ms);
bool terminal_text_blink_on(const struct terminal *terminal, uint64_t now_ms);
uint64_t terminal_next_text_blink_ms(struct terminal *terminal,
                                     uint64_t now_ms);

#endif
