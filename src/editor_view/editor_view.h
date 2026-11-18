#ifndef WSAP_EDITOR_VIEW_EDITOR_VIEW_H
#define WSAP_EDITOR_VIEW_EDITOR_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "editor/annotations.h"
#include "editor/document.h"
#include "editor/position.h"
#include "ui/widget.h"

/* The multiline code-editor widget. It reads and edits one editor document and
 * it draws the visible part of it. The widget keeps one retained row for each
 * document line that it has shown. An edit rebuilds only the rows that it
 * changed. */

/* How a token of a line looks. The widget resolves the color of each kind
 * through CSS, so a theme changes the palette. */
enum ui_editor_token {
    UI_EDITOR_TOKEN_TEXT = 0,
    UI_EDITOR_TOKEN_KEYWORD,
    UI_EDITOR_TOKEN_STRING,
    UI_EDITOR_TOKEN_NUMBER,
    UI_EDITOR_TOKEN_COMMENT,
};

/* One styled run of a line. `start` and `length` count bytes. */
struct ui_editor_span {
    uint32_t start;
    uint32_t length;
    enum ui_editor_token token;
};

/* Fills `out` with the spans of one line. `cap` is the free span count.
 * Returns false when the line needs more spans than `cap`. */
typedef bool (*ui_editor_tokenize_fn)(const char *text, size_t length,
                                      struct ui_editor_span *out, size_t cap,
                                      size_t *count, void *userdata);

/* A gutter mark of a line. */
enum ui_editor_mark {
    UI_EDITOR_MARK_NONE = 0,
    UI_EDITOR_MARK_BREAKPOINT,
    UI_EDITOR_MARK_GIT_ADDED,
    UI_EDITOR_MARK_GIT_MODIFIED,
    UI_EDITOR_MARK_GIT_DELETED,
};

/* Where an extra row that a decoration adds sits. */
enum ui_editor_inlay_place {
    /* One text run at the end of the line. The row grows. */
    UI_EDITOR_INLAY_INLINE = 0,
    /* A band above the line. Code lens and peek views use it. */
    UI_EDITOR_INLAY_ABOVE,
};

/* The count of rows that the last frame painted, and the count of rows that
 * rebuilt their spans in it. */
struct ui_editor_paint_stats {
    int rows_painted;
    int rows_rebuilt;
};

struct ui_editor_state;

struct ui_editor_view {
    struct ui_widget widget;
    /* The widget allocates this state on first use. An all-zero view is a
     * valid empty view. */
    struct ui_editor_state *state;
};

/* Sets the values of `view`. The view owns its document and its folds. */
void ui_editor_view_init(struct ui_editor_view *view);
/* Destroys the document and the state of `view`. */
void ui_editor_view_clear(struct ui_editor_view *view);
/* Returns the widget of `view`. A NULL view gives NULL. */
struct ui_widget *ui_editor_view_widget(struct ui_editor_view *view);

/* Opens the file at `path`. A missing file gives an empty document. */
bool ui_editor_view_open(struct ui_editor_view *view, const char *path);
/* Replaces the text. The caret moves to the start. */
bool ui_editor_view_set_text(struct ui_editor_view *view, const char *bytes,
                             size_t length);
/* Returns the document of `view`, or NULL. */
struct editor_document *ui_editor_view_document(struct ui_editor_view *view);

/* Sets the word wrap. A wrapped line takes more than one visual row. */
void ui_editor_view_set_wrap(struct ui_editor_view *view, bool wrap);
bool ui_editor_view_wrap(const struct ui_editor_view *view);
/* Sets the line number gutter. */
void ui_editor_view_set_gutter(struct ui_editor_view *view, bool shown);
bool ui_editor_view_gutter(const struct ui_editor_view *view);

/* Sets the tokenizer. A NULL function restores the default tokenizer. */
void ui_editor_view_set_tokenizer(struct ui_editor_view *view,
                                  ui_editor_tokenize_fn fn, void *userdata);

/* Copies the ranges of `folds` and collapses each one. */
bool ui_editor_view_set_folds(struct ui_editor_view *view,
                              const struct editor_folds *folds);
/* Adds one collapsed range. */
bool ui_editor_view_add_fold(struct ui_editor_view *view, uint32_t start_line,
                             uint32_t end_line, const char *label);
/* Collapses or expands the fold that starts on `line`. Returns false when no
 * fold starts there. */
bool ui_editor_view_toggle_fold(struct ui_editor_view *view, uint32_t line);
/* True when `line` is inside a collapsed range and is not its first line. */
bool ui_editor_view_line_hidden(const struct ui_editor_view *view,
                                uint32_t line);
/* The count of visible lines, so folded lines do not count. */
uint32_t ui_editor_view_visible_line_count(const struct ui_editor_view *view);

/* Borrows `diagnostics`. The widget draws a gutter dot for each one. */
void ui_editor_view_set_diagnostics(struct ui_editor_view *view,
                                    const struct editor_diagnostics *diagnostics);

/* Sets the gutter mark of `line`. */
void ui_editor_view_set_mark(struct ui_editor_view *view, uint32_t line,
                             enum ui_editor_mark mark);
enum ui_editor_mark ui_editor_view_mark(const struct ui_editor_view *view,
                                        uint32_t line);

/* Adds one decoration. The text stays alive in the view. */
bool ui_editor_view_add_inlay(struct ui_editor_view *view, uint32_t line,
                              const char *text, enum ui_editor_inlay_place place);
/* Clears the decorations. */
void ui_editor_view_clear_inlays(struct ui_editor_view *view);

/* Sets the scrollbar tick lines, such as search matches. The view copies the
 * lines. */
bool ui_editor_view_set_matches(struct ui_editor_view *view,
                                const uint32_t *lines, size_t count);

/* The caret in document positions. A UTF-32 character counts code points. */
bool ui_editor_view_set_caret(struct ui_editor_view *view,
                              struct editor_position caret);
struct editor_position ui_editor_view_caret(const struct ui_editor_view *view);
/* The caret as a byte offset of the text. */
uint64_t ui_editor_view_caret_offset(const struct ui_editor_view *view);
/* Moves the caret to the document position under a local point. */
struct editor_position ui_editor_view_position_at(const struct ui_editor_view *view,
                                                  struct vector2i point);

/* Selects from the caret to `offset`. Equal values clear the selection. */
bool ui_editor_view_select(struct ui_editor_view *view, uint64_t offset);
/* The selected byte range. Returns false when the selection is empty. */
bool ui_editor_view_selection(const struct ui_editor_view *view,
                              uint64_t *start, uint64_t *end);
/* The selection rectangles of the visible rows. Writes up to `cap` rectangles
 * and returns the count that it wrote. A NULL `out` returns the count. */
size_t ui_editor_view_selection_rects(const struct ui_editor_view *view,
                                      struct rect2i *out, size_t cap);

/* The inline IME preedit. It draws at the caret. An empty text hides it. */
void ui_editor_view_set_preedit(struct ui_editor_view *view, const char *text,
                                size_t length);
const char *ui_editor_view_preedit(const struct ui_editor_view *view,
                                   size_t *length);

/* Replaces `remove` bytes at `offset` with `insert`. The helper marks only the
 * rows that the edit changed. */
bool ui_editor_view_replace(struct ui_editor_view *view, uint64_t offset,
                            uint64_t remove, const char *insert,
                            size_t insert_length);

/* Scrolls the view. `scroll_lines` moves the first visible line. */
void ui_editor_view_scroll_lines(struct ui_editor_view *view, int lines);
void ui_editor_view_scroll_to_line(struct ui_editor_view *view, uint32_t line);
/* Sets the horizontal scroll in pixels. */
void ui_editor_view_scroll_x(struct ui_editor_view *view, int px);
uint32_t ui_editor_view_first_line(const struct ui_editor_view *view);
int ui_editor_view_scroll_x_pixels(const struct ui_editor_view *view);

/* The line that the sticky header shows at the top, or false when the view
 * shows no header. It is the innermost collapsed range above the first
 * visible line that still holds it. */
bool ui_editor_view_sticky_line(const struct ui_editor_view *view,
                                uint32_t *line);

/* The top pixel of `line` in the content area, minus the scroll offset. */
bool ui_editor_view_line_y(const struct ui_editor_view *view, uint32_t line,
                           int *y);
/* The height of the row of `line` in pixels. A hidden line has height zero. */
int ui_editor_view_line_height(const struct ui_editor_view *view, uint32_t line);
/* The count of visual rows of `line`, which word wrap can raise. */
int ui_editor_view_line_rows(const struct ui_editor_view *view, uint32_t line);

/* The stats of the last painted frame. */
struct ui_editor_paint_stats ui_editor_view_paint_stats(
    const struct ui_editor_view *view);

#endif
