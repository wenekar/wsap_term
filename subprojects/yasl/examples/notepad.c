/**
 * @file notepad.c
 * A desktop text editor built on YASL's public CPU surface API.
 *
 * Features:
 * - New (Ctrl+N), Open (Ctrl+O, CLI arg, drag-and-drop), Save (Ctrl+S), Save As (Ctrl+Shift+S)
 * - Tab key with 4-space tab stops; Shift+Tab unindent
 * - Clipboard Copy (Ctrl+C), Cut (Ctrl+X), Paste (Ctrl+V) via YASL native clipboard
 * - Undo (Ctrl+Z) & Redo (Ctrl+Y / Ctrl+Shift+Z)
 * - Find (Ctrl+F), Find Next (F3), Find Prev (Shift+F3), Replace (Ctrl+H), Goto Line (Ctrl+G)
 * - Insert Date/Time (F5)
 * - Word navigation (Ctrl+Left/Right) & Word deletion (Ctrl+Backspace/Delete)
 * - Word Wrap toggle (Ctrl+W) & Horizontal Scrolling
 * - Zoom In / Out / Reset (Ctrl+=, Ctrl+-, Ctrl+0)
 * - Mouse selection with click, drag, double-click (word), triple-click (line)
 * - Unsaved changes confirmation on exit
 * - Real-time asynchronous background stats via a worker and yasl_wake()
 */

#define _DEFAULT_SOURCE
#include <yasl/yasl.h>

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINES 4096
#define MAX_LINE_LEN 1024
#define MAX_UNDO 32

/* Classic 8x8 font bitmap covering ASCII 32 (' ') to 126 ('~') */
static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* $ */
    {0x00,0x63,0x66,0x0C,0x18,0x33,0x63,0x00}, /* % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
    {0x0C,0x1C,0x34,0x64,0x7E,0x04,0x04,0x00}, /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* > */
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, /* ? */
    {0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00}, /* @ */
    {0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00}, /* A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* B */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* C */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* D */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* E */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* F */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, /* G */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
    {0x0E,0x06,0x06,0x06,0x66,0x66,0x3C,0x00}, /* J */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* K */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* L */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* M */
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, /* N */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* O */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* P */
    {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x0E,0x00}, /* Q */
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, /* R */
    {0x3C,0x66,0x30,0x18,0x0C,0x66,0x3C,0x00}, /* S */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* T */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* U */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* Y */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
    {0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x00}, /* \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00}, /* _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00}, /* a */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* b */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* c */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, /* d */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* e */
    {0x1C,0x30,0x30,0x78,0x30,0x30,0x30,0x00}, /* f */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x3C}, /* g */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C}, /* j */
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* o */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* p */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, /* q */
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, /* r */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* s */
    {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00}, /* t */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3B,0x00}, /* u */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00}, /* w */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, /* x */
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, /* y */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, /* z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  /* ~ */
};

typedef enum prompt_mode {
    PROMPT_NONE = 0,
    PROMPT_OPEN,
    PROMPT_SAVE,
    PROMPT_FIND,
    PROMPT_REPLACE_FIND,
    PROMPT_REPLACE_WITH,
    PROMPT_GOTO,
    PROMPT_CONFIRM_QUIT,
} prompt_mode;

struct doc_stats {
    int words;
    int chars;
};

struct undo_snapshot {
    char lines[MAX_LINES][MAX_LINE_LEN];
    int line_count;
    int cursor_row;
    int cursor_col;
};

struct editor {
    yasl_surface *surface;
    double surface_scale;
    bool dirty, running, failed;
    uint64_t text_revision;
    char ime_snapshot[MAX_LINE_LEN];
    size_t ime_begin, ime_end;
    bool ime_snapshot_ready;
    double ime_caret_x, ime_caret_y;
    bool ime_caret_ready;

    /* File tracking */
    char file_path[512];
    bool has_file;
    bool is_modified;

    /* Text buffer */
    char lines[MAX_LINES][MAX_LINE_LEN];
    int line_count;

    /* Cursor & Selection */
    int cursor_row;
    int cursor_col;
    int sel_anchor_row;
    int sel_anchor_col;
    bool has_selection;
    bool mouse_dragging;

    /* Double/Triple click tracking */
    uint64_t last_click_time;
    double last_click_x, last_click_y;
    int click_count;

    /* Viewport & Layout */
    int scroll_y;
    int scroll_x;
    bool word_wrap;
    int zoom_offset; /* -1, 0, 1, 2 */
    bool caret_visible;

    /* Interactive mini-buffer prompt */
    prompt_mode prompt;
    char prompt_buf[256];
    int prompt_len;
    char search_query[256];
    char replace_query[256];

    /* Notification message in status bar */
    char status_msg[128];
    uint64_t status_msg_time;

    /* Undo & Redo stacks */
    struct undo_snapshot undo_stack[MAX_UNDO];
    int undo_count;
    struct undo_snapshot redo_stack[MAX_UNDO];
    int redo_count;

    /* Background stats from async worker */
    struct doc_stats stats;
    _Atomic bool worker_running;
    bool worker_started, worker_mutex_ready, worker_cond_ready;
    bool worker_dirty;
    char (*worker_lines)[MAX_LINE_LEN];
    int worker_line_count;
    uint64_t stats_generation;
    pthread_t worker_tid;
    pthread_mutex_t worker_mutex;
    pthread_cond_t worker_cond;
};

static void editor_set_status(struct editor *ed, const char *msg)
{
    snprintf(ed->status_msg, sizeof(ed->status_msg), "%s", msg);
    ed->status_msg_time = yasl_time_ns();
    ed->dirty = true;
}

static void editor_request_render(struct editor *ed)
{
    if (ed) ed->dirty = true;
}

static void editor_update_title(struct editor *ed)
{
    if (!ed->surface) return;
    const char *name = ed->has_file ? ed->file_path : "Untitled";
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;

    char title[540];
    snprintf(title, sizeof(title), "%s%s - YASL Notepad", ed->is_modified ? "*" : "", name);
    yasl_surface_set_title(ed->surface, title);
}

static void editor_save_undo(struct editor *ed)
{
    if (ed->undo_count >= MAX_UNDO) {
        memmove(&ed->undo_stack[0], &ed->undo_stack[1], sizeof(struct undo_snapshot) * (MAX_UNDO - 1));
        ed->undo_count = MAX_UNDO - 1;
    }
    struct undo_snapshot *s = &ed->undo_stack[ed->undo_count++];
    s->line_count = ed->line_count;
    s->cursor_row = ed->cursor_row;
    s->cursor_col = ed->cursor_col;
    for (int i = 0; i < ed->line_count; i++) {
        memcpy(s->lines[i], ed->lines[i], MAX_LINE_LEN);
    }
    ed->redo_count = 0; /* New edit clears redo stack */
}

static void editor_undo(struct editor *ed)
{
    if (ed->undo_count <= 0) {
        editor_set_status(ed, "Already at oldest change");
        return;
    }
    /* Push current state to redo */
    if (ed->redo_count < MAX_UNDO) {
        struct undo_snapshot *rs = &ed->redo_stack[ed->redo_count++];
        rs->line_count = ed->line_count;
        rs->cursor_row = ed->cursor_row;
        rs->cursor_col = ed->cursor_col;
        for (int i = 0; i < ed->line_count; i++) {
            memcpy(rs->lines[i], ed->lines[i], MAX_LINE_LEN);
        }
    }

    struct undo_snapshot *s = &ed->undo_stack[--ed->undo_count];
    ed->line_count = s->line_count;
    ed->cursor_row = s->cursor_row;
    ed->cursor_col = s->cursor_col;
    for (int i = 0; i < ed->line_count; i++) {
        memcpy(ed->lines[i], s->lines[i], MAX_LINE_LEN);
    }
    ed->has_selection = false;
    ed->is_modified = true;
    editor_update_title(ed);
    editor_set_status(ed, "Undo");
}

static void editor_redo(struct editor *ed)
{
    if (ed->redo_count <= 0) {
        editor_set_status(ed, "Already at newest change");
        return;
    }
    /* Push current state to undo */
    if (ed->undo_count < MAX_UNDO) {
        struct undo_snapshot *us = &ed->undo_stack[ed->undo_count++];
        us->line_count = ed->line_count;
        us->cursor_row = ed->cursor_row;
        us->cursor_col = ed->cursor_col;
        for (int i = 0; i < ed->line_count; i++) {
            memcpy(us->lines[i], ed->lines[i], MAX_LINE_LEN);
        }
    }

    struct undo_snapshot *s = &ed->redo_stack[--ed->redo_count];
    ed->line_count = s->line_count;
    ed->cursor_row = s->cursor_row;
    ed->cursor_col = s->cursor_col;
    for (int i = 0; i < ed->line_count; i++) {
        memcpy(ed->lines[i], s->lines[i], MAX_LINE_LEN);
    }
    ed->has_selection = false;
    ed->is_modified = true;
    editor_update_title(ed);
    editor_set_status(ed, "Redo");
}

static void editor_notify_worker(struct editor *ed)
{
    char (*snapshot)[MAX_LINE_LEN] = malloc(sizeof(ed->lines));
    if (!snapshot) return;
    memcpy(snapshot, ed->lines, sizeof(ed->lines));
    pthread_mutex_lock(&ed->worker_mutex);
    free(ed->worker_lines);
    ed->worker_lines = snapshot;
    ed->worker_line_count = ed->line_count;
    ed->worker_dirty = true;
    pthread_cond_signal(&ed->worker_cond);
    pthread_mutex_unlock(&ed->worker_mutex);
}

static void *worker_thread(void *arg)
{
    struct editor *ed = arg;
    while (ed->worker_running) {
        pthread_mutex_lock(&ed->worker_mutex);
        while (!ed->worker_dirty && ed->worker_running) {
            pthread_cond_wait(&ed->worker_cond, &ed->worker_mutex);
        }
        if (!ed->worker_running) {
            pthread_mutex_unlock(&ed->worker_mutex);
            break;
        }
        char (*lines)[MAX_LINE_LEN] = ed->worker_lines;
        int line_count = ed->worker_line_count;
        ed->worker_lines = NULL;
        ed->worker_dirty = false;
        pthread_mutex_unlock(&ed->worker_mutex);
        if (!lines) continue;

        int word_count = 0;
        int char_count = 0;
        for (int i = 0; i < line_count; i++) {
            const char *p = lines[i];
            bool in_word = false;
            while (*p) {
                char_count++;
                if (isspace((unsigned char)*p)) {
                    in_word = false;
                } else if (!in_word) {
                    in_word = true;
                    word_count++;
                }
                p++;
            }
        }

        free(lines);
        if (!ed->worker_running)
            break;
        pthread_mutex_lock(&ed->worker_mutex);
        ed->stats.words = word_count;
        ed->stats.chars = char_count;
        ed->stats_generation++;
        pthread_mutex_unlock(&ed->worker_mutex);
        yasl_wake();
    }
    return NULL;
}

/* File I/O */
static bool editor_open_file(struct editor *ed, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        char err[128];
        snprintf(err, sizeof(err), "Could not open: %s", path);
        editor_set_status(ed, err);
        return false;
    }

    ed->line_count = 0;
    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof(buf), f) && ed->line_count < MAX_LINES) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        snprintf(ed->lines[ed->line_count++], MAX_LINE_LEN, "%s", buf);
    }
    fclose(f);

    if (ed->line_count == 0) {
        ed->lines[0][0] = '\0';
        ed->line_count = 1;
    }

    snprintf(ed->file_path, sizeof(ed->file_path), "%s", path);
    ed->has_file = true;
    ed->is_modified = false;
    ed->cursor_row = 0;
    ed->cursor_col = 0;
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->has_selection = false;
    ed->undo_count = 0;
    ed->redo_count = 0;

    editor_update_title(ed);
    char msg[128];
    snprintf(msg, sizeof(msg), "Opened %s (%d lines)", path, ed->line_count);
    editor_set_status(ed, msg);
    editor_notify_worker(ed);
    return true;
}

static bool editor_save_file(struct editor *ed, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        char err[128];
        snprintf(err, sizeof(err), "Could not save: %s", path);
        editor_set_status(ed, err);
        return false;
    }

    for (int i = 0; i < ed->line_count; i++) {
        fprintf(f, "%s\n", ed->lines[i]);
    }
    fclose(f);

    snprintf(ed->file_path, sizeof(ed->file_path), "%s", path);
    ed->has_file = true;
    ed->is_modified = false;
    editor_update_title(ed);

    char msg[128];
    snprintf(msg, sizeof(msg), "Saved to %s", path);
    editor_set_status(ed, msg);
    return true;
}

static void editor_new_file(struct editor *ed)
{
    ed->lines[0][0] = '\0';
    ed->line_count = 1;
    ed->has_file = false;
    ed->file_path[0] = '\0';
    ed->is_modified = false;
    ed->cursor_row = 0;
    ed->cursor_col = 0;
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->has_selection = false;
    ed->undo_count = 0;
    ed->redo_count = 0;
    editor_update_title(ed);
    editor_set_status(ed, "New file created");
    editor_notify_worker(ed);
}

/* Selection helper */
static void get_selection_bounds(const struct editor *ed, int *r0, int *c0, int *r1, int *c1)
{
    if (!ed->has_selection) {
        *r0 = *r1 = ed->cursor_row;
        *c0 = *c1 = ed->cursor_col;
        return;
    }
    if (ed->sel_anchor_row < ed->cursor_row ||
        (ed->sel_anchor_row == ed->cursor_row && ed->sel_anchor_col <= ed->cursor_col)) {
        *r0 = ed->sel_anchor_row; *c0 = ed->sel_anchor_col;
        *r1 = ed->cursor_row;     *c1 = ed->cursor_col;
    }
    else {
        *r0 = ed->cursor_row;     *c0 = ed->cursor_col;
        *r1 = ed->sel_anchor_row; *c1 = ed->sel_anchor_col;
    }
}

static bool is_char_selected(const struct editor *ed, int r, int c)
{
    if (!ed->has_selection) return false;
    int r0, c0, r1, c1;
    get_selection_bounds(ed, &r0, &c0, &r1, &c1);
    if (r < r0 || r > r1) return false;
    if (r == r0 && r == r1) return c >= c0 && c < c1;
    if (r == r0) return c >= c0;
    if (r == r1) return c < c1;
    return true;
}

static void editor_delete_selection(struct editor *ed)
{
    if (!ed->has_selection) return;
    editor_save_undo(ed);

    int r0, c0, r1, c1;
    get_selection_bounds(ed, &r0, &c0, &r1, &c1);

    if (r0 == r1) {
        char *line = ed->lines[r0];
        int len = (int)strlen(line);
        memmove(line + c0, line + c1, (size_t)(len - c1 + 1));
    }
    else {
        char *first = ed->lines[r0];
        char *last = ed->lines[r1];
        char temp[MAX_LINE_LEN];
        snprintf(temp, sizeof(temp), "%s", last + c1);
        snprintf(first + c0, MAX_LINE_LEN - c0, "%s", temp);

        int lines_deleted = r1 - r0;
        memmove(ed->lines[r0 + 1], ed->lines[r1 + 1],
                (size_t)(ed->line_count - r1 - 1) * MAX_LINE_LEN);
        ed->line_count -= lines_deleted;
    }

    ed->cursor_row = r0;
    ed->cursor_col = c0;
    ed->has_selection = false;
    ed->is_modified = true;
    editor_update_title(ed);
    editor_notify_worker(ed);
}

/* Clipboard operations */
static void editor_copy(struct editor *ed)
{
    char *buf = malloc((size_t)MAX_LINES * MAX_LINE_LEN);
    if (!buf) return;
    buf[0] = '\0';

    if (ed->has_selection) {
        int r0, c0, r1, c1;
        get_selection_bounds(ed, &r0, &c0, &r1, &c1);
        if (r0 == r1) {
            int len = c1 - c0;
            if (len > 0) {
                strncpy(buf, ed->lines[r0] + c0, (size_t)len);
                buf[len] = '\0';
            }
        } else {
            strcat(buf, ed->lines[r0] + c0);
            strcat(buf, "\n");
            for (int r = r0 + 1; r < r1; r++) {
                strcat(buf, ed->lines[r]);
                strcat(buf, "\n");
            }
            strncat(buf, ed->lines[r1], (size_t)c1);
        }
    } else {
        snprintf(buf, (size_t)MAX_LINES * MAX_LINE_LEN, "%s\n", ed->lines[ed->cursor_row]);
    }

    if ((yasl_clipboard_set(YASL_CLIPBOARD, buf) < 0))
        editor_set_status(ed, "Clipboard copy failed");
    free(buf);
    editor_set_status(ed, "Copied to clipboard");
}

static void editor_cut(struct editor *ed)
{
    editor_copy(ed);
    if (ed->has_selection) {
        editor_delete_selection(ed);
    } else {
        editor_save_undo(ed);
        if (ed->line_count > 1) {
            memmove(ed->lines[ed->cursor_row], ed->lines[ed->cursor_row + 1],
                    (size_t)(ed->line_count - ed->cursor_row - 1) * MAX_LINE_LEN);
            ed->line_count--;
            if (ed->cursor_row >= ed->line_count) ed->cursor_row = ed->line_count - 1;
            ed->cursor_col = 0;
            ed->is_modified = true;
            editor_update_title(ed);
        } else {
            ed->lines[0][0] = '\0';
            ed->cursor_col = 0;
        }
    }
}

/* Search & Replace */
static void editor_find_next(struct editor *ed, const char *query, bool forward)
{
    if (!query || !*query) return;
    int qlen = (int)strlen(query);
    snprintf(ed->search_query, sizeof(ed->search_query), "%s", query);

    int start_r = ed->cursor_row;
    int dir = forward ? 1 : -1;

    for (int step = 0; step < ed->line_count; step++) {
        int r = (start_r + step * dir + ed->line_count) % ed->line_count;
        const char *line = ed->lines[r];
        int len = (int)strlen(line);

        if (forward) {
            int start_c = (r == ed->cursor_row && step == 0) ? ed->cursor_col : 0;
            if (start_c < len) {
                const char *match = strstr(line + start_c, query);
                if (match) {
                    int c = (int)(match - line);
                    ed->cursor_row = r;
                    ed->cursor_col = c + qlen;
                    ed->sel_anchor_row = r;
                    ed->sel_anchor_col = c;
                    ed->has_selection = true;
                    editor_set_status(ed, "Found match");
                    return;
                }
            }
        } else {
            /* Backward search on line */
            int limit = (r == ed->cursor_row && step == 0) ? ed->cursor_col - qlen : len - qlen;
            for (int c = limit; c >= 0; c--) {
                if (strncmp(line + c, query, (size_t)qlen) == 0) {
                    ed->cursor_row = r;
                    ed->cursor_col = c + qlen;
                    ed->sel_anchor_row = r;
                    ed->sel_anchor_col = c;
                    ed->has_selection = true;
                    editor_set_status(ed, "Found match");
                    return;
                }
            }
        }
    }
    editor_set_status(ed, "Not found");
}

static void editor_replace_current(struct editor *ed, const char *with)
{
    if (!ed->has_selection) return;
    editor_delete_selection(ed);
    size_t wlen = strlen(with);
    for (size_t i = 0; i < wlen; i++) {
        char *line = ed->lines[ed->cursor_row];
        int len = (int)strlen(line);
        if (len < MAX_LINE_LEN - 2) {
            memmove(line + ed->cursor_col + 1, line + ed->cursor_col, (size_t)(len - ed->cursor_col + 1));
            line[ed->cursor_col++] = with[i];
        }
    }
    ed->is_modified = true;
    editor_update_title(ed);
    editor_set_status(ed, "Replaced match");
}

/* Word navigation */
static int find_word_prev(const char *line, int col)
{
    while (col > 0 && isspace((unsigned char)line[col - 1])) col--;
    while (col > 0 && !isspace((unsigned char)line[col - 1])) col--;
    return col;
}

static int find_word_next(const char *line, int col)
{
    int len = (int)strlen(line);
    while (col < len && !isspace((unsigned char)line[col])) col++;
    while (col < len && isspace((unsigned char)line[col])) col++;
    return col;
}

/* Drawing primitives */
static void fill_rect(uint32_t *pixels, int w, int h, int rx, int ry, int rw, int rh, uint32_t col)
{
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw > w ? w : rx + rw;
    int y1 = ry + rh > h ? h : ry + rh;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = pixels + (size_t)y * w;
        for (int x = x0; x < x1; x++) {
            row[x] = col;
        }
    }
}

static void draw_char(uint32_t *pixels, int w, int h, int px, int py, char c, uint32_t col, int scale)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x8[c - 32];

    for (int y = 0; y < 8; y++) {
        uint8_t row = glyph[y];
        for (int x = 0; x < 8; x++) {
            if (row & (0x80 >> x)) {
                fill_rect(pixels, w, h, px + x * scale, py + y * scale, scale, scale, col);
            }
        }
    }
}

static void draw_string(uint32_t *pixels, int w, int h, int px, int py, const char *str, uint32_t col, int scale)
{
    while (*str) {
        draw_char(pixels, w, h, px, py, *str, col, scale);
        px += 8 * scale;
        str++;
    }
}

/* Editing operations */
static void insert_char_at_cursor(struct editor *ed, char c)
{
    char *line = ed->lines[ed->cursor_row];
    int len = (int)strlen(line);
    if (len >= MAX_LINE_LEN - 2) return;

    memmove(line + ed->cursor_col + 1, line + ed->cursor_col, (size_t)(len - ed->cursor_col + 1));
    line[ed->cursor_col] = c;
    ed->cursor_col++;
    ed->is_modified = true;
    ed->caret_visible = true;
    editor_update_title(ed);
    editor_notify_worker(ed);
}

static void insert_text(struct editor *ed, const char *text, size_t len)
{
    if (ed->has_selection) {
        editor_delete_selection(ed);
    }
    editor_save_undo(ed);

    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            if (ed->line_count >= MAX_LINES - 1) break;
            char *curr = ed->lines[ed->cursor_row];
            char remainder[MAX_LINE_LEN];
            snprintf(remainder, sizeof(remainder), "%s", curr + ed->cursor_col);
            curr[ed->cursor_col] = '\0';
            memmove(ed->lines[ed->cursor_row + 2], ed->lines[ed->cursor_row + 1],
                    (size_t)(ed->line_count - ed->cursor_row - 1) * MAX_LINE_LEN);
            ed->line_count++;
            snprintf(ed->lines[ed->cursor_row + 1], MAX_LINE_LEN, "%s", remainder);
            ed->cursor_row++;
            ed->cursor_col = 0;
        } else if (text[i] == '\t') {
            /* Tab stop: align to next multiple of 4 */
            int spaces = 4 - (ed->cursor_col % 4);
            if (spaces == 0) spaces = 4;
            for (int k = 0; k < spaces; k++) insert_char_at_cursor(ed, ' ');
        } else if (text[i] >= 32 && text[i] <= 126) {
            insert_char_at_cursor(ed, text[i]);
        }
    }
    ed->is_modified = true;
    ed->caret_visible = true;
    editor_update_title(ed);
    editor_notify_worker(ed);
}

static void handle_tab(struct editor *ed, bool unindent)
{
    editor_save_undo(ed);
    if (!unindent) {
        if (!ed->has_selection) {
            int spaces = 4 - (ed->cursor_col % 4);
            if (spaces == 0) spaces = 4;
            for (int k = 0; k < spaces; k++) insert_char_at_cursor(ed, ' ');
        } else {
            int r0, c0, r1, c1;
            get_selection_bounds(ed, &r0, &c0, &r1, &c1);
            for (int r = r0; r <= r1; r++) {
                char *line = ed->lines[r];
                if (strlen(line) + 4 < MAX_LINE_LEN) {
                    memmove(line + 4, line, strlen(line) + 1);
                    memcpy(line, "    ", 4);
                }
            }
            ed->cursor_col += 4;
            ed->sel_anchor_col += 4;
            ed->is_modified = true;
            editor_update_title(ed);
        }
    } else {
        int r0 = ed->cursor_row, r1 = ed->cursor_row;
        if (ed->has_selection) {
            int c0, c1;
            get_selection_bounds(ed, &r0, &c0, &r1, &c1);
        }
        for (int r = r0; r <= r1; r++) {
            char *line = ed->lines[r];
            int spaces = 0;
            while (spaces < 4 && line[spaces] == ' ') spaces++;
            if (spaces > 0) {
                memmove(line, line + spaces, strlen(line) - spaces + 1);
                if (r == ed->cursor_row && ed->cursor_col >= spaces) ed->cursor_col -= spaces;
                ed->is_modified = true;
                editor_update_title(ed);
            }
        }
    }
}

static void handle_backspace(struct editor *ed, bool ctrl)
{
    if (ed->has_selection) {
        editor_delete_selection(ed);
        return;
    }
    editor_save_undo(ed);
    char *line = ed->lines[ed->cursor_row];
    if (ctrl) {
        /* Delete previous word */
        int prev_c = find_word_prev(line, ed->cursor_col);
        int deleted = ed->cursor_col - prev_c;
        if (deleted > 0) {
            int len = (int)strlen(line);
            memmove(line + prev_c, line + ed->cursor_col, (size_t)(len - ed->cursor_col + 1));
            ed->cursor_col = prev_c;
        }
    } else {
        if (ed->cursor_col > 0) {
            int len = (int)strlen(line);
            memmove(line + ed->cursor_col - 1, line + ed->cursor_col, (size_t)(len - ed->cursor_col + 1));
            ed->cursor_col--;
        } else if (ed->cursor_row > 0) {
            char *prev = ed->lines[ed->cursor_row - 1];
            int prev_len = (int)strlen(prev);
            int curr_len = (int)strlen(line);
            if (prev_len + curr_len < MAX_LINE_LEN - 1) {
                strncat(prev, line, (size_t)(MAX_LINE_LEN - prev_len - 1));
                memmove(ed->lines[ed->cursor_row], ed->lines[ed->cursor_row + 1],
                        (size_t)(ed->line_count - ed->cursor_row - 1) * MAX_LINE_LEN);
                ed->line_count--;
                ed->cursor_row--;
                ed->cursor_col = prev_len;
            }
        }
    }
    ed->is_modified = true;
    ed->caret_visible = true;
    editor_update_title(ed);
    editor_notify_worker(ed);
}

static void handle_delete(struct editor *ed, bool ctrl)
{
    if (ed->has_selection) {
        editor_delete_selection(ed);
        return;
    }
    editor_save_undo(ed);
    char *line = ed->lines[ed->cursor_row];
    int len = (int)strlen(line);
    if (ctrl) {
        /* Delete next word */
        int next_c = find_word_next(line, ed->cursor_col);
        int deleted = next_c - ed->cursor_col;
        if (deleted > 0) {
            memmove(line + ed->cursor_col, line + next_c, (size_t)(len - next_c + 1));
        }
    } else {
        if (ed->cursor_col < len) {
            memmove(line + ed->cursor_col, line + ed->cursor_col + 1, (size_t)(len - ed->cursor_col));
        } else if (ed->cursor_row < ed->line_count - 1) {
            char *next = ed->lines[ed->cursor_row + 1];
            int next_len = (int)strlen(next);
            if (len + next_len < MAX_LINE_LEN - 1) {
                strncat(line, next, (size_t)(MAX_LINE_LEN - len - 1));
                memmove(ed->lines[ed->cursor_row + 1], ed->lines[ed->cursor_row + 2],
                        (size_t)(ed->line_count - ed->cursor_row - 2) * MAX_LINE_LEN);
                ed->line_count--;
            }
        }
    }
    ed->is_modified = true;
    ed->caret_visible = true;
    editor_update_title(ed);
    editor_notify_worker(ed);
}

static void handle_enter(struct editor *ed)
{
    if (ed->has_selection) {
        editor_delete_selection(ed);
    }
    editor_save_undo(ed);
    if (ed->line_count >= MAX_LINES - 1) return;

    char *curr = ed->lines[ed->cursor_row];
    char remainder[MAX_LINE_LEN];
    snprintf(remainder, sizeof(remainder), "%s", curr + ed->cursor_col);
    curr[ed->cursor_col] = '\0';
    memmove(ed->lines[ed->cursor_row + 2], ed->lines[ed->cursor_row + 1],
            (size_t)(ed->line_count - ed->cursor_row - 1) * MAX_LINE_LEN);
    ed->line_count++;
    snprintf(ed->lines[ed->cursor_row + 1], MAX_LINE_LEN, "%s", remainder);
    ed->cursor_row++;
    ed->cursor_col = 0;
    ed->is_modified = true;
    ed->caret_visible = true;
    editor_update_title(ed);
    editor_notify_worker(ed);
}

static void insert_date_time(struct editor *ed)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%I:%M %p %m/%d/%Y", tm);
    insert_text(ed, buf, strlen(buf));
}

/* Prompt actions */
static void prompt_execute(struct editor *ed)
{
    switch (ed->prompt) {
    case PROMPT_OPEN:
        editor_open_file(ed, ed->prompt_buf);
        ed->prompt = PROMPT_NONE;
        break;
    case PROMPT_SAVE:
        editor_save_file(ed, ed->prompt_buf);
        ed->prompt = PROMPT_NONE;
        break;
    case PROMPT_FIND:
        editor_find_next(ed, ed->prompt_buf, true);
        ed->prompt = PROMPT_NONE;
        break;
    case PROMPT_REPLACE_FIND:
        snprintf(ed->search_query, sizeof(ed->search_query), "%s", ed->prompt_buf);
        ed->prompt = PROMPT_REPLACE_WITH;
        ed->prompt_len = 0;
        ed->prompt_buf[0] = '\0';
        return;
    case PROMPT_REPLACE_WITH:
        editor_find_next(ed, ed->search_query, true);
        editor_replace_current(ed, ed->prompt_buf);
        ed->prompt = PROMPT_NONE;
        break;
    case PROMPT_GOTO: {
        int target = atoi(ed->prompt_buf);
        if (target > 0) {
            if (target > ed->line_count) target = ed->line_count;
            ed->cursor_row = target - 1;
            ed->cursor_col = 0;
            char msg[64];
            snprintf(msg, sizeof(msg), "Jumped to line %d", target);
            editor_set_status(ed, msg);
        }
        ed->prompt = PROMPT_NONE;
        break;
    }
    case PROMPT_CONFIRM_QUIT:
        if (tolower((unsigned char)ed->prompt_buf[0]) == 'y') {
            if (ed->has_file) {
                editor_save_file(ed, ed->file_path);
            }
        }
        ed->prompt = PROMPT_NONE;
        break;
    default:
        ed->prompt = PROMPT_NONE;
        break;
    }
}

static bool editor_init(struct editor *ed)
{
    ed->surface = yasl_surface_create(&(struct yasl_surface_options){.title="Untitled - YASL Notepad", .width=960, .height=640});
    if (!ed->surface) return false;
    ed->surface_scale = yasl_surface_get_info(ed->surface).scale;

    ed->worker_running = true;
    if (pthread_mutex_init(&ed->worker_mutex, NULL)) return false;
    ed->worker_mutex_ready = true;
    if (pthread_cond_init(&ed->worker_cond, NULL)) return false;
    ed->worker_cond_ready = true;
    if (pthread_create(&ed->worker_tid, NULL, worker_thread, ed)) return false;
    ed->worker_started = true;

    if (ed->has_file) {
        editor_open_file(ed, ed->file_path);
    } else {
        editor_update_title(ed);
    }

    editor_notify_worker(ed);
    editor_request_render(ed);
    ed->running = true;
    return true;
}

static void editor_update_ime(struct editor *ed)
{
    if (!ed->surface || ed->line_count <= 0) return;
    const char *line = ed->lines[ed->cursor_row];
    size_t length = strlen(line);
    size_t begin = (size_t)ed->cursor_col, end = begin;
    if (ed->has_selection && ed->sel_anchor_row == ed->cursor_row) {
        begin = (size_t)(ed->sel_anchor_col < ed->cursor_col ? ed->sel_anchor_col : ed->cursor_col);
        end = (size_t)(ed->sel_anchor_col > ed->cursor_col ? ed->sel_anchor_col : ed->cursor_col);
    }
    if (begin > length) begin = length;
    if (end > length) end = length;
    bool snapshot_changed = !ed->ime_snapshot_ready || strcmp(ed->ime_snapshot, line) ||
                            begin != ed->ime_begin || end != ed->ime_end;
    if (snapshot_changed) {
        snprintf(ed->ime_snapshot, sizeof(ed->ime_snapshot), "%s", line);
        ed->ime_begin = begin;
        ed->ime_end = end;
        ed->ime_snapshot_ready = true;
        ed->text_revision++;
    }
    double scale = yasl_surface_get_info(ed->surface).scale;
    int zoom = (scale >= 2.0 ? 2 : 1) + ed->zoom_offset;
    if (zoom < 1) zoom = 1;
    double caret_x = (48.0 * zoom + 8.0 * zoom + ed->cursor_col * 8.0 * zoom) / scale;
    double caret_y = (24.0 * zoom + (ed->cursor_row - ed->scroll_y) * 16.0 * zoom) / scale;
    if (snapshot_changed || !ed->ime_caret_ready || caret_x != ed->ime_caret_x ||
        caret_y != ed->ime_caret_y) {
        ed->ime_caret_x = caret_x;
        ed->ime_caret_y = caret_y;
        ed->ime_caret_ready = true;
        yasl_surface_set_text_input(ed->surface, &(struct yasl_text_input){
            .text = ed->ime_snapshot, .length = strlen(ed->ime_snapshot),
            .selection_begin = ed->ime_begin, .selection_end = ed->ime_end,
            .revision = ed->text_revision, .caret_x = caret_x, .caret_y = caret_y,
            .caret_width = 2.0, .caret_height = 16.0});
    }
}

static void on_event(struct editor *ed, const yasl_event *ev)
{
    switch (ev->type) {
    case YASL_EVENT_TEXT:
        if (ed->prompt != PROMPT_NONE) {
            for (size_t i = 0; i < ev->length && ed->prompt_len < (int)sizeof(ed->prompt_buf) - 1; i++) {
                if (ev->text[i] >= 32 && ev->text[i] <= 126) {
                    ed->prompt_buf[ed->prompt_len++] = ev->text[i];
                    ed->prompt_buf[ed->prompt_len] = '\0';
                }
            }
        } else {
            insert_text(ed, ev->text, ev->length);
        }
        editor_request_render(ed);
        break;

    case YASL_EVENT_CLIPBOARD:
        if (ev->action != YASL_TRANSFER_OK) {
            editor_set_status(ed, ev->action == YASL_TRANSFER_UNAVAILABLE
                ? "Clipboard has no text" : "Clipboard transfer failed");
            editor_request_render(ed);
            break;
        }
        if (ev->text && ev->length > 0) {
            insert_text(ed, ev->text, ev->length);
            editor_request_render(ed);
        }
        break;

    case YASL_EVENT_TEXT_EDIT:
        if (ed->prompt == PROMPT_NONE && ev->text &&
            ev->ime.revision == ed->text_revision &&
            ev->ime.begin <= ev->ime.end &&
            ev->ime.end <= strlen(ed->ime_snapshot)) {
            ed->cursor_col = (int)ev->ime.begin;
            ed->sel_anchor_row = ed->cursor_row;
            ed->sel_anchor_col = (int)ev->ime.begin;
            ed->cursor_col = (int)ev->ime.end;
            ed->has_selection = ev->ime.begin != ev->ime.end;
            insert_text(ed, ev->text, ev->length);
            editor_request_render(ed);
        }
        break;

    case YASL_EVENT_DROP:
        if (ev->action == YASL_DROP_RELEASE) {
            const char *cursor = ev->text;
            char path[512];
            size_t length;
            while ((length = yasl_drop_next_path(&cursor, path, sizeof(path))) > 0) {
                if (length < sizeof(path)) {
                    editor_open_file(ed, path);
                    editor_request_render(ed);
                    break;
                }
            }
        }
        break;

    case YASL_EVENT_KEY:
        if (ev->action == YASL_KEY_PRESS || ev->action == YASL_KEY_REPEAT) {
            bool ctrl = (ev->mods & (YASL_MOD_CTRL | YASL_MOD_SUPER)) != 0;
            bool shift = (ev->mods & YASL_MOD_SHIFT) != 0;

            /* Interactive Prompt Mode */
            if (ed->prompt != PROMPT_NONE) {
                if (ev->key.key == YASL_KEY_ESCAPE) {
                    ed->prompt = PROMPT_NONE;
                } else if (ev->key.key == YASL_KEY_ENTER) {
                    prompt_execute(ed);
                } else if (ev->key.key == YASL_KEY_BACKSPACE && ed->prompt_len > 0) {
                    ed->prompt_buf[--ed->prompt_len] = '\0';
                }
                editor_request_render(ed);
                break;
            }

            /* Function Keys */
            if (ev->key.key == YASL_KEY_F3) {
                editor_find_next(ed, ed->search_query, !shift);
                editor_request_render(ed);
                return;
            } else if (ev->key.key == YASL_KEY_F5) {
                insert_date_time(ed);
                editor_request_render(ed);
                return;
            }

            /* Shortcuts */
            if (ctrl) {
                switch (ev->key.key) {
                case YASL_KEY_Q:
                    ed->running = false; return;
                case YASL_KEY_N:
                    editor_new_file(ed);
                    editor_request_render(ed);
                    return;
                case YASL_KEY_S:
                    if (shift || !ed->has_file) {
                        ed->prompt = PROMPT_SAVE;
                        ed->prompt_len = 0;
                        ed->prompt_buf[0] = '\0';
                    } else {
                        editor_save_file(ed, ed->file_path);
                    }
                    editor_request_render(ed);
                    return;
                case YASL_KEY_O:
                    ed->prompt = PROMPT_OPEN;
                    ed->prompt_len = 0;
                    ed->prompt_buf[0] = '\0';
                    editor_request_render(ed);
                    return;
                case YASL_KEY_F:
                    ed->prompt = PROMPT_FIND;
                    ed->prompt_len = 0;
                    ed->prompt_buf[0] = '\0';
                    editor_request_render(ed);
                    return;
                case YASL_KEY_H:
                    ed->prompt = PROMPT_REPLACE_FIND;
                    ed->prompt_len = 0;
                    ed->prompt_buf[0] = '\0';
                    editor_request_render(ed);
                    return;
                case YASL_KEY_G:
                    ed->prompt = PROMPT_GOTO;
                    ed->prompt_len = 0;
                    ed->prompt_buf[0] = '\0';
                    editor_request_render(ed);
                    return;
                case YASL_KEY_W:
                    ed->word_wrap = !ed->word_wrap;
                    editor_set_status(ed, ed->word_wrap ? "Word Wrap: ON" : "Word Wrap: OFF");
                    editor_request_render(ed);
                    return;
                case YASL_KEY_EQUAL: /* Zoom In */
                    if (ed->zoom_offset < 2) ed->zoom_offset++;
                    editor_set_status(ed, "Zoom In");
                    editor_request_render(ed);
                    return;
                case YASL_KEY_MINUS: /* Zoom Out */
                    if (ed->zoom_offset > -1) ed->zoom_offset--;
                    editor_set_status(ed, "Zoom Out");
                    editor_request_render(ed);
                    return;
                case YASL_KEY_0: /* Zoom Reset */
                    ed->zoom_offset = 0;
                    editor_set_status(ed, "Zoom Reset");
                    editor_request_render(ed);
                    return;
                case YASL_KEY_A:
                    ed->sel_anchor_row = 0;
                    ed->sel_anchor_col = 0;
                    ed->cursor_row = ed->line_count - 1;
                    ed->cursor_col = (int)strlen(ed->lines[ed->line_count - 1]);
                    ed->has_selection = true;
                    editor_request_render(ed);
                    return;
                case YASL_KEY_C:
                    editor_copy(ed);
                    editor_request_render(ed);
                    return;
                case YASL_KEY_X:
                    editor_cut(ed);
                    editor_request_render(ed);
                    return;
                case YASL_KEY_V:
                    if (yasl_clipboard_request(ed->surface, YASL_CLIPBOARD) < 0)
                        editor_set_status(ed, "Clipboard paste failed");
                    return;
                case YASL_KEY_Z:
                    if (shift) editor_redo(ed);
                    else editor_undo(ed);
                    editor_request_render(ed);
                    return;
                case YASL_KEY_Y:
                    editor_redo(ed);
                    editor_request_render(ed);
                    return;
                case YASL_KEY_BACKSPACE:
                    handle_backspace(ed, true);
                    editor_request_render(ed);
                    return;
                case YASL_KEY_DELETE:
                    handle_delete(ed, true);
                    editor_request_render(ed);
                    return;
                default:
                    break;
                }
            }

            /* Tab key */
            if (ev->key.key == YASL_KEY_TAB) {
                handle_tab(ed, shift);
                editor_request_render(ed);
                break;
            }

            /* Navigation with optional Shift selection */
            bool was_selecting = ed->has_selection;
            if (shift && !was_selecting) {
                ed->sel_anchor_row = ed->cursor_row;
                ed->sel_anchor_col = ed->cursor_col;
                ed->has_selection = true;
            }

            switch (ev->key.key) {
            case YASL_KEY_BACKSPACE:
                handle_backspace(ed, false);
                break;
            case YASL_KEY_DELETE:
                handle_delete(ed, false);
                break;
            case YASL_KEY_ENTER:
                handle_enter(ed);
                break;
            case YASL_KEY_LEFT:
                if (ctrl) {
                    ed->cursor_col = find_word_prev(ed->lines[ed->cursor_row], ed->cursor_col);
                } else if (ed->cursor_col > 0) {
                    ed->cursor_col--;
                } else if (ed->cursor_row > 0) {
                    ed->cursor_row--;
                    ed->cursor_col = (int)strlen(ed->lines[ed->cursor_row]);
                }
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_RIGHT: {
                int len = (int)strlen(ed->lines[ed->cursor_row]);
                if (ctrl) {
                    ed->cursor_col = find_word_next(ed->lines[ed->cursor_row], ed->cursor_col);
                } else if (ed->cursor_col < len) {
                    ed->cursor_col++;
                } else if (ed->cursor_row < ed->line_count - 1) {
                    ed->cursor_row++;
                    ed->cursor_col = 0;
                }
                if (!shift) ed->has_selection = false;
                break;
            }
            case YASL_KEY_UP:
                if (ed->cursor_row > 0) {
                    ed->cursor_row--;
                    int len = (int)strlen(ed->lines[ed->cursor_row]);
                    if (ed->cursor_col > len) ed->cursor_col = len;
                }
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_DOWN:
                if (ed->cursor_row < ed->line_count - 1) {
                    ed->cursor_row++;
                    int len = (int)strlen(ed->lines[ed->cursor_row]);
                    if (ed->cursor_col > len) ed->cursor_col = len;
                }
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_HOME:
                ed->cursor_col = 0;
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_END:
                ed->cursor_col = (int)strlen(ed->lines[ed->cursor_row]);
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_PAGE_UP:
                ed->scroll_y -= 15;
                if (ed->scroll_y < 0) ed->scroll_y = 0;
                ed->cursor_row = ed->scroll_y;
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_PAGE_DOWN:
                ed->scroll_y += 15;
                if (ed->scroll_y >= ed->line_count) ed->scroll_y = ed->line_count - 1;
                ed->cursor_row = ed->scroll_y;
                if (!shift) ed->has_selection = false;
                break;
            case YASL_KEY_ESCAPE:
                ed->has_selection = false;
                break;
            default:
                break;
            }
            ed->caret_visible = true;
            editor_request_render(ed);
        }
        break;

    case YASL_EVENT_POINTER: {
        double device_scale = yasl_surface_get_info(ed->surface).scale;
        int px = (int)lround(ev->x * device_scale);
        int py = (int)lround(ev->y * device_scale);
        if (ev->action == YASL_POINTER_DOWN && ev->pointer.button == YASL_BUTTON_PRIMARY) {
            int scale = (device_scale >= 2.0 ? 2 : 1) + ed->zoom_offset;
            if (scale < 1) scale = 1;
            int char_w = 8 * scale;
            int line_h = 16 * scale;
            int gutter_w = 48 * scale;
            int header_h = 24 * scale;

            int row = (py - header_h) / line_h + ed->scroll_y;
            int col = (px - gutter_w) / char_w + (ed->word_wrap ? 0 : ed->scroll_x);
            if (row < 0) row = 0;
            if (row >= ed->line_count) row = ed->line_count - 1;
            int len = (int)strlen(ed->lines[row]);
            if (col < 0) col = 0;
            if (col > len) col = len;

            uint64_t now = yasl_time_ns();
            if (now - ed->last_click_time < 350000000ull &&
                abs(px - (int)ed->last_click_x) < 8 && abs(py - (int)ed->last_click_y) < 8) {
                ed->click_count++;
            } else {
                ed->click_count = 1;
            }
            ed->last_click_time = now;
            ed->last_click_x = px;
            ed->last_click_y = py;

            if (ed->click_count == 2) {
                /* Double click: select word */
                const char *line = ed->lines[row];
                int c0 = col, c1 = col;
                while (c0 > 0 && !isspace((unsigned char)line[c0 - 1])) c0--;
                while (line[c1] && !isspace((unsigned char)line[c1])) c1++;
                ed->sel_anchor_row = row; ed->sel_anchor_col = c0;
                ed->cursor_row = row; ed->cursor_col = c1;
                ed->has_selection = true;
            } else if (ed->click_count >= 3) {
                /* Triple click: select whole line */
                ed->sel_anchor_row = row; ed->sel_anchor_col = 0;
                ed->cursor_row = row; ed->cursor_col = (int)strlen(ed->lines[row]);
                ed->has_selection = true;
            } else {
                ed->cursor_row = row;
                ed->cursor_col = col;
                ed->sel_anchor_row = row;
                ed->sel_anchor_col = col;
                ed->has_selection = false;
                ed->mouse_dragging = true;
            }
            ed->caret_visible = true;
            editor_request_render(ed);
        } else if (ev->action == YASL_POINTER_UP && ev->pointer.button == YASL_BUTTON_PRIMARY) {
            ed->mouse_dragging = false;
        } else if (ev->action == YASL_POINTER_LEAVE) {
            ed->mouse_dragging = false;
        } else if (ev->action == YASL_POINTER_MOVE && ed->mouse_dragging) {
            int scale = (device_scale >= 2.0 ? 2 : 1) + ed->zoom_offset;
            if (scale < 1) scale = 1;
            int char_w = 8 * scale;
            int line_h = 16 * scale;
            int gutter_w = 48 * scale;
            int header_h = 24 * scale;

            int row = (py - header_h) / line_h + ed->scroll_y;
            int col = (px - gutter_w) / char_w + (ed->word_wrap ? 0 : ed->scroll_x);
            if (row < 0) row = 0;
            if (row >= ed->line_count) row = ed->line_count - 1;
            int len = (int)strlen(ed->lines[row]);
            if (col < 0) col = 0;
            if (col > len) col = len;

            ed->cursor_row = row;
            ed->cursor_col = col;
            ed->has_selection = (ed->cursor_row != ed->sel_anchor_row || ed->cursor_col != ed->sel_anchor_col);
            editor_request_render(ed);
        }
        break;
    }

    case YASL_EVENT_PEN: {
        yasl_event pointer = {0};
        pointer.type = YASL_EVENT_POINTER;
        pointer.surface = ev->surface;
        pointer.x = ev->x;
        pointer.y = ev->y;
        pointer.pointer.button = YASL_BUTTON_PRIMARY;
        switch (ev->action) {
        case YASL_PEN_DOWN: pointer.action = YASL_POINTER_DOWN; break;
        case YASL_PEN_UP: pointer.action = YASL_POINTER_UP; break;
        case YASL_PEN_LEAVE:
        case YASL_PEN_CANCEL: pointer.action = YASL_POINTER_LEAVE; break;
        default: pointer.action = YASL_POINTER_MOVE; break;
        }
        on_event(ed, &pointer);
        break;
    }

    case YASL_EVENT_SCROLL:
        if (ev->mods & (YASL_MOD_CTRL | YASL_MOD_SUPER)) {
            /* Ctrl + Wheel = Zoom */
            if (ev->scroll.lines_y > 0 && ed->zoom_offset < 2) ed->zoom_offset++;
            else if (ev->scroll.lines_y < 0 && ed->zoom_offset > -1) ed->zoom_offset--;
        } else if (ev->mods & YASL_MOD_SHIFT) {
            /* Shift + Wheel = Horizontal scroll */
            ed->scroll_x -= (int)(ev->scroll.lines_y * 3.0);
            if (ed->scroll_x < 0) ed->scroll_x = 0;
        } else {
            /* Normal vertical scroll */
            ed->scroll_y -= (int)(ev->scroll.lines_y * 3.0);
            if (ed->scroll_y < 0) ed->scroll_y = 0;
            if (ed->scroll_y >= ed->line_count) ed->scroll_y = ed->line_count - 1;
        }
        editor_request_render(ed);
        break;

    case YASL_EVENT_CLOSE:
        ed->running = false; return;
    case YASL_EVENT_QUIT:
        ed->running = false; return;

    case YASL_EVENT_WINDOW:
        ed->surface_scale = ev->window.scale;
        if (ev->window.state & YASL_STATE_ACTIVATED) ed->caret_visible = true;
        editor_request_render(ed);
        break;
    case YASL_EVENT_REFRESH:
        editor_request_render(ed);
        break;

    default:
        break;
    }

    return;
}

static void editor_render(struct editor *ed)
{
    int w, h;
    uint32_t *pix = yasl_cpu_pixels(ed->surface, &w, &h);
    if (!pix) return;

    double display_scale = yasl_surface_get_info(ed->surface).scale;
    int scale = (display_scale >= 2.0 ? 2 : 1) + ed->zoom_offset;
    if (scale < 1) scale = 1;
    int char_w = 8 * scale;
    int line_h = 16 * scale;
    int gutter_w = 48 * scale;
    int header_h = 24 * scale;
    int status_h = 24 * scale;

    /* Theme colors */
    uint32_t bg_main = 0xff1e1e2e;
    uint32_t bg_header = 0xff181825;
    uint32_t bg_gutter = 0xff181825;
    uint32_t bg_active_line = 0xff252538;
    uint32_t bg_selection = 0xff3b4261;
    uint32_t bg_status = 0xff11111b;
    uint32_t bg_prompt = 0xff2a2b3d;
    uint32_t text_fg = 0xffcdd6f4;
    uint32_t text_gutter = 0xff6c7086;
    uint32_t text_header = 0xff89b4fa;
    uint32_t text_status = 0xffa6adc8;
    uint32_t caret_col = 0xfff38ba8;
    uint32_t status_accent = 0xffa6e3a1;

    /* 1. Clear background */
    fill_rect(pix, w, h, 0, 0, w, h, bg_main);

    /* 2. Top Header Menu bar */
    fill_rect(pix, w, h, 0, 0, w, header_h, bg_header);
    char header_str[256];
    snprintf(header_str, sizeof(header_str),
             "^N New  ^O Open  ^S Save  ^F Find  ^H Repl  ^G Goto  ^Z Undo  F5 Time  ^W Wrap");
    draw_string(pix, w, h, 12 * scale, 6 * scale, header_str, text_header, scale);

    /* 3. Gutter background */
    fill_rect(pix, w, h, 0, header_h, gutter_w, h - header_h - status_h, bg_gutter);

    /* 4. Ensure cursor is in visible vertical and horizontal bounds */
    int visible_lines = (h - header_h - status_h) / line_h;
    if (ed->cursor_row < ed->scroll_y) {
        ed->scroll_y = ed->cursor_row;
    } else if (ed->cursor_row >= ed->scroll_y + visible_lines) {
        ed->scroll_y = ed->cursor_row - visible_lines + 1;
    }

    int visible_cols = (w - gutter_w - 16 * scale) / char_w;
    if (visible_cols < 1) visible_cols = 1;
    if (!ed->word_wrap) {
        if (ed->cursor_col < ed->scroll_x) {
            ed->scroll_x = ed->cursor_col;
        } else if (ed->cursor_col >= ed->scroll_x + visible_cols) {
            ed->scroll_x = ed->cursor_col - visible_cols + 1;
        }
    } else {
        ed->scroll_x = 0;
    }

    /* 5. Active line highlight */
    int active_line_y = header_h + (ed->cursor_row - ed->scroll_y) * line_h;
    if (active_line_y >= header_h && active_line_y < h - status_h) {
        fill_rect(pix, w, h, gutter_w, active_line_y, w - gutter_w, line_h, bg_active_line);
    }

    /* 6. Render lines, gutter, and selection */
    for (int i = 0; i < visible_lines; i++) {
        int line_idx = ed->scroll_y + i;
        if (line_idx >= ed->line_count) break;

        int y = header_h + i * line_h + 4 * scale;

        /* Line number */
        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "%3d", line_idx + 1);
        draw_string(pix, w, h, 8 * scale, y, num_buf, text_gutter, scale);

        /* Text and selection highlighting */
        const char *line_text = ed->lines[line_idx];
        int text_x = gutter_w + 8 * scale;
        int col = ed->scroll_x;

        while (line_text[col] && col - ed->scroll_x < visible_cols + 1) {
            int char_x = text_x + (col - ed->scroll_x) * char_w;
            if (is_char_selected(ed, line_idx, col)) {
                fill_rect(pix, w, h, char_x, header_h + i * line_h, char_w, line_h, bg_selection);
            }
            draw_char(pix, w, h, char_x, y, line_text[col], text_fg, scale);
            col++;
        }
    }

    /* 7. Caret */
    if (ed->caret_visible && active_line_y >= header_h && active_line_y < h - status_h) {
        int caret_x = gutter_w + 8 * scale + (ed->cursor_col - ed->scroll_x) * char_w;
        if (caret_x >= gutter_w && caret_x < w) {
            int caret_y = active_line_y + 3 * scale;
            fill_rect(pix, w, h, caret_x, caret_y, 2 * scale, 10 * scale, caret_col);
        }
    }

    /* 8. Bottom Status bar or Mini-buffer Prompt */
    int status_y = h - status_h;
    if (ed->prompt != PROMPT_NONE) {
        fill_rect(pix, w, h, 0, status_y, w, status_h, bg_prompt);
        const char *prompt_label = ed->prompt == PROMPT_OPEN ? "Open File: " :
                                   ed->prompt == PROMPT_SAVE ? "Save As: " :
                                   ed->prompt == PROMPT_FIND ? "Find: " :
                                   ed->prompt == PROMPT_REPLACE_FIND ? "Replace (Find): " :
                                   ed->prompt == PROMPT_REPLACE_WITH ? "Replace with: " :
                                   ed->prompt == PROMPT_GOTO ? "Go to Line: " :
                                   "Save changes before exit? (y/n/esc): ";
        char prompt_disp[350];
        snprintf(prompt_disp, sizeof(prompt_disp), "%s%s_", prompt_label, ed->prompt_buf);
        draw_string(pix, w, h, 12 * scale, status_y + 6 * scale, prompt_disp, 0xfffab387, scale);
    } else {
        fill_rect(pix, w, h, 0, status_y, w, status_h, bg_status);

        char status_left[160];
        if (ed->status_msg[0] && (yasl_time_ns() - ed->status_msg_time < 3000000000ull)) {
            snprintf(status_left, sizeof(status_left), "%s", ed->status_msg);
        } else {
            pthread_mutex_lock(&ed->worker_mutex);
            struct doc_stats stats = ed->stats;
            pthread_mutex_unlock(&ed->worker_mutex);
            snprintf(status_left, sizeof(status_left), "Ln %d, Col %d   Lines: %d   Words: %d   Chars: %d",
                     ed->cursor_row + 1, ed->cursor_col + 1, ed->line_count, stats.words, stats.chars);
        }
        draw_string(pix, w, h, 12 * scale, status_y + 6 * scale, status_left, text_status, scale);

        char status_right[96];
        snprintf(status_right, sizeof(status_right), "%s  [Wrap:%s]  [0%% CPU]",
                 ed->is_modified ? "[Modified]" : "[Saved]",
                 ed->word_wrap ? "ON" : "OFF");
        int right_w = (int)strlen(status_right) * char_w;
        draw_string(pix, w, h, w - right_w - 12 * scale, status_y + 6 * scale, status_right,
                    ed->is_modified ? 0xfffab387 : status_accent, scale);
    }

    if (yasl_cpu_present(ed->surface) < 0) {
        fprintf(stderr, "Could not present CPU image\n");
        ed->failed = true;
        ed->running = false;
        return;
    }
    ed->dirty = false;
}

static void editor_shutdown(struct editor *ed)
{
    ed->worker_running = false;
    if (ed->worker_started) {
        pthread_mutex_lock(&ed->worker_mutex);
        pthread_cond_broadcast(&ed->worker_cond);
        pthread_mutex_unlock(&ed->worker_mutex);
        pthread_join(ed->worker_tid, NULL);
    }
    free(ed->worker_lines);
    if (ed->worker_cond_ready) pthread_cond_destroy(&ed->worker_cond);
    if (ed->worker_mutex_ready) pthread_mutex_destroy(&ed->worker_mutex);
}

int main(int argc, char **argv)
{
    static struct editor ed;
    bool smoke_test = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--smoke-test")) smoke_test = true;
        else if (!ed.has_file) {
            snprintf(ed.file_path, sizeof(ed.file_path), "%s", argv[i]);
            ed.has_file = true;
        }
    }
    if (!ed.has_file) {
        const char *initial_content[] = {
            "// Welcome to YASL Notepad!",
            "// A full-featured desktop text editor built with YASL.",
            "",
            "#include <stdio.h>",
            "",
            "int main(int argc, char **argv) {",
            "    printf(\"Hello, modern YASL!\\n\");",
            "    return 0;",
            "}",
            "",
            "Comprehensive Features & Shortcuts:",
            "  - New Document:       Ctrl+N",
            "  - Open File:          Ctrl+O  (or drag-and-drop file onto window)",
            "  - Save / Save As:     Ctrl+S / Ctrl+Shift+S",
            "  - Find / Find Next:   Ctrl+F / F3 (Shift+F3 for prev)",
            "  - Replace:            Ctrl+H",
            "  - Go to Line:         Ctrl+G",
            "  - Insert Date/Time:   F5",
            "  - Word Wrap Toggle:   Ctrl+W",
            "  - Zoom In/Out/Reset:  Ctrl+= / Ctrl+- / Ctrl+0 (or Ctrl+Wheel)",
            "  - Copy / Cut / Paste: Ctrl+C / Ctrl+X / Ctrl+V",
            "  - Undo / Redo:        Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z)",
            "  - Select All:         Ctrl+A",
            "  - Mouse Selection:    Click+Drag, Double-click (word), Triple-click (line)",
            "  - Word Navigation:    Ctrl+Left / Ctrl+Right",
            "  - Word Deletion:      Ctrl+Backspace / Ctrl+Delete",
            "  - Tab / Shift+Tab:    Align 4-space tab stops / unindent",
            "",
            "Try editing, typing, or opening files!"
        };
        ed.line_count = sizeof(initial_content) / sizeof(initial_content[0]);
        for (int i = 0; i < ed.line_count; i++) {
            snprintf(ed.lines[i], MAX_LINE_LEN, "%s", initial_content[i]);
        }
    }

    if (yasl_init(&(struct yasl_options){.app_id = "org.yasl.notepad"}) < 0 || !editor_init(&ed)) {
        fprintf(stderr, "Could not initialize YASL Notepad\n");
        editor_shutdown(&ed);
        yasl_shutdown();
        return 1;
    }

    editor_update_ime(&ed);
    editor_render(&ed);
    if (smoke_test) {
        editor_shutdown(&ed);
        yasl_surface_destroy(ed.surface);
        yasl_shutdown();
        return ed.failed ? 1 : 0;
    }

    uint64_t next_blink = yasl_time_ns() + 500000000ull;
    uint64_t seen_stats = 0;
    int exit_status = 0;
    while (ed.running) {
        uint64_t now = yasl_time_ns();
        int timeout_ms = next_blink > now ? (int)((next_blink - now + 999999ull) / 1000000ull) : 0;
        yasl_event ev;
        int result = yasl_wait_event(&ev, timeout_ms);
        if (result > 0) {
            on_event(&ed, &ev);
        } else if (result < 0) {
            fprintf(stderr, "YASL event wait failed\n");
            exit_status = 1;
            break;
        }
        int queued = 0;
        while (ed.running && (queued = yasl_wait_event(&ev, 0)) > 0)
            on_event(&ed, &ev);
        if (queued < 0) {
            fprintf(stderr, "YASL event poll failed\n");
            exit_status = 1;
            break;
        }

        now = yasl_time_ns();
        if (now >= next_blink) {
            ed.caret_visible = !ed.caret_visible;
            ed.dirty = true;
            next_blink = now + 500000000ull;
        }
        pthread_mutex_lock(&ed.worker_mutex);
        uint64_t stats_generation = ed.stats_generation;
        pthread_mutex_unlock(&ed.worker_mutex);
        if (stats_generation != seen_stats) {
            seen_stats = stats_generation;
            ed.dirty = true;
        }
        editor_update_ime(&ed);
        if (ed.dirty) {
            editor_render(&ed);
            editor_update_ime(&ed);
        }
    }

    editor_shutdown(&ed);
    yasl_surface_destroy(ed.surface);
    yasl_shutdown();
    return exit_status || ed.failed ? 1 : 0;
}
