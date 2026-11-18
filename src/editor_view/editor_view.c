#include "editor_view/editor_view.h"

#include "editor/edit.h"
#include "editor/text.h"
#include "paint/text_run.h"
#include "ui/css.h"
#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "ui/text_break.h"
#include "ui/widget_ops.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUTTER_MARK_CELLS 1
#define GUTTER_FOLD_CELLS 1
#define GUTTER_PAD_CELLS 1
#define SCROLLBAR_WIDTH 8
#define SCROLLBAR_THUMB_MIN 24
#define TOKEN_SPAN_MIN 8

/* The default palette. CSS overrides each value. */
#define COLOR_TEXT 0xFFD4D4D4
#define COLOR_KEYWORD 0xFFC586C0
#define COLOR_STRING 0xFFCE9178
#define COLOR_NUMBER 0xFFB5CEA8
#define COLOR_COMMENT 0xFF6A9955
#define COLOR_GUTTER 0xFF858585
#define COLOR_BREAKPOINT 0xFFE51400
#define COLOR_GIT_ADDED 0xFF587C0C
#define COLOR_GIT_MODIFIED 0xFF0C7D9D
#define COLOR_GIT_DELETED 0xFF94151B
#define COLOR_SELECTION 0x553B6EA9
#define COLOR_CARET 0xFFD4D4D4
#define COLOR_CURRENT_LINE 0x12FFFFFF
#define COLOR_SCROLLBAR 0x3A7F849C
#define COLOR_SCROLLBAR_TICK 0xFFE5C07B
#define COLOR_STICKY 0xFF252526
#define COLOR_PREEDIT 0x66FFFFFF

struct ui_editor_row {
    struct ui_editor_span *spans;
    uint16_t span_count;
    uint16_t span_cap;
    /* The text generation that the spans belong to. */
    uint64_t text_generation;
    /* The layout generation that the height belongs to. */
    uint64_t height_generation;
    int height;
    int rows;
    bool built;
};

struct ui_editor_fold {
    uint32_t start;
    uint32_t end;
    bool collapsed;
};

struct ui_editor_mark_entry {
    uint32_t line;
    enum ui_editor_mark mark;
};

struct ui_editor_inlay {
    uint32_t line;
    char *text;
    enum ui_editor_inlay_place place;
};

struct ui_editor_state {
    struct editor_document *document;
    const struct editor_diagnostics *diagnostics;
    struct ui_editor_row *rows;
    uint32_t row_capacity;
    /* prefix[i] is the top pixel of line i. It holds prefix_valid + 1 values. */
    uint32_t *prefix;
    uint32_t prefix_capacity;
    uint32_t prefix_valid;
    uint64_t text_generation;
    uint64_t height_generation;
    ui_editor_tokenize_fn tokenizer;
    void *tokenizer_data;
    struct ui_editor_fold *folds;
    uint32_t fold_count;
    uint32_t fold_capacity;
    struct ui_editor_mark_entry *marks;
    uint32_t mark_count;
    uint32_t mark_capacity;
    struct ui_editor_inlay *inlays;
    uint32_t inlay_count;
    uint32_t inlay_capacity;
    uint32_t *matches;
    size_t match_count;
    char *preedit;
    size_t preedit_len;
    char *scratch;
    size_t scratch_capacity;
    uint64_t caret_offset;
    uint64_t anchor_offset;
    int scroll_x;
    int first_line;
    int content_width;
    int content_height;
    int gutter_width;
    int goal_column;
    struct ui_editor_paint_stats stats;
    bool wrap;
    bool gutter;
    bool caret_valid;
};

static struct ui_editor_view *view_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_editor_view,
                           widget);
}

static struct ui_editor_state *state_of(const struct ui_editor_view *view)
{
    return view ? view->state : NULL;
}

static uint32_t line_count_of(const struct ui_editor_state *state)
{
    const struct editor_text *text = editor_document_text(state->document);
    return text ? editor_text_line_count(text) : 0;
}

/* Grows `*items` to hold `count` items of `size` bytes. */
static bool grow(void **items, uint32_t *capacity, uint32_t count, size_t size)
{
    if (count <= *capacity)
        return true;
    uint32_t next = *capacity ? *capacity : 8;
    while (next < count) {
        if (next > UINT32_MAX / 2)
            return false;
        next *= 2;
    }
    void *items_new = realloc(*items, (size_t)next * size);
    if (!items_new)
        return false;
    memset((char *)items_new + (size_t)*capacity * size, 0,
           (size_t)(next - *capacity) * size);
    *items = items_new;
    *capacity = next;
    return true;
}

static bool ensure_rows(struct ui_editor_state *state, uint32_t count)
{
    return grow((void **)&state->rows, &state->row_capacity, count,
                sizeof(*state->rows));
}

/* Loads the bytes of `line` into the scratch buffer and returns them. */
static const char *line_bytes(struct ui_editor_state *state, uint32_t line,
                              size_t *length)
{
    struct editor_line info;
    *length = 0;
    if (line >= line_count_of(state) ||
        !editor_text_line(editor_document_text(state->document), line, &info))
        return "";
    if (info.length + 1 > state->scratch_capacity) {
        size_t next = info.length + 1;
        char *scratch = realloc(state->scratch, next);
        if (!scratch)
            return "";
        state->scratch = scratch;
        state->scratch_capacity = next;
    }
    uint64_t copied = editor_text_copy(editor_document_text(state->document),
                                       info.start, info.length, state->scratch);
    state->scratch[copied] = '\0';
    *length = (size_t)copied;
    return state->scratch;
}

/* The default tokenizer. It marks comments, strings, numbers, and the
 * keywords of a few common languages. */
static const char *const default_keywords[] = {
    "auto", "break", "case", "char", "class", "const", "continue", "def",
    "default", "do", "else", "enum", "extern", "false", "fn", "for", "func",
    "function", "goto", "if", "impl", "import", "in", "int", "interface",
    "let", "long", "loop", "match", "mod", "mut", "namespace", "new", "null",
    "nullptr", "package", "private", "pub", "public", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "template", "this",
    "throw", "trait", "true", "try", "typedef", "typename", "union",
    "unsigned", "use", "using", "var", "void", "volatile", "where", "while",
};

static bool is_keyword(const char *text, size_t length)
{
    for (size_t i = 0; i < sizeof(default_keywords) / sizeof(*default_keywords);
         i++) {
        size_t len = strlen(default_keywords[i]);
        if (len == length && !memcmp(default_keywords[i], text, length))
            return true;
    }
    return false;
}

static bool is_word_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool default_tokenize(const char *text, size_t length,
                             struct ui_editor_span *out, size_t cap,
                             size_t *count, void *userdata)
{
    (void)userdata;
    size_t off = 0, n = 0;
    while (off < length) {
        enum ui_editor_token token = UI_EDITOR_TOKEN_TEXT;
        size_t end = off + 1;
        unsigned char c = (unsigned char)text[off];
        if (c == '/' && off + 1 < length && text[off + 1] == '/') {
            token = UI_EDITOR_TOKEN_COMMENT;
            end = length;
        } else if (c == '"' || c == '\'' || c == '`') {
            token = UI_EDITOR_TOKEN_STRING;
            end = off + 1;
            while (end < length) {
                if (text[end] == '\\' && end + 1 < length) {
                    end += 2;
                    continue;
                }
                if (text[end] == (char)c) {
                    end++;
                    break;
                }
                end++;
            }
        } else if (c >= '0' && c <= '9') {
            token = UI_EDITOR_TOKEN_NUMBER;
            while (end < length && is_word_char((unsigned char)text[end]))
                end++;
        } else if (is_word_char(c)) {
            while (end < length && is_word_char((unsigned char)text[end]))
                end++;
            if (is_keyword(text + off, end - off))
                token = UI_EDITOR_TOKEN_KEYWORD;
        } else {
            while (end < length && !is_word_char((unsigned char)text[end]) &&
                   text[end] != '"' && text[end] != '\'' && text[end] != '`' &&
                   !(text[end] == '/' && end + 1 < length && text[end + 1] == '/'))
                end++;
        }
        if (n == cap)
            return false;
        out[n++] = (struct ui_editor_span){
            .start = (uint32_t)off,
            .length = (uint32_t)(end - off),
            .token = token,
        };
        off = end;
    }
    *count = n;
    return true;
}

/* Builds the spans of the row of `line` again when the text changed. */
static struct ui_editor_row *row_spans(struct ui_editor_state *state,
                                       uint32_t line, bool *rebuilt)
{
    if (!ensure_rows(state, line + 1))
        return NULL;
    struct ui_editor_row *row = &state->rows[line];
    *rebuilt = false;
    if (row->built)
        return row;
    size_t length = 0;
    const char *text = line_bytes(state, line, &length);
    ui_editor_tokenize_fn fn = state->tokenizer ? state->tokenizer
                                                : default_tokenize;
    for (;;) {
        size_t cap = row->span_cap ? row->span_cap : TOKEN_SPAN_MIN;
        if (!row->spans) {
            struct ui_editor_span *spans =
                malloc(cap * sizeof(*spans));
            if (!spans)
                return NULL;
            row->spans = spans;
            row->span_cap = (uint16_t)cap;
        }
        size_t count = 0;
        if (fn(text, length, row->spans, row->span_cap, &count,
               state->tokenizer_data)) {
            row->span_count = (uint16_t)count;
            break;
        }
        if (row->span_cap >= UINT16_MAX / 2) {
            free(row->spans);
            row->spans = NULL;
            row->span_cap = 0;
            return NULL;
        }
        free(row->spans);
        row->spans = NULL;
        row->span_cap = (uint16_t)(row->span_cap * 2);
    }
    row->text_generation = state->text_generation;
    row->built = true;
    *rebuilt = true;
    return row;
}

static int inlay_extra_rows(const struct ui_editor_state *state, uint32_t line,
                            enum ui_editor_inlay_place place)
{
    int count = 0;
    for (uint32_t i = 0; i < state->inlay_count; i++)
        if (state->inlays[i].line == line && state->inlays[i].place == place)
            count++;
    return count;
}

static bool diagnostics_at(const struct ui_editor_state *state, uint32_t line)
{
    if (!state->diagnostics)
        return false;
    struct editor_line info;
    if (!editor_text_line(editor_document_text(state->document), line, &info))
        return false;
    struct editor_diagnostic diagnostic;
    return editor_diagnostics_at(state->diagnostics, info.start, &diagnostic);
}

/* True when `line` is inside a collapsed fold and is not its first line. */
static bool folded(const struct ui_editor_state *state, uint32_t line)
{
    for (uint32_t i = 0; i < state->fold_count; i++) {
        const struct ui_editor_fold *fold = &state->folds[i];
        if (fold->collapsed && line > fold->start && line <= fold->end)
            return true;
    }
    return false;
}

static int row_height(struct ui_widget *widget, struct ui_editor_state *state,
                      uint32_t line, int *rows)
{
    if (!ensure_rows(state, line + 1))
        return 0;
    struct ui_editor_row *row = &state->rows[line];
    *rows = 1;
    if (folded(state, line)) {
        row->height = 0;
        row->rows = 0;
        return 0;
    }
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    int segments = 1;
    if (state->wrap && state->content_width > 0 && style.advance > 0) {
        size_t length = 0;
        const char *text = line_bytes(state, line, &length);
        int cells = ui_text_run_cells(text, length);
        int per_row = state->content_width / style.advance;
        segments = per_row > 0 ? (cells + per_row - 1) / per_row : 1;
        if (segments < 1)
            segments = 1;
    }
    row->rows = segments;
    row->height = segments * style.cell_h +
                  inlay_extra_rows(state, line, UI_EDITOR_INLAY_INLINE) *
                      style.cell_h +
                  inlay_extra_rows(state, line, UI_EDITOR_INLAY_ABOVE) *
                      style.cell_h;
    row->height_generation = state->height_generation;
    *rows = segments;
    return row->height;
}

/* Measures the prefix up to `line`, so prefix[line] is the top of the line. */
static void ensure_prefix(struct ui_widget *widget,
                          struct ui_editor_state *state, uint32_t line)
{
    uint32_t count = line_count_of(state);
    if (line >= count)
        line = count ? count - 1 : 0;
    if (state->prefix_capacity < count + 1) {
        uint32_t capacity = state->prefix_capacity ? state->prefix_capacity : 64;
        while (capacity < count + 1)
            capacity *= 2;
        uint32_t *prefix = realloc(state->prefix, capacity * sizeof(*prefix));
        if (!prefix)
            return;
        state->prefix = prefix;
        state->prefix_capacity = capacity;
    }
    if (state->prefix_valid > count)
        state->prefix_valid = count;
    if (state->prefix_valid == 0)
        state->prefix[0] = 0;
    while (state->prefix_valid <= line && state->prefix_valid < count) {
        int rows = 1;
        int height = row_height(widget, state, state->prefix_valid, &rows);
        uint32_t next = state->prefix[state->prefix_valid] +
                        (uint32_t)(height > 0 ? height : 0);
        state->prefix[state->prefix_valid + 1] = next;
        state->prefix_valid++;
    }
}

static int content_height(struct ui_widget *widget,
                          struct ui_editor_state *state)
{
    uint32_t count = line_count_of(state);
    if (!count)
        return 0;
    ensure_prefix(widget, state, count - 1);
    return (int)state->prefix[count];
}

/* The line that holds pixel `y` of the content. */
static uint32_t line_at_y(struct ui_widget *widget,
                          struct ui_editor_state *state, int y)
{
    uint32_t count = line_count_of(state);
    if (!count)
        return 0;
    ensure_prefix(widget, state, count - 1);
    uint32_t lo = 0, hi = state->prefix_valid;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((int)state->prefix[mid] <= y)
            lo = mid;
        else
            hi = mid;
    }
    return lo < count ? lo : count - 1;
}

static void invalidate_heights(struct ui_editor_state *state,
                               uint32_t from_line)
{
    state->height_generation++;
    if (state->prefix_valid > from_line)
        state->prefix_valid = from_line;
}

/* Drops the spans of every row. Use it after a full text replacement. */
static void invalidate_rows(struct ui_editor_state *state)
{
    for (uint32_t line = 0; line < state->row_capacity; line++) {
        state->rows[line].built = false;
        state->rows[line].text_generation = state->text_generation - 1;
    }
}

static void invalidate_paint(struct ui_editor_view *view)
{
    ui_widget_invalidate(&view->widget, UI_WIDGET_INVALIDATE_PAINT);
}

/* Marks the rows of the lines [first, last] as changed. */
static void mark_lines(struct ui_editor_state *state, uint32_t first,
                       uint32_t last)
{
    if (!ensure_rows(state, last + 1))
        return;
    for (uint32_t line = first; line <= last; line++) {
        state->rows[line].built = false;
        state->rows[line].text_generation = state->text_generation - 1;
    }
    invalidate_heights(state, first);
}

static void changed(struct ui_editor_view *view, uint32_t first_line,
                    uint32_t last_line)
{
    struct ui_editor_state *state = view->state;
    state->text_generation++;
    mark_lines(state, first_line, last_line);
    invalidate_paint(view);
}

static uint32_t line_of(const struct ui_editor_state *state, uint64_t offset)
{
    uint32_t line = 0;
    editor_text_locate(editor_document_text(state->document), offset, &line,
                       NULL);
    return line;
}

static struct editor_line line_info(const struct ui_editor_state *state,
                                    uint32_t line)
{
    struct editor_line info = {0};
    editor_text_line(editor_document_text(state->document), line, &info);
    return info;
}

static int gutter_width(struct ui_widget *widget, struct ui_editor_state *state)
{
    if (!state->gutter)
        return 0;
    uint32_t count = line_count_of(state);
    int digits = 1;
    for (uint32_t lines = count; lines >= 10; lines /= 10)
        digits++;
    int advance = ui_widget_text_style(&widget->style_scale).advance;
    return (GUTTER_MARK_CELLS + GUTTER_FOLD_CELLS + digits +
            GUTTER_PAD_CELLS) * advance;
}

static int text_width(struct ui_widget *widget, struct ui_editor_state *state,
                      struct rect2i content)
{
    int gutter = gutter_width(widget, state);
    int width = content.width - gutter - SCROLLBAR_WIDTH;
    return width > 0 ? width : 0;
}

static void layout_state(struct ui_widget *widget, struct ui_editor_state *state,
                         struct rect2i content)
{
    state->gutter_width = gutter_width(widget, state);
    int width = text_width(widget, state, content);
    if (width != state->content_width) {
        state->content_width = width;
        invalidate_heights(state, 0);
    }
    state->content_height = content.height;
}

static int caret_cells(struct ui_editor_state *state, uint32_t line,
                       uint64_t offset)
{
    struct editor_line info = line_info(state, line);
    if (offset <= info.start)
        return 0;
    if (offset > info.start + info.length)
        offset = info.start + info.length;
    size_t length = 0;
    const char *text = line_bytes(state, line, &length);
    return ui_text_run_cells(text, (size_t)(offset - info.start));
}

/* The cell column that the caret tries to keep during a vertical move. */
static int goal_cell(struct ui_widget *widget, struct ui_editor_state *state)
{
    if (state->goal_column >= 0)
        return state->goal_column;
    (void)widget;
    uint32_t line = line_of(state, state->caret_offset);
    struct editor_line info = line_info(state, line);
    size_t length = 0;
    const char *text = line_bytes(state, line, &length);
    uint64_t column = state->caret_offset - info.start;
    if (column > length)
        column = length;
    return ui_text_run_cells(text, (size_t)column);
}

static uint64_t offset_at_cell(struct ui_editor_state *state, uint32_t line,
                               int cell, int advance)
{
    struct editor_line info = line_info(state, line);
    size_t length = 0;
    const char *text = line_bytes(state, line, &length);
    if (!length)
        return info.start;
    size_t offset = ui_text_offset_at_x(text, length,
                                        (int64_t)cell * advance, advance);
    return info.start + offset;
}

static void set_caret_offsets(struct ui_editor_view *view, uint64_t caret,
                              uint64_t anchor)
{
    struct ui_editor_state *state = view->state;
    uint64_t length = editor_text_length(editor_document_text(state->document));
    state->caret_offset = caret > length ? length : caret;
    state->anchor_offset = anchor > length ? length : anchor;
    state->caret_valid = true;
    state->goal_column = -1;
    invalidate_paint(view);
}

/* Moves the first visible line so that the caret row is inside the view. */
static void reveal_caret(struct ui_editor_view *view)
{
    struct ui_editor_state *state = view->state;
    if (!state->caret_valid || state->content_height <= 0)
        return;
    uint32_t line = line_of(state, state->caret_offset);
    uint32_t anchor = line > (uint32_t)state->first_line
                          ? line : (uint32_t)state->first_line;
    ensure_prefix(&view->widget, state, anchor);
    int top = (int)state->prefix[line];
    int height = ui_editor_view_line_height(view, line);
    int scroll_y = (int)state->prefix[state->first_line];
    if (top < scroll_y)
        ui_editor_view_scroll_to_line(view, line);
    else if (top + height > scroll_y + state->content_height)
        ui_editor_view_scroll_to_line(view, line);
}

/* Changes the text and keeps the caret and the row cache correct. */
bool ui_editor_view_replace(struct ui_editor_view *view, uint64_t offset,
                            uint64_t remove, const char *insert,
                            size_t insert_length)
{
    struct ui_editor_state *state = state_of(view);
    if (!state || !state->document)
        return false;
    uint64_t length = editor_text_length(editor_document_text(state->document));
    if (offset > length)
        return false;
    if (remove > length - offset)
        remove = length - offset;
    uint32_t first = line_of(state, offset);
    uint32_t last = line_of(state, offset + remove);
    if (!editor_document_replace(state->document, offset, remove, insert,
                                 insert_length))
        return false;
    uint32_t new_last = line_of(state, offset + insert_length);
    if (new_last > last)
        last = new_last;
    changed(view, first, last);
    /* The line count can change, so the rows of every later line move. */
    uint32_t count = line_count_of(state);
    if (state->row_capacity > count && ensure_rows(state, count))
        memset(state->rows + count, 0,
               (size_t)(state->row_capacity - count) * sizeof(*state->rows));
    uint64_t caret = offset + insert_length;
    set_caret_offsets(view, caret, caret);
    reveal_caret(view);
    return true;
}

/* Sets the caret from a document position and keeps the selection when
 * `extend` is true. */
static void move_caret(struct ui_editor_view *view, uint64_t offset,
                       bool extend)
{
    struct ui_editor_state *state = view->state;
    uint64_t anchor = extend ? state->anchor_offset : offset;
    set_caret_offsets(view, offset, anchor);
    reveal_caret(view);
}

static void move_vertical(struct ui_editor_view *view, int delta, bool extend)
{
    struct ui_editor_state *state = view->state;
    struct ui_text_style style = ui_widget_text_style(&view->widget.style_scale);
    int cell = goal_cell(&view->widget, state);
    uint32_t count = line_count_of(state);
    uint32_t line = line_of(state, state->caret_offset);
    int step = delta < 0 ? -1 : 1;
    int left = delta < 0 ? -delta : delta;
    while (left > 0) {
        uint32_t next = line + (uint32_t)step;
        while (next < count && folded(state, next))
            next = (uint32_t)((int)next + step);
        if (next >= count)
            break;
        line = next;
        left--;
    }
    uint64_t offset = offset_at_cell(state, line, cell, style.advance);
    state->goal_column = cell;
    uint64_t anchor = extend ? state->anchor_offset : offset;
    set_caret_offsets(view, offset, anchor);
    state->goal_column = cell;
    reveal_caret(view);
}

static void move_horizontal(struct ui_editor_view *view, bool left, bool extend)
{
    struct ui_editor_state *state = view->state;
    const struct editor_text *text = editor_document_text(state->document);
    uint32_t line = 0;
    uint64_t column = 0;
    if (!editor_text_locate(text, state->caret_offset, &line, &column))
        return;
    struct editor_line info = line_info(state, line);
    size_t length = 0;
    const char *bytes = line_bytes(state, line, &length);
    uint64_t offset = state->caret_offset;
    if (left) {
        if (column == 0) {
            if (line == 0)
                return;
            struct editor_line prev = line_info(state, line - 1);
            move_caret(view, prev.start + prev.length, extend);
            return;
        }
        size_t pos = 0;
        while (pos < column && pos < length) {
            int width = 0;
            size_t next = ui_text_cluster_next(bytes, length, pos, &width);
            if (next >= column)
                break;
            pos = next;
        }
        offset = info.start + pos;
    } else {
        if (column >= info.length) {
            if (line + 1 >= line_count_of(state))
                return;
            struct editor_line next = line_info(state, line + 1);
            move_caret(view, next.start, extend);
            return;
        }
        int width = 0;
        offset = info.start +
                 ui_text_cluster_next(bytes, length, (size_t)column, &width);
    }
    move_caret(view, offset, extend);
}

static void move_home(struct ui_editor_view *view, bool extend)
{
    struct ui_editor_state *state = view->state;
    uint32_t line = line_of(state, state->caret_offset);
    /* The first non-blank byte, as most editors do on a second press. */
    struct editor_line info = line_info(state, line);
    size_t length = 0;
    const char *bytes = line_bytes(state, line, &length);
    size_t indent = 0;
    while (indent < length && (bytes[indent] == ' ' || bytes[indent] == '\t'))
        indent++;
    uint64_t offset = indent && state->caret_offset != info.start + indent
                          ? info.start + indent
                          : info.start;
    move_caret(view, offset, extend);
}

static void move_end(struct ui_editor_view *view, bool extend)
{
    struct ui_editor_state *state = view->state;
    uint32_t line = line_of(state, state->caret_offset);
    struct editor_line info = line_info(state, line);
    move_caret(view, info.start + info.length, extend);
}

static void move_page(struct ui_editor_view *view, bool up, bool extend)
{
    struct ui_editor_state *state = view->state;
    struct ui_text_style style = ui_widget_text_style(&view->widget.style_scale);
    int per_page = style.cell_h > 0
                       ? state->content_height / style.cell_h - 1 : 1;
    if (per_page < 1)
        per_page = 1;
    move_vertical(view, up ? -per_page : per_page, extend);
}

static bool selection_range(const struct ui_editor_state *state,
                            uint64_t *start, uint64_t *end)
{
    if (state->caret_offset == state->anchor_offset)
        return false;
    *start = state->caret_offset < state->anchor_offset ? state->caret_offset
                                                        : state->anchor_offset;
    *end = state->caret_offset < state->anchor_offset ? state->anchor_offset
                                                      : state->caret_offset;
    return true;
}

static bool copy_selection(struct ui_editor_view *view, bool cut)
{
    struct ui_editor_state *state = view->state;
    uint64_t start, end;
    if (!selection_range(state, &start, &end))
        return false;
    size_t length = (size_t)(end - start);
    char *copy = malloc(length + 1);
    if (!copy)
        return false;
    editor_text_copy(editor_document_text(state->document), start, length, copy);
    copy[length] = '\0';
    bool copied = ui_surface_copy_text(ui_widget_surface(&view->widget), copy);
    free(copy);
    if (copied && cut)
        ui_editor_view_replace(view, start, end - start, NULL, 0);
    return copied;
}

static void insert_text(struct ui_editor_view *view, const char *bytes,
                        size_t length)
{
    struct ui_editor_state *state = view->state;
    uint64_t start, end;
    if (selection_range(state, &start, &end))
        ui_editor_view_replace(view, start, end - start, bytes, length);
    else
        ui_editor_view_replace(view, state->caret_offset, 0, bytes, length);
}

static void insert_newline(struct ui_editor_view *view)
{
    struct ui_editor_state *state = view->state;
    struct editor_language language = { .name = "text", .indent = "    " };
    uint32_t line = line_of(state, state->caret_offset);
    struct editor_line info = line_info(state, line);
    size_t length = 0;
    const char *bytes = line_bytes(state, line, &length);
    size_t column = state->caret_offset > info.start
                        ? (size_t)(state->caret_offset - info.start) : 0;
    if (column > length)
        column = length;
    char indent[256];
    size_t take = editor_smart_indent(bytes, length, &language, column, indent,
                                      sizeof(indent));
    char text[257];
    text[0] = '\n';
    memcpy(text + 1, indent, take);
    insert_text(view, text, take + 1);
}

static void delete_backward(struct ui_editor_view *view)
{
    struct ui_editor_state *state = view->state;
    uint64_t start, end;
    if (selection_range(state, &start, &end)) {
        ui_editor_view_replace(view, start, end - start, NULL, 0);
        return;
    }
    if (!state->caret_offset)
        return;
    uint32_t line = 0;
    uint64_t column = 0;
    const struct editor_text *text = editor_document_text(state->document);
    if (!editor_text_locate(text, state->caret_offset, &line, &column))
        return;
    size_t length = 0;
    const char *bytes = line_bytes(state, line, &length);
    uint64_t remove = 1;
    if (column > 0) {
        size_t pos = 0;
        while (pos < column && pos < length) {
            int width = 0;
            size_t next = ui_text_cluster_next(bytes, length, pos, &width);
            if (next >= column)
                break;
            pos = next;
        }
        remove = column - pos;
    }
    ui_editor_view_replace(view, state->caret_offset - remove, remove, NULL, 0);
}

static void delete_forward(struct ui_editor_view *view)
{
    struct ui_editor_state *state = view->state;
    uint64_t start, end;
    if (selection_range(state, &start, &end)) {
        ui_editor_view_replace(view, start, end - start, NULL, 0);
        return;
    }
    uint64_t length = editor_text_length(editor_document_text(state->document));
    if (state->caret_offset >= length)
        return;
    uint32_t line = 0;
    uint64_t column = 0;
    const struct editor_text *text = editor_document_text(state->document);
    if (!editor_text_locate(text, state->caret_offset, &line, &column))
        return;
    struct editor_line info = line_info(state, line);
    size_t line_length = 0;
    const char *bytes = line_bytes(state, line, &line_length);
    uint64_t remove = 1;
    if (column < info.length) {
        int width = 0;
        remove = ui_text_cluster_next(bytes, line_length, (size_t)column, &width);
        remove = remove > column ? remove - column : 1;
    }
    ui_editor_view_replace(view, state->caret_offset, remove, NULL, 0);
}

static void scroll_pixels(struct ui_editor_view *view, int delta)
{
    struct ui_editor_state *state = view->state;
    int total = content_height(&view->widget, state);
    if (!state->prefix)
        return;
    int max = total - state->content_height;
    if (max < 0)
        max = 0;
    int scroll = (int)state->prefix[state->first_line] + delta;
    if (scroll < 0)
        scroll = 0;
    if (scroll > max)
        scroll = max;
    uint32_t line = line_at_y(&view->widget, state, scroll);
    state->first_line = (int)line;
    invalidate_paint(view);
}

static void scroll_x_by(struct ui_editor_view *view, int delta)
{
    struct ui_editor_state *state = state_of(view);
    if (!state)
        return;
    int scroll = state->scroll_x + delta;
    if (scroll < 0)
        scroll = 0;
    if (scroll != state->scroll_x) {
        state->scroll_x = scroll;
        invalidate_paint(view);
    }
}

/* --- gutter ------------------------------------------------------------ */

static enum ui_editor_mark mark_of(const struct ui_editor_state *state,
                                   uint32_t line)
{
    for (uint32_t i = 0; i < state->mark_count; i++)
        if (state->marks[i].line == line)
            return state->marks[i].mark;
    return UI_EDITOR_MARK_NONE;
}

static bool fold_starts(const struct ui_editor_state *state, uint32_t line,
                        bool *collapsed)
{
    for (uint32_t i = 0; i < state->fold_count; i++) {
        if (state->folds[i].start == line) {
            *collapsed = state->folds[i].collapsed;
            return true;
        }
    }
    return false;
}

static void toggle_line_fold(struct ui_editor_view *view, uint32_t line)
{
    struct ui_editor_state *state = view->state;
    for (uint32_t i = 0; i < state->fold_count; i++) {
        if (state->folds[i].start != line)
            continue;
        state->folds[i].collapsed = !state->folds[i].collapsed;
        invalidate_heights(state, line);
        uint32_t count = line_count_of(state);
        if (ensure_rows(state, count))
            for (uint32_t row = line; row < count; row++)
                state->rows[row].height_generation = state->height_generation - 1;
        invalidate_paint(view);
        return;
    }
}

static void toggle_breakpoint(struct ui_editor_view *view, uint32_t line)
{
    enum ui_editor_mark mark = ui_editor_view_mark(view, line);
    ui_editor_view_set_mark(view, line,
                            mark == UI_EDITOR_MARK_BREAKPOINT
                                ? UI_EDITOR_MARK_NONE
                                : UI_EDITOR_MARK_BREAKPOINT);
}

/* --- layout and paint -------------------------------------------------- */

static struct rect2i content_rect(const struct ui_editor_view *view,
                                  struct vector2i size)
{
    struct rect2i fill = rect2i_inset((struct rect2i){ .size = size },
                                      ui_widget_border_width(&view->widget));
    return rect2i_apply_insets(fill, ui_ctx_insets(&view->widget.style_scale,
                                                  view->widget.style.padding,
                                                  0));
}

static void paint_run(struct ui_paint_context *paint, struct rect2i rect,
                      struct ui_text_style style, uint32_t color,
                      const char *text, size_t length)
{
    rect.width = geom_sat_i((int64_t)ui_text_run_cells(text, length) *
                            style.advance);
    if (rect.width > 0)
        ui_paint_text(paint, rect, style, color, text, length);
}

struct editor_part {
    ui_part_id id;
    const char *name;
    uint32_t fallback;
};

enum {
    PART_TEXT = 0,
    PART_KEYWORD,
    PART_STRING,
    PART_NUMBER,
    PART_COMMENT,
    PART_GUTTER,
    PART_SELECTION,
    PART_CURRENT,
    PART_CARET,
    PART_SCROLLBAR,
    PART_TICK,
    PART_STICKY,
    PART_PREEDIT,
    PART_COUNT,
};

static struct editor_part editor_parts[PART_COUNT] = {
    [PART_TEXT] = { .name = "editor-text", .fallback = COLOR_TEXT },
    [PART_KEYWORD] = { .name = "editor-keyword", .fallback = COLOR_KEYWORD },
    [PART_STRING] = { .name = "editor-string", .fallback = COLOR_STRING },
    [PART_NUMBER] = { .name = "editor-number", .fallback = COLOR_NUMBER },
    [PART_COMMENT] = { .name = "editor-comment", .fallback = COLOR_COMMENT },
    [PART_GUTTER] = { .name = "editor-gutter", .fallback = COLOR_GUTTER },
    [PART_SELECTION] = { .name = "editor-selection", .fallback = COLOR_SELECTION },
    [PART_CURRENT] = { .name = "editor-current-line", .fallback = COLOR_CURRENT_LINE },
    [PART_CARET] = { .name = "editor-caret", .fallback = COLOR_CARET },
    [PART_SCROLLBAR] = { .name = "editor-scrollbar", .fallback = COLOR_SCROLLBAR },
    [PART_TICK] = { .name = "editor-scrollbar-tick", .fallback = COLOR_SCROLLBAR_TICK },
    [PART_STICKY] = { .name = "editor-sticky", .fallback = COLOR_STICKY },
    [PART_PREEDIT] = { .name = "editor-preedit", .fallback = COLOR_PREEDIT },
};

static void editor_parts_init(void)
{
    if (editor_parts[0].id)
        return;
    for (int i = 0; i < PART_COUNT; i++)
        editor_parts[i].id = ui_part_intern(editor_parts[i].name);
}

/* Resolves the color of one token kind and adds the widget opacity. */
static uint32_t part_color(const struct ui_editor_view *view, int part)
{
    struct css_style style = ui_widget_part(&view->widget, editor_parts[part].id,
                                            CSS_PSEUDO_NONE);
    uint32_t color = style.has_text_color ? style.text_color
                                          : editor_parts[part].fallback;
    float opacity = css_style_effective_opacity(&view->widget.style) *
                    css_style_effective_opacity(&style);
    return ui_color_opacity(color, opacity);
}

/* Paints the selection and the current-line band of the visible rows. */
static void paint_selection(struct ui_editor_view *view,
                            struct ui_paint_context *paint,
                            struct rect2i content, struct ui_text_style style)
{
    struct ui_editor_state *state = view->state;
    uint32_t count = line_count_of(state);
    uint32_t line = (uint32_t)state->first_line;
    ensure_prefix(&view->widget, state, line);
    int y = content.y;
    int scroll_y = (int)state->prefix[line];
    uint64_t sel_start = 0, sel_end = 0;
    bool selected = selection_range(state, &sel_start, &sel_end);
    uint32_t caret_line = state->caret_valid ? line_of(state, state->caret_offset)
                                             : UINT32_MAX;
    while (line < count && y < content.y + content.height) {
        int rows = 1;
        int height = row_height(&view->widget, state, line, &rows);
        if (height > 0) {
            if (line == caret_line) {
                ui_paint_rect(paint, (struct rect2i){
                    .pos = { .x = content.x, .y = y },
                    .size = { .x = content.width, .y = height },
                }, part_color(view, PART_CURRENT));
            }
            if (selected) {
                struct editor_line info = line_info(state, line);
                uint64_t lo = sel_start > info.start ? sel_start : info.start;
                uint64_t hi = sel_end < info.start + info.length
                                  ? sel_end : info.start + info.length;
                if (sel_end > info.start + info.length)
                    hi = info.start + info.length;
                if (lo < hi) {
                    int before = caret_cells(state, line, lo);
                    int cells = caret_cells(state, line, hi) - before;
                    ui_paint_rect(paint, (struct rect2i){
                        .pos = { .x = content.x + state->gutter_width +
                                        before * style.advance - state->scroll_x,
                                 .y = y },
                        .size = { .x = cells * style.advance, .y = style.cell_h },
                    }, part_color(view, PART_SELECTION));
                }
            }
            y += height;
        }
        line++;
    }
    (void)scroll_y;
}

static void paint_inlays(struct ui_editor_view *view,
                         struct ui_paint_context *paint, struct rect2i content,
                         struct ui_text_style style, uint32_t line, int y)
{
    struct ui_editor_state *state = view->state;
    int band = y;
    for (uint32_t i = 0; i < state->inlay_count; i++) {
        const struct ui_editor_inlay *inlay = &state->inlays[i];
        if (inlay->line != line || inlay->place != UI_EDITOR_INLAY_ABOVE)
            continue;
        struct rect2i rect = {
            .pos = { .x = content.x + state->gutter_width - state->scroll_x,
                     .y = band },
            .size = { .x = content.width, .y = style.cell_h },
        };
        paint_run(paint, rect, style, part_color(view, PART_COMMENT),
                  inlay->text, strlen(inlay->text));
        band += style.cell_h;
    }
}

static void paint_inline_inlays(struct ui_editor_view *view,
                                struct ui_paint_context *paint,
                                struct rect2i row, struct ui_text_style style,
                                uint32_t line, int cells)
{
    struct ui_editor_state *state = view->state;
    int y = row.y + style.cell_h;
    for (uint32_t i = 0; i < state->inlay_count; i++) {
        const struct ui_editor_inlay *inlay = &state->inlays[i];
        if (inlay->line != line || inlay->place != UI_EDITOR_INLAY_INLINE)
            continue;
        struct rect2i rect = {
            .pos = { .x = row.x + cells * style.advance, .y = y },
            .size = { .x = row.width, .y = style.cell_h },
        };
        paint_run(paint, rect, style, part_color(view, PART_COMMENT),
                  inlay->text, strlen(inlay->text));
        y += style.cell_h;
    }
}

static void paint_rows(struct ui_editor_view *view, struct ui_paint_context *paint,
                       struct rect2i content, struct ui_text_style style)
{
    struct ui_editor_state *state = view->state;
    uint32_t count = line_count_of(state);
    uint32_t line = (uint32_t)state->first_line;
    ensure_prefix(&view->widget, state, line);
    int y = content.y;
    state->stats.rows_painted = 0;
    state->stats.rows_rebuilt = 0;
    int per_row = style.advance > 0 && state->content_width > 0
                      ? state->content_width / style.advance : 0;
    while (line < count && y < content.y + content.height) {
        int rows = 1;
        int height = row_height(&view->widget, state, line, &rows);
        if (height == 0) {
            line++;
            continue;
        }
        paint_inlays(view, paint, content, style, line, y);
        int band = y + inlay_extra_rows(state, line, UI_EDITOR_INLAY_ABOVE) *
                           style.cell_h;
        size_t length = 0;
        const char *bytes = line_bytes(state, line, &length);
        bool rebuilt = false;
        struct ui_editor_row *row = row_spans(state, line, &rebuilt);
        if (rebuilt)
            state->stats.rows_rebuilt++;
        state->stats.rows_painted += rows;
        int row_index = 0;
        size_t offset = 0;
        while (offset < length && row_index < rows) {
            size_t take = length - offset;
            if (per_row > 0) {
                int cells = 0;
                size_t pos = offset;
                while (pos < length) {
                    int width = 0;
                    size_t next = ui_text_cluster_next(bytes, length, pos, &width);
                    if (cells + width > per_row)
                        break;
                    cells += width;
                    pos = next;
                }
                take = pos > offset ? pos - offset : length - offset;
            }
            int cells = ui_text_run_cells(bytes + offset, take);
            struct rect2i rect = {
                .pos = { .x = content.x + state->gutter_width +
                                state->scroll_x * -1,
                         .y = band + row_index * style.cell_h },
                .size = { .x = cells * style.advance, .y = style.cell_h },
            };
            if (row) {
                uint32_t run_start = (uint32_t)offset;
                uint32_t run_end = (uint32_t)(offset + take);
                for (uint16_t i = 0; i < row->span_count; i++) {
                    const struct ui_editor_span *span = &row->spans[i];
                    uint32_t start = span->start > run_start ? span->start : run_start;
                    uint32_t end = span->start + span->length < run_end
                                       ? span->start + span->length : run_end;
                    if (start >= end)
                        continue;
                    int part = PART_TEXT;
                    switch (span->token) {
                    case UI_EDITOR_TOKEN_KEYWORD: part = PART_KEYWORD; break;
                    case UI_EDITOR_TOKEN_STRING: part = PART_STRING; break;
                    case UI_EDITOR_TOKEN_NUMBER: part = PART_NUMBER; break;
                    case UI_EDITOR_TOKEN_COMMENT: part = PART_COMMENT; break;
                    default: part = PART_TEXT; break;
                    }
                    int before = ui_text_run_cells(bytes + offset,
                                                   start - offset);
                    paint_run(paint, (struct rect2i){
                        .pos = { .x = rect.x + before * style.advance,
                                 .y = rect.y },
                        .size = { .x = rect.width, .y = rect.height },
                    }, style, part_color(view, part), bytes + start, end - start);
                }
            }
            paint_inline_inlays(view, paint, rect, style, line,
                                ui_text_run_cells(bytes + offset, take));
            offset += take;
            row_index++;
        }
        y += height;
        line++;
    }
}

static void paint_gutter(struct ui_editor_view *view,
                         struct ui_paint_context *paint, struct rect2i content,
                         struct ui_text_style style)
{
    struct ui_editor_state *state = view->state;
    if (!state->gutter_width)
        return;
    uint32_t count = line_count_of(state);
    uint32_t line = (uint32_t)state->first_line;
    ensure_prefix(&view->widget, state, line);
    int y = content.y;
    uint32_t caret_line = state->caret_valid ? line_of(state, state->caret_offset)
                                             : UINT32_MAX;
    int advance = style.advance;
    int digits_x = content.x + (GUTTER_MARK_CELLS + GUTTER_FOLD_CELLS) * advance;
    while (line < count && y < content.y + content.height) {
        int rows = 1;
        int height = row_height(&view->widget, state, line, &rows);
        if (height == 0) {
            line++;
            continue;
        }
        int band = y + inlay_extra_rows(state, line, UI_EDITOR_INLAY_ABOVE) *
                           style.cell_h;
        enum ui_editor_mark mark = mark_of(state, line);
        uint32_t mark_color = 0;
        switch (mark) {
        case UI_EDITOR_MARK_BREAKPOINT: mark_color = COLOR_BREAKPOINT; break;
        case UI_EDITOR_MARK_GIT_ADDED: mark_color = COLOR_GIT_ADDED; break;
        case UI_EDITOR_MARK_GIT_MODIFIED: mark_color = COLOR_GIT_MODIFIED; break;
        case UI_EDITOR_MARK_GIT_DELETED: mark_color = COLOR_GIT_DELETED; break;
        default: break;
        }
        if (mark_color) {
            ui_paint_rect(paint, (struct rect2i){
                .pos = { .x = content.x, .y = band },
                .size = { .x = advance, .y = height },
            }, ui_color_opacity(mark_color,
                                css_style_effective_opacity(&view->widget.style)));
        }
        if (diagnostics_at(state, line)) {
            ui_paint_rect(paint, (struct rect2i){
                .pos = { .x = content.x + advance / 2, .y = band },
                .size = { .x = advance / 2 > 0 ? advance / 2 : 1,
                          .y = height },
            }, COLOR_BREAKPOINT);
        }
        bool collapsed = false;
        if (fold_starts(state, line, &collapsed)) {
            const char *glyph = collapsed ? "\xE2\x96\xB8" : "\xE2\x96\xBE";
            paint_run(paint, (struct rect2i){
                .pos = { .x = content.x + GUTTER_MARK_CELLS * advance, .y = band },
                .size = { .x = advance, .y = style.cell_h },
            }, style, part_color(view, PART_GUTTER), glyph, 3);
        }
        char number[16];
        int length = snprintf(number, sizeof(number), "%u", line + 1);
        int cells = length > 0 ? length : 0;
        int digits = (state->gutter_width / advance) - GUTTER_MARK_CELLS -
                     GUTTER_FOLD_CELLS - GUTTER_PAD_CELLS;
        struct rect2i number_rect = {
            .pos = { .x = digits_x + (digits - cells) * advance, .y = band },
            .size = { .x = cells * advance, .y = style.cell_h },
        };
        uint32_t color = line == caret_line ? part_color(view, PART_TEXT)
                                            : part_color(view, PART_GUTTER);
        paint_run(paint, number_rect, style, color, number, (size_t)cells);
        y += height;
        line++;
    }
}

static void paint_caret(struct ui_editor_view *view,
                        struct ui_paint_context *paint, struct rect2i content,
                        struct ui_text_style style)
{
    struct ui_editor_state *state = view->state;
    if (!state->caret_valid || !ui_widget_has_focus(&view->widget))
        return;
    uint32_t line = line_of(state, state->caret_offset);
    ensure_prefix(&view->widget, state, line);
    int top = (int)state->prefix[line] - (int)state->prefix[state->first_line] +
              content.y;
    int cells = caret_cells(state, line, state->caret_offset);
    int x = content.x + state->gutter_width + cells * style.advance -
            state->scroll_x;
    if (top < content.y || top >= content.y + content.height)
        return;
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = x, .y = top },
        .size = { .x = 2, .y = style.cell_h },
    }, part_color(view, PART_CARET));
    if (state->preedit_len) {
        int cells_preedit = ui_text_run_cells(state->preedit, state->preedit_len);
        struct rect2i rect = {
            .pos = { .x = x, .y = top },
            .size = { .x = cells_preedit * style.advance, .y = style.cell_h },
        };
        ui_paint_rect(paint, rect, part_color(view, PART_PREEDIT));
        paint_run(paint, rect, style, part_color(view, PART_TEXT), state->preedit,
                  state->preedit_len);
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = rect.x, .y = rect.y + rect.height - 1 },
            .size = { .x = rect.width, .y = 1 },
        }, part_color(view, PART_TEXT));
    }
}

static void paint_sticky(struct ui_editor_view *view,
                         struct ui_paint_context *paint, struct rect2i content,
                         struct ui_text_style style)
{
    struct ui_editor_state *state = view->state;
    uint32_t line = 0;
    if (!ui_editor_view_sticky_line(view, &line))
        return;
    struct rect2i bar = {
        .pos = { .x = content.x, .y = content.y },
        .size = { .x = content.width, .y = style.cell_h },
    };
    ui_paint_rect(paint, bar, part_color(view, PART_STICKY));
    size_t length = 0;
    const char *bytes = line_bytes(state, line, &length);
    paint_run(paint, (struct rect2i){
        .pos = { .x = bar.x + state->gutter_width - state->scroll_x,
                 .y = bar.y },
        .size = { .x = bar.width, .y = bar.height },
    }, style, part_color(view, PART_TEXT), bytes, length);
}

static void paint_scrollbar(struct ui_editor_view *view,
                            struct ui_paint_context *paint,
                            struct rect2i content)
{
    struct ui_editor_state *state = view->state;
    int total = content_height(&view->widget, state);
    if (total <= state->content_height || content.height <= 0)
        return;
    int track_x = content.x + content.width - SCROLLBAR_WIDTH;
    struct rect2i track = {
        .pos = { .x = track_x, .y = content.y },
        .size = { .x = SCROLLBAR_WIDTH, .y = content.height },
    };
    ui_paint_rect(paint, track, part_color(view, PART_SCROLLBAR));
    int thumb = geom_sat_i((int64_t)content.height * state->content_height / total);
    if (thumb < SCROLLBAR_THUMB_MIN)
        thumb = SCROLLBAR_THUMB_MIN;
    if (thumb > content.height)
        thumb = content.height;
    int scroll_y = (int)state->prefix[state->first_line];
    int travel = content.height - thumb;
    int max = total - state->content_height;
    int offset = max > 0 ? geom_sat_i((int64_t)travel * scroll_y / max) : 0;
    ui_paint_rect(paint, (struct rect2i){
        .pos = { .x = track_x, .y = content.y + offset },
        .size = { .x = SCROLLBAR_WIDTH, .y = thumb },
    }, part_color(view, PART_TEXT));
    for (size_t i = 0; i < state->match_count; i++) {
        uint32_t line = state->matches[i];
        if (line >= line_count_of(state))
            continue;
        ensure_prefix(&view->widget, state, line);
        int y = geom_sat_i((int64_t)content.height * state->prefix[line] / total);
        ui_paint_rect(paint, (struct rect2i){
            .pos = { .x = track_x, .y = content.y + y },
            .size = { .x = SCROLLBAR_WIDTH, .y = 2 },
        }, part_color(view, PART_TICK));
    }
}

static void editor_paint(const struct ui_widget *widget,
                         struct ui_paint_context *paint)
{
    struct ui_editor_view *view = view_of(widget);
    struct ui_editor_state *state = view->state;
    struct rect2i bounds = ui_paint_bounds(paint);
    struct ui_paint_box_style box = ui_paint_box_style_of(&widget->style,
                                                          &widget->style_scale);
    if (!ui_paint_box(paint, bounds, &box, NULL))
        return;
    if (!state || !state->document)
        return;
    editor_parts_init();
    struct rect2i content = content_rect(view, bounds.size);
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    if (content.width <= 0 || content.height <= 0)
        return;
    layout_state(&view->widget, state, content);
    ensure_prefix(&view->widget, state, (uint32_t)state->first_line);
    if (!ui_paint_push_clip(paint, content))
        return;
    paint_selection(view, paint, content, style);
    paint_gutter(view, paint, content, style);
    paint_rows(view, paint, content, style);
    paint_sticky(view, paint, content, style);
    paint_caret(view, paint, content, style);
    paint_scrollbar(view, paint, content);
    ui_paint_pop_clip(paint);
}

static void editor_parts_debug(const struct ui_widget *widget,
                               struct ui_debug_parts *parts)
{
    struct ui_editor_view *view = view_of(widget);
    struct ui_editor_state *state = view->state;
    if (!state || !state->document)
        return;
    struct rect2i content = content_rect(view, widget->bounds.size);
    ui_debug_part(parts, "content", -1, ui_widget_rect_to_surface(widget, content));
    if (state->gutter_width)
        ui_debug_part(parts, "gutter", -1, ui_widget_rect_to_surface(widget,
            (struct rect2i){ .pos = content.pos,
                             .size = { .x = state->gutter_width,
                                       .y = content.height } }));
    uint32_t count = line_count_of(state);
    uint32_t line = (uint32_t)state->first_line;
    ensure_prefix(&view->widget, state, line);
    int y = content.y;
    while (line < count && y < content.y + content.height) {
        int rows = 1;
        int height = row_height(&view->widget, state, line, &rows);
        if (height == 0) {
            line++;
            continue;
        }
        ui_debug_part(parts, "text-row", (int)line, ui_widget_rect_to_surface(widget,
            (struct rect2i){ .pos = { .x = content.x + state->gutter_width,
                                      .y = y },
                             .size = { .x = content.width - state->gutter_width,
                                       .y = height } }));
        if (mark_of(state, line) != UI_EDITOR_MARK_NONE)
            ui_debug_part(parts, "gutter-mark", (int)line,
                          ui_widget_rect_to_surface(widget, (struct rect2i){
                              .pos = { .x = content.x, .y = y },
                              .size = { .x = ui_widget_cell_w(widget),
                                        .y = height } }));
        bool collapsed = false;
        if (fold_starts(state, line, &collapsed))
            ui_debug_part(parts, "gutter-fold", (int)line,
                          ui_widget_rect_to_surface(widget, (struct rect2i){
                              .pos = { .x = content.x + ui_widget_cell_w(widget),
                                       .y = y },
                              .size = { .x = ui_widget_cell_w(widget),
                                        .y = height } }));
        y += height;
        line++;
    }
}

static void editor_style_changed(struct ui_widget *widget)
{
    struct ui_editor_view *view = view_of(widget);
    if (!view->state)
        return;
    invalidate_heights(view->state, 0);
}

static struct vector2i editor_measure(const struct ui_widget *widget,
                                      struct ui_constraints constraints)
{
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    struct vector2i pad = insets2i_extent(ui_ctx_insets(
        &widget->style_scale, widget->style.padding, 0));
    int border = ui_widget_border_width(widget);
    int width = constraints.max.x;
    int height = constraints.max.y;
    if (height == INT_MAX || height > 40 * style.cell_h)
        height = 40 * style.cell_h;
    if (width == INT_MAX)
        width = 80 * style.advance;
    return ui_constraints_clamp(constraints, (struct vector2i){
        .x = geom_sat_i((int64_t)width),
        .y = geom_sat_i((int64_t)height + pad.y + 2 * border),
    });
}

static void editor_layout(struct ui_widget *widget, struct rect2i bounds)
{
    (void)bounds;
    struct ui_editor_view *view = view_of(widget);
    if (!view->state)
        return;
    struct rect2i content = content_rect(view, widget->bounds.size);
    layout_state(widget, view->state, content);
}

/* --- events ------------------------------------------------------------ */

static uint64_t offset_at_point(struct ui_widget *widget,
                                struct ui_editor_state *state,
                                struct vector2i point)
{
    struct ui_editor_view *view = view_of(widget);
    struct rect2i content = content_rect(view, widget->bounds.size);
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    ensure_prefix(widget, state, (uint32_t)state->first_line);
    int y = point.y - content.y + (int)state->prefix[state->first_line];
    uint32_t line = line_at_y(widget, state, y < 0 ? 0 : y);
    int x = point.x - content.x - state->gutter_width + state->scroll_x;
    return offset_at_cell(state, line, x / (style.advance > 0 ? style.advance : 1),
                          style.advance);
}

static bool gutter_event(struct ui_editor_view *view,
                         struct vector2i point)
{
    struct ui_editor_state *state = view->state;
    struct rect2i content = content_rect(view, view->widget.bounds.size);
    ensure_prefix(&view->widget, state, (uint32_t)state->first_line);
    int y = point.y - content.y + (int)state->prefix[state->first_line];
    uint32_t line = line_at_y(&view->widget, state, y < 0 ? 0 : y);
    int cell = (point.x - content.x) / ui_widget_cell_w(&view->widget);
    if (cell < GUTTER_MARK_CELLS) {
        toggle_breakpoint(view, line);
        return true;
    }
    if (cell < GUTTER_MARK_CELLS + GUTTER_FOLD_CELLS) {
        toggle_line_fold(view, line);
        return true;
    }
    struct editor_position position = { .line = line, .character = 0 };
    uint64_t offset = 0;
    editor_text_offset_at(editor_document_text(state->document), position,
                          EDITOR_COLUMN_UTF32, &offset);
    set_caret_offsets(view, offset, offset);
    return true;
}

static bool editor_pointer(struct ui_editor_view *view,
                           const struct ui_event *event)
{
    struct ui_editor_state *state = view->state;
    const struct ui_pointer_event *p = &event->data.pointer;
    if (p->kind == UI_POINTER_PRESS && p->button == 0) {
        struct vector2i local = ui_widget_to_local(&view->widget, p->pos);
        ui_widget_focus(&view->widget);
        if (state->gutter_width &&
            local.x < content_rect(view, view->widget.bounds.size).x +
                          state->gutter_width) {
            return gutter_event(view, local);
        }
        ui_widget_capture_pointer(&view->widget);
        uint64_t offset = offset_at_point(&view->widget, state, local);
        bool extend = (p->mods & UI_MOD_SHIFT) != 0;
        move_caret(view, offset, extend);
        return true;
    }
    if (p->kind == UI_POINTER_MOTION && ui_widget_is_captured(&view->widget)) {
        struct vector2i local = ui_widget_to_local(&view->widget, p->pos);
        move_caret(view, offset_at_point(&view->widget, state, local), true);
        return true;
    }
    if (p->kind == UI_POINTER_RELEASE && p->button == 0) {
        ui_widget_release_pointer(&view->widget);
        return true;
    }
    if (p->kind == UI_POINTER_SCROLL) {
        if (ui_pointer_is_horizontal_wheel(p))
            scroll_x_by(view, p->scroll_lines * ui_widget_cell_w(&view->widget));
        else
            scroll_pixels(view, -p->scroll_lines *
                                    ui_widget_cell_h(&view->widget));
        return true;
    }
    return false;
}

static bool editor_key(struct ui_editor_view *view, const struct ui_key_event *key)
{
    bool shift = (key->mods & UI_MOD_SHIFT) != 0;
    switch (key->edit_command) {
    case UI_EDIT_SELECT_ALL:
        set_caret_offsets(view, 0,
            editor_text_length(editor_document_text(view->state->document)));
        return true;
    case UI_EDIT_COPY:
        return copy_selection(view, false);
    case UI_EDIT_CUT:
        return copy_selection(view, true);
    case UI_EDIT_UNDO:
        if (editor_document_undo(view->state->document)) {
            changed(view, 0, line_count_of(view->state) ? line_count_of(view->state) - 1 : 0);
            return true;
        }
        return false;
    case UI_EDIT_REDO:
        if (editor_document_redo(view->state->document)) {
            changed(view, 0, line_count_of(view->state) ? line_count_of(view->state) - 1 : 0);
            return true;
        }
        return false;
    default:
        break;
    }
    switch (key->key) {
    case UI_KEY_LEFT: {
        bool word = key->edit_unit != UI_EDIT_CHARACTER;
        (void)word;
        move_horizontal(view, true, shift);
        return true;
    }
    case UI_KEY_RIGHT:
        move_horizontal(view, false, shift);
        return true;
    case UI_KEY_UP:
        move_vertical(view, -1, shift);
        return true;
    case UI_KEY_DOWN:
        move_vertical(view, 1, shift);
        return true;
    case UI_KEY_HOME:
        move_home(view, shift);
        return true;
    case UI_KEY_END:
        move_end(view, shift);
        return true;
    case UI_KEY_PAGE_UP:
        move_page(view, true, shift);
        return true;
    case UI_KEY_PAGE_DOWN:
        move_page(view, false, shift);
        return true;
    case UI_KEY_BACKSPACE:
        delete_backward(view);
        return true;
    case UI_KEY_DELETE:
        delete_forward(view);
        return true;
    case UI_KEY_ENTER:
        insert_newline(view);
        return true;
    default:
        break;
    }
    return false;
}

static bool editor_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_editor_view *view = view_of(widget);
    struct ui_editor_state *state = view->state;
    if (!state || !state->document)
        return false;
    switch (event->kind) {
    case UI_EVENT_KEY:
        return event->data.key.action == UI_KEY_PRESS &&
               editor_key(view, &event->data.key);
    case UI_EVENT_TEXT:
        if (!event->data.text.len)
            return false;
        insert_text(view, event->data.text.data, event->data.text.len);
        return true;
    case UI_EVENT_PASTE:
        if (!event->data.text.len)
            return false;
        insert_text(view, event->data.text.data, event->data.text.len);
        return true;
    case UI_EVENT_POINTER:
        return editor_pointer(view, event);
    case UI_EVENT_SHORTCUT:
        return event->data.shortcut.edit != UI_EDIT_NONE &&
               editor_key(view, &(struct ui_key_event){
                   .edit_command = event->data.shortcut.edit });
    default:
        return false;
    }
}

static enum ui_cursor editor_cursor(const struct ui_widget *widget,
                                    struct vector2i pos)
{
    (void)widget;
    (void)pos;
    return UI_CURSOR_TEXT;
}

static const struct ui_widget_ops EDITOR_OPS = {
    .name = "editor-view",
    .width_sensitive = true,
    .measure = editor_measure,
    .layout = editor_layout,
    .event = editor_event,
    .paint = editor_paint,
    .style_changed = editor_style_changed,
    .cursor = editor_cursor,
    .debug_parts = editor_parts_debug,
};

static struct ui_editor_state *view_state(const struct ui_editor_view *view)
{
    return view ? view->state : NULL;
}

static bool ensure_state(struct ui_editor_view *view)
{
    if (!view || view->widget.ops)
        return view && view->state;
    editor_parts_init();
    struct ui_editor_state *state = calloc(1, sizeof(*state));
    if (!state)
        return false;
    state->document = editor_document_create();
    if (!state->document) {
        free(state);
        return false;
    }
    state->gutter = true;
    view->state = state;
    ui_widget_init(&view->widget, &EDITOR_OPS, NULL);
    ui_widget_set_style_identity(&view->widget, "editor-view", NULL, "editor-view");
    ui_widget_set_focusable(&view->widget, true);
    return true;
}

void ui_editor_view_init(struct ui_editor_view *view)
{
    if (!view)
        return;
    *view = (struct ui_editor_view){0};
    ensure_state(view);
}

void ui_editor_view_clear(struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    if (!state)
        return;
    for (uint32_t i = 0; i < state->row_capacity; i++)
        free(state->rows[i].spans);
    free(state->rows);
    free(state->prefix);
    free(state->folds);
    free(state->marks);
    for (uint32_t i = 0; i < state->inlay_count; i++)
        free(state->inlays[i].text);
    free(state->inlays);
    free(state->matches);
    free(state->preedit);
    free(state->scratch);
    editor_document_destroy(state->document);
    free(state);
    view->state = NULL;
    if (view->widget.ops)
        ui_widget_destroy(&view->widget);
}

struct ui_widget *ui_editor_view_widget(struct ui_editor_view *view)
{
    if (!view)
        return NULL;
    if (!ensure_state(view))
        return NULL;
    return &view->widget;
}

bool ui_editor_view_open(struct ui_editor_view *view, const char *path)
{
    if (!ensure_state(view))
        return false;
    if (!editor_document_open(view->state->document, path))
        return false;
    view->state->text_generation++;
    invalidate_rows(view->state);
    invalidate_heights(view->state, 0);
    set_caret_offsets(view, 0, 0);
    ui_widget_invalidate(&view->widget, UI_WIDGET_INVALIDATE_MEASURE);
    return true;
}

bool ui_editor_view_set_text(struct ui_editor_view *view, const char *bytes,
                             size_t length)
{
    if (!ensure_state(view))
        return false;
    struct editor_document *document = view->state->document;
    uint64_t current = editor_text_length(editor_document_text(document));
    if (!editor_document_replace(document, 0, current, bytes ? bytes : "",
                                 bytes ? length : 0))
        return false;
    view->state->text_generation++;
    invalidate_rows(view->state);
    invalidate_heights(view->state, 0);
    set_caret_offsets(view, 0, 0);
    ui_widget_invalidate(&view->widget, UI_WIDGET_INVALIDATE_MEASURE);
    return true;
}

struct editor_document *ui_editor_view_document(struct ui_editor_view *view)
{
    return view_state(view) ? view->state->document : NULL;
}

void ui_editor_view_set_wrap(struct ui_editor_view *view, bool wrap)
{
    if (!ensure_state(view) || view->state->wrap == wrap)
        return;
    view->state->wrap = wrap;
    invalidate_heights(view->state, 0);
    ui_widget_invalidate(&view->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

bool ui_editor_view_wrap(const struct ui_editor_view *view)
{
    return view_state(view) ? view->state->wrap : false;
}

void ui_editor_view_set_gutter(struct ui_editor_view *view, bool shown)
{
    if (!ensure_state(view))
        return;
    view->state->gutter = shown;
    ui_widget_invalidate(&view->widget, UI_WIDGET_INVALIDATE_LAYOUT);
}

bool ui_editor_view_gutter(const struct ui_editor_view *view)
{
    return view_state(view) ? view->state->gutter : false;
}

void ui_editor_view_set_tokenizer(struct ui_editor_view *view,
                                  ui_editor_tokenize_fn fn, void *userdata)
{
    if (!ensure_state(view))
        return;
    view->state->tokenizer = fn;
    view->state->tokenizer_data = fn ? userdata : NULL;
    view->state->text_generation++;
    invalidate_rows(view->state);
    invalidate_paint(view);
}

bool ui_editor_view_add_fold(struct ui_editor_view *view, uint32_t start_line,
                             uint32_t end_line, const char *label)
{
    (void)label;
    if (!ensure_state(view) || start_line >= end_line)
        return false;
    struct ui_editor_state *state = view->state;
    if (!grow((void **)&state->folds, &state->fold_capacity, state->fold_count + 1,
              sizeof(*state->folds)))
        return false;
    state->folds[state->fold_count++] = (struct ui_editor_fold){
        .start = start_line, .end = end_line, .collapsed = true };
    invalidate_heights(state, start_line);
    invalidate_paint(view);
    return true;
}

bool ui_editor_view_set_folds(struct ui_editor_view *view,
                              const struct editor_folds *folds)
{
    if (!ensure_state(view))
        return false;
    struct ui_editor_state *state = view->state;
    state->fold_count = 0;
    struct editor_folds *normalized = editor_folds_create();
    if (!normalized)
        return false;
    for (size_t i = 0; i < editor_folds_count(folds); i++) {
        struct editor_fold_range range;
        if (editor_folds_get(folds, i, &range))
            editor_folds_add(normalized, range.start_line, range.end_line,
                             range.label);
    }
    editor_folds_normalize(normalized);
    for (size_t i = 0; i < editor_folds_count(normalized); i++) {
        struct editor_fold_range range;
        if (!editor_folds_get(normalized, i, &range))
            continue;
        if (!ui_editor_view_add_fold(view, range.start_line, range.end_line,
                                     range.label)) {
            editor_folds_destroy(normalized);
            return false;
        }
    }
    editor_folds_destroy(normalized);
    invalidate_heights(state, 0);
    return true;
}

bool ui_editor_view_toggle_fold(struct ui_editor_view *view, uint32_t line)
{
    struct ui_editor_state *state = view_state(view);
    if (!state)
        return false;
    for (uint32_t i = 0; i < state->fold_count; i++) {
        if (state->folds[i].start == line) {
            toggle_line_fold(view, line);
            return true;
        }
    }
    return false;
}

bool ui_editor_view_line_hidden(const struct ui_editor_view *view, uint32_t line)
{
    struct ui_editor_state *state = view_state(view);
    return state ? folded(state, line) : false;
}

uint32_t ui_editor_view_visible_line_count(const struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document)
        return 0;
    uint32_t count = line_count_of(state);
    uint32_t visible = 0;
    for (uint32_t line = 0; line < count; line++)
        if (!folded(state, line))
            visible++;
    return visible;
}

void ui_editor_view_set_diagnostics(struct ui_editor_view *view,
                                    const struct editor_diagnostics *diagnostics)
{
    if (!ensure_state(view))
        return;
    view->state->diagnostics = diagnostics;
    invalidate_paint(view);
}

void ui_editor_view_set_mark(struct ui_editor_view *view, uint32_t line,
                             enum ui_editor_mark mark)
{
    if (!ensure_state(view))
        return;
    struct ui_editor_state *state = view->state;
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < state->mark_count; i++)
        if (state->marks[i].line == line)
            found = i;
    if (mark == UI_EDITOR_MARK_NONE) {
        if (found != UINT32_MAX) {
            state->marks[found] = state->marks[--state->mark_count];
            invalidate_paint(view);
        }
        return;
    }
    if (found == UINT32_MAX) {
        if (!grow((void **)&state->marks, &state->mark_capacity,
                  state->mark_count + 1, sizeof(*state->marks)))
            return;
        found = state->mark_count++;
        state->marks[found].line = line;
    }
    state->marks[found].mark = mark;
    invalidate_paint(view);
}

enum ui_editor_mark ui_editor_view_mark(const struct ui_editor_view *view,
                                        uint32_t line)
{
    struct ui_editor_state *state = view_state(view);
    return state ? mark_of(state, line) : UI_EDITOR_MARK_NONE;
}

bool ui_editor_view_add_inlay(struct ui_editor_view *view, uint32_t line,
                              const char *text, enum ui_editor_inlay_place place)
{
    if (!ensure_state(view) || !text)
        return false;
    struct ui_editor_state *state = view->state;
    if (!grow((void **)&state->inlays, &state->inlay_capacity,
              state->inlay_count + 1, sizeof(*state->inlays)))
        return false;
    char *copy = strdup(text);
    if (!copy)
        return false;
    state->inlays[state->inlay_count++] = (struct ui_editor_inlay){
        .line = line, .text = copy, .place = place };
    invalidate_heights(state, line);
    invalidate_paint(view);
    return true;
}

void ui_editor_view_clear_inlays(struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    if (!state)
        return;
    for (uint32_t i = 0; i < state->inlay_count; i++)
        free(state->inlays[i].text);
    state->inlay_count = 0;
    invalidate_heights(state, 0);
    invalidate_paint(view);
}

bool ui_editor_view_set_matches(struct ui_editor_view *view,
                                const uint32_t *lines, size_t count)
{
    if (!ensure_state(view))
        return false;
    struct ui_editor_state *state = view->state;
    free(state->matches);
    state->matches = NULL;
    state->match_count = 0;
    if (!count)
        return true;
    uint32_t *copy = malloc(count * sizeof(*copy));
    if (!copy)
        return false;
    memcpy(copy, lines, count * sizeof(*copy));
    state->matches = copy;
    state->match_count = count;
    invalidate_paint(view);
    return true;
}

bool ui_editor_view_set_caret(struct ui_editor_view *view,
                              struct editor_position caret)
{
    if (!ensure_state(view))
        return false;
    uint64_t offset = 0;
    if (!editor_text_offset_at(editor_document_text(view->state->document), caret,
                               EDITOR_COLUMN_UTF32, &offset))
        return false;
    set_caret_offsets(view, offset, offset);
    reveal_caret(view);
    return true;
}

struct editor_position ui_editor_view_caret(const struct ui_editor_view *view)
{
    struct editor_position position = {0};
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document)
        return position;
    editor_text_position(editor_document_text(state->document),
                         state->caret_offset, EDITOR_COLUMN_UTF32, &position);
    return position;
}

uint64_t ui_editor_view_caret_offset(const struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    return state ? state->caret_offset : 0;
}

struct editor_position ui_editor_view_position_at(const struct ui_editor_view *view,
                                                  struct vector2i point)
{
    struct editor_position position = {0};
    if (!view_state(view))
        return position;
    uint64_t offset = offset_at_point((struct ui_widget *)&view->widget,
                                      view->state, point);
    editor_text_position(editor_document_text(view->state->document), offset,
                         EDITOR_COLUMN_UTF32, &position);
    return position;
}

bool ui_editor_view_select(struct ui_editor_view *view, uint64_t offset)
{
    if (!ensure_state(view))
        return false;
    uint64_t caret = view->state->caret_offset;
    set_caret_offsets(view, offset, caret);
    return true;
}

bool ui_editor_view_selection(const struct ui_editor_view *view,
                              uint64_t *start, uint64_t *end)
{
    struct ui_editor_state *state = view_state(view);
    if (!state)
        return false;
    uint64_t lo, hi;
    if (!selection_range(state, &lo, &hi))
        return false;
    if (start)
        *start = lo;
    if (end)
        *end = hi;
    return true;
}

size_t ui_editor_view_selection_rects(const struct ui_editor_view *view,
                                      struct rect2i *out, size_t cap)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document)
        return 0;
    uint64_t start, end;
    if (!selection_range(state, &start, &end))
        return 0;
    struct rect2i content = content_rect(view, view->widget.bounds.size);
    struct ui_text_style style = ui_widget_text_style(&view->widget.style_scale);
    uint32_t count = line_count_of(state);
    size_t written = 0;
    for (uint32_t line = 0; line < count; line++) {
        struct editor_line info = line_info(state, line);
        if (info.start + info.length < start && info.end <= start)
            continue;
        if (info.start > end)
            break;
        uint64_t lo = start > info.start ? start : info.start;
        uint64_t hi = end < info.start + info.length ? end : info.start + info.length;
        if (lo > hi)
            continue;
        int top = 0;
        if (!ui_editor_view_line_y(view, line, &top))
            continue;
        int before = caret_cells(state, line, lo);
        int cells = caret_cells(state, line, hi) - before;
        if (out && written < cap)
            out[written] = (struct rect2i){
                .pos = { .x = content.x + state->gutter_width +
                                before * style.advance - state->scroll_x,
                         .y = content.y + top },
                .size = { .x = cells * style.advance, .y = style.cell_h },
            };
        written++;
    }
    return written;
}

void ui_editor_view_set_preedit(struct ui_editor_view *view, const char *text,
                                size_t length)
{
    if (!ensure_state(view))
        return;
    struct ui_editor_state *state = view->state;
    free(state->preedit);
    state->preedit = NULL;
    state->preedit_len = 0;
    if (text && length) {
        char *copy = malloc(length + 1);
        if (!copy)
            return;
        memcpy(copy, text, length);
        copy[length] = '\0';
        state->preedit = copy;
        state->preedit_len = length;
    }
    invalidate_paint(view);
}

const char *ui_editor_view_preedit(const struct ui_editor_view *view,
                                   size_t *length)
{
    struct ui_editor_state *state = view_state(view);
    if (length)
        *length = state ? state->preedit_len : 0;
    return state && state->preedit ? state->preedit : "";
}

void ui_editor_view_scroll_lines(struct ui_editor_view *view, int lines)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document)
        return;
    uint32_t count = line_count_of(state);
    int target = state->first_line + lines;
    if (target < 0)
        target = 0;
    if (count && target >= (int)count)
        target = (int)count - 1;
    state->first_line = target;
    ensure_prefix(&view->widget, state, (uint32_t)target);
    invalidate_paint(view);
}

void ui_editor_view_scroll_to_line(struct ui_editor_view *view, uint32_t line)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document)
        return;
    uint32_t count = line_count_of(state);
    if (count && line >= count)
        line = count - 1;
    state->first_line = (int)line;
    ensure_prefix(&view->widget, state, line);
    invalidate_paint(view);
}

void ui_editor_view_scroll_x(struct ui_editor_view *view, int px)
{
    struct ui_editor_state *state = view_state(view);
    if (!state)
        return;
    state->scroll_x = px > 0 ? px : 0;
    invalidate_paint(view);
}

uint32_t ui_editor_view_first_line(const struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    return state ? (uint32_t)state->first_line : 0;
}

int ui_editor_view_scroll_x_pixels(const struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    return state ? state->scroll_x : 0;
}

bool ui_editor_view_sticky_line(const struct ui_editor_view *view,
                                uint32_t *line)
{
    struct ui_editor_state *state = view_state(view);
    if (!state)
        return false;
    uint32_t top = (uint32_t)state->first_line;
    uint32_t best = UINT32_MAX;
    /* The innermost range that holds the first visible line. */
    for (uint32_t i = 0; i < state->fold_count; i++) {
        const struct ui_editor_fold *fold = &state->folds[i];
        if (fold->start >= top || top > fold->end)
            continue;
        if (best == UINT32_MAX || fold->start > best)
            best = fold->start;
    }
    if (best == UINT32_MAX)
        return false;
    if (line)
        *line = best;
    return true;
}

bool ui_editor_view_line_y(const struct ui_editor_view *view, uint32_t line,
                           int *y)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document || line >= line_count_of(state))
        return false;
    ensure_prefix((struct ui_widget *)&view->widget, state, line);
    int value = (int)state->prefix[line] - (int)state->prefix[state->first_line];
    *y = value;
    return true;
}

int ui_editor_view_line_height(const struct ui_editor_view *view, uint32_t line)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document || line >= line_count_of(state))
        return 0;
    int rows = 1;
    return row_height((struct ui_widget *)&view->widget, state, line, &rows);
}

int ui_editor_view_line_rows(const struct ui_editor_view *view, uint32_t line)
{
    struct ui_editor_state *state = view_state(view);
    if (!state || !state->document || line >= line_count_of(state))
        return 0;
    int rows = 1;
    row_height((struct ui_widget *)&view->widget, state, line, &rows);
    return rows;
}

struct ui_editor_paint_stats ui_editor_view_paint_stats(
    const struct ui_editor_view *view)
{
    struct ui_editor_state *state = view_state(view);
    return state ? state->stats : (struct ui_editor_paint_stats){0};
}
