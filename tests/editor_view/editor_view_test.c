#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "editor/annotations.h"
#include "editor/document.h"
#include "editor_view/editor_view.h"
#include "ui/debug.h"
#include "ui/event.h"
#include "ui/widget_ops.h"
#include "ui/surface.h"
#include "ui/ui.h"
#include "ui/widget_ops.h"

/* The widget tests of the editor view. They drive the public API and the
 * public events, and they read the scene or the documented stats. */

struct fixture {
    struct ui_surface surface;
    struct ui_editor_view view;
    struct ui_scene scene;
    struct css_sheet sheet;
    char *copied;
};

static void copy_text(void *userdata, const char *text)
{
    struct fixture *f = userdata;
    free(f->copied);
    f->copied = strdup(text);
    assert(f->copied);
}

static void init(struct fixture *f, struct vector2i size)
{
    *f = (struct fixture){0};
    const struct ui_surface_host host = { .copy_text = copy_text };
    ui_surface_init(&f->surface, &host, f);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_editor_view_init(&f->view);
    ui_surface_set_root(&f->surface, ui_editor_view_widget(&f->view));
    ui_surface_resize(&f->surface, size);
}

static void destroy(struct fixture *f)
{
    ui_editor_view_clear(&f->view);
    ui_surface_destroy(&f->surface);
    free(f->copied);
}

static void paint(struct fixture *f)
{
    ui_scene_destroy(&f->scene);
    ui_scene_init(&f->scene);
    struct ui_paint_context paint = { .scene = &f->scene,
                                      .size = f->view.widget.bounds.size };
    f->view.widget.ops->paint(&f->view.widget, &paint);
}

static void key(struct fixture *f, enum ui_key key, unsigned mods)
{
    ui_widget_event(&f->view.widget, &(struct ui_event){
        .kind = UI_EVENT_KEY,
        .time_ms = 1,
        .data.key = { .key = key, .mods = mods, .action = UI_KEY_PRESS },
    });
}

static void text(struct fixture *f, const char *bytes)
{
    ui_widget_event(&f->view.widget, &(struct ui_event){
        .kind = UI_EVENT_TEXT,
        .time_ms = 1,
        .data.text = { .data = bytes, .len = strlen(bytes) },
    });
}

static void press(struct fixture *f, struct vector2i pos)
{
    ui_widget_event(&f->view.widget, &(struct ui_event){
        .kind = UI_EVENT_POINTER,
        .time_ms = 1,
        .data.pointer = { .kind = UI_POINTER_PRESS, .pos = pos, .button = 0 },
    });
}

/* Builds a document with `lines` numbered lines. */
static char *numbered_document(int lines, size_t *length)
{
    size_t cap = (size_t)lines * 32 + 1;
    char *text_bytes = malloc(cap);
    assert(text_bytes);
    size_t off = 0;
    for (int i = 0; i < lines; i++)
        off += (size_t)snprintf(text_bytes + off, cap - off,
                                "line %d of the document\n", i);
    *length = off;
    return text_bytes;
}

static void test_open_10k_lines(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    size_t length = 0;
    char *bytes = numbered_document(10000, &length);
    assert(ui_editor_view_set_text(&f.view, bytes, length));
    free(bytes);
    assert(editor_text_line_count(editor_document_text(
               ui_editor_view_document(&f.view))) == 10001);
    paint(&f);
    /* The view shows the top of the document and paints only its rows. */
    assert(ui_editor_view_first_line(&f.view) == 0);
    struct ui_editor_paint_stats stats = ui_editor_view_paint_stats(&f.view);
    assert(stats.rows_painted > 0);
    assert(stats.rows_painted < 30);
    assert(f.scene.n_ops > 0);
    destroy(&f);
    puts("ok open 10k lines");
}

static void test_scroll_to_end(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    size_t length = 0;
    char *bytes = numbered_document(10000, &length);
    assert(ui_editor_view_set_text(&f.view, bytes, length));
    free(bytes);
    paint(&f);
    ui_editor_view_scroll_to_line(&f.view, 10000);
    paint(&f);
    assert(ui_editor_view_first_line(&f.view) == 10000);
    /* The last line holds the caret band and the text. */
    struct ui_editor_paint_stats stats = ui_editor_view_paint_stats(&f.view);
    assert(stats.rows_painted >= 1);
    assert(ui_editor_view_line_height(&f.view, 10000) == 16);
    destroy(&f);
    puts("ok scroll to end");
}

static void test_one_row_repaint(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    assert(ui_editor_view_set_text(&f.view,
        "first line\nsecond line\nthird line\nfourth line\n", 44));
    paint(&f);
    int painted = ui_editor_view_paint_stats(&f.view).rows_painted;
    assert(painted == 4);
    /* Edit one line. Only its row rebuilds its spans. */
    uint64_t offset = 11; /* The start of "second". */
    assert(ui_editor_view_replace(&f.view, offset, 0, "new ", 4));
    assert(ui_editor_view_replace(&f.view, offset, 0, "", 0));
    paint(&f);
    struct ui_editor_paint_stats stats = ui_editor_view_paint_stats(&f.view);
    assert(stats.rows_painted == painted);
    assert(stats.rows_rebuilt == 1);
    destroy(&f);
    puts("ok one row repaint");
}

static void test_fold_hides_range(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    assert(ui_editor_view_set_text(&f.view,
        "header\nbody one\nbody two\nfooter\n", 33));
    assert(editor_text_line_count(editor_document_text(
               ui_editor_view_document(&f.view))) == 5);
    assert(ui_editor_view_visible_line_count(&f.view) == 5);
    struct editor_folds *folds = editor_folds_create();
    assert(folds);
    assert(editor_folds_add(folds, 0, 2, "header"));
    assert(ui_editor_view_set_folds(&f.view, folds));
    editor_folds_destroy(folds);
    assert(ui_editor_view_visible_line_count(&f.view) == 3);
    assert(ui_editor_view_line_hidden(&f.view, 1));
    assert(ui_editor_view_line_hidden(&f.view, 2));
    assert(!ui_editor_view_line_hidden(&f.view, 0));
    assert(ui_editor_view_line_height(&f.view, 1) == 0);
    paint(&f);
    /* The gutter shows the fold marker. The fold toggles it again. */
    assert(ui_editor_view_toggle_fold(&f.view, 0));
    assert(!ui_editor_view_line_hidden(&f.view, 1));
    assert(ui_editor_view_visible_line_count(&f.view) == 5);
    destroy(&f);
    puts("ok fold hides range");
}

static void test_gutter_mark(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    assert(ui_editor_view_set_text(&f.view, "one\ntwo\nthree", 13));
    ui_editor_view_set_mark(&f.view, 1, UI_EDITOR_MARK_BREAKPOINT);
    assert(ui_editor_view_mark(&f.view, 1) == UI_EDITOR_MARK_BREAKPOINT);
    assert(ui_editor_view_mark(&f.view, 0) == UI_EDITOR_MARK_NONE);
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(&f.view.widget, &parts);
    bool found = false;
    for (int i = 0; i < parts.n; i++)
        if (!strcmp(parts.items[i].name, "gutter-mark") &&
            parts.items[i].index == 1)
            found = true;
    assert(found);
    ui_debug_parts_free(&parts);
    /* A press on the mark cell toggles the breakpoint off. */
    struct rect2i content = f.view.widget.bounds;
    press(&f, (struct vector2i){ .x = content.x + 2, .y = content.y + 16 + 4 });
    assert(ui_editor_view_mark(&f.view, 1) == UI_EDITOR_MARK_NONE);
    destroy(&f);
    puts("ok gutter mark");
}

static void test_caret_wide_char(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    /* U+4E16 is one code point in two cells. */
    assert(ui_editor_view_set_text(&f.view, "a\xE4\xB8\x96" "b", 5));
    assert(ui_editor_view_set_caret(&f.view, (struct editor_position){0, 0}));
    assert(ui_editor_view_caret_offset(&f.view) == 0);
    key(&f, UI_KEY_RIGHT, 0);
    assert(ui_editor_view_caret_offset(&f.view) == 1);
    assert(ui_editor_view_caret(&f.view).character == 1);
    key(&f, UI_KEY_RIGHT, 0);
    /* One press crosses the whole wide character. */
    assert(ui_editor_view_caret_offset(&f.view) == 4);
    assert(ui_editor_view_caret(&f.view).character == 2);
    key(&f, UI_KEY_RIGHT, 0);
    assert(ui_editor_view_caret_offset(&f.view) == 5);
    key(&f, UI_KEY_LEFT, 0);
    assert(ui_editor_view_caret_offset(&f.view) == 4);
    destroy(&f);
    puts("ok caret wide char");
}

static void test_selection_spans_lines(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    assert(ui_editor_view_set_text(&f.view, "alpha\nbravo\ncharlie\n", 20));
    assert(ui_editor_view_set_caret(&f.view, (struct editor_position){2, 3}));
    assert(ui_editor_view_select(&f.view, 2));
    uint64_t start = 0, end = 0;
    assert(ui_editor_view_selection(&f.view, &start, &end));
    assert(start == 2 && end == 15);
    struct rect2i rects[8];
    size_t count = ui_editor_view_selection_rects(&f.view, rects,
                                                  sizeof(rects) / sizeof(*rects));
    assert(count == 3);
    assert(rects[0].y < rects[1].y && rects[1].y < rects[2].y);
    /* The first rectangle starts at the selection start cell. */
    assert(rects[0].x > f.view.widget.bounds.x);
    destroy(&f);
    puts("ok selection spans lines");
}

static void test_wrap_and_inlay_height(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 120, .y = 320 });
    /* 40 cells do not fit the 120 px wide view, so the row wraps. */
    assert(ui_editor_view_set_text(&f.view,
        "0123456789012345678901234567890123456789\nshort\n", 44));
    int plain = ui_editor_view_line_rows(&f.view, 0);
    assert(plain == 1);
    ui_editor_view_set_wrap(&f.view, true);
    assert(ui_editor_view_wrap(&f.view));
    assert(ui_editor_view_line_rows(&f.view, 0) > 1);
    assert(ui_editor_view_line_height(&f.view, 0) >
           ui_editor_view_line_height(&f.view, 1));
    /* An inline hint and a code lens band add height. */
    int before = ui_editor_view_line_height(&f.view, 1);
    assert(ui_editor_view_add_inlay(&f.view, 1, "hint",
                                    UI_EDITOR_INLAY_INLINE));
    assert(ui_editor_view_add_inlay(&f.view, 1, "3 references",
                                    UI_EDITOR_INLAY_ABOVE));
    assert(ui_editor_view_line_height(&f.view, 1) == before + 32);
    destroy(&f);
    puts("ok wrap and inlay height");
}

static void test_sticky_header_and_ticks(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 160 });
    size_t length = 0;
    char *bytes = numbered_document(200, &length);
    assert(ui_editor_view_set_text(&f.view, bytes, length));
    free(bytes);
    assert(ui_editor_view_add_fold(&f.view, 0, 50, "block"));
    assert(ui_editor_view_add_fold(&f.view, 20, 30, "inner"));
    assert(ui_editor_view_toggle_fold(&f.view, 0));
    assert(ui_editor_view_toggle_fold(&f.view, 20));
    paint(&f);
    ui_editor_view_scroll_to_line(&f.view, 25);
    paint(&f);
    uint32_t line = UINT32_MAX;
    assert(ui_editor_view_sticky_line(&f.view, &line));
    /* The innermost range holds the header of line 20. */
    assert(line == 20);
    ui_editor_view_scroll_to_line(&f.view, 40);
    paint(&f);
    assert(ui_editor_view_sticky_line(&f.view, &line));
    assert(line == 0);
    uint32_t matches[] = { 10, 100, 190 };
    assert(ui_editor_view_set_matches(&f.view, matches, 3));
    paint(&f);
    destroy(&f);
    puts("ok sticky header and ticks");
}

static void test_preedit(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    assert(ui_editor_view_set_text(&f.view, "text", 4));
    ui_editor_view_set_preedit(&f.view, "ka", 2);
    size_t length = 0;
    assert(!strcmp(ui_editor_view_preedit(&f.view, &length), "ka"));
    assert(length == 2);
    paint(&f);
    ui_editor_view_set_preedit(&f.view, NULL, 0);
    assert(ui_editor_view_preedit(&f.view, &length)[0] == '\0');
    assert(length == 0);
    destroy(&f);
    puts("ok preedit");
}

static void test_edit_typing(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    assert(ui_editor_view_set_text(&f.view, "one\ntwo", 7));
    assert(ui_editor_view_set_caret(&f.view, (struct editor_position){0, 3}));
    text(&f, "!");
    assert(editor_text_length(editor_document_text(
               ui_editor_view_document(&f.view))) == 8);
    key(&f, UI_KEY_ENTER, 0);
    assert(ui_editor_view_caret(&f.view).line == 1);
    key(&f, UI_KEY_BACKSPACE, 0);
    assert(ui_editor_view_caret(&f.view).line == 0);
    text(&f, "x");
    assert(ui_editor_view_selection(&f.view, NULL, NULL) == false);
    destroy(&f);
    puts("ok typing");
}

/* Reports the frame cost of a scroll on the 10k-line document. The test checks
 * only that the frames paint; the timing goes to the log. */
static void test_scroll_frame_cost(void)
{
    struct fixture f;
    init(&f, (struct vector2i){ .x = 400, .y = 320 });
    size_t length = 0;
    char *bytes = numbered_document(10000, &length);
    assert(ui_editor_view_set_text(&f.view, bytes, length));
    free(bytes);
    paint(&f);
    enum { FRAMES = 60 };
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < FRAMES; i++) {
        ui_editor_view_scroll_lines(&f.view, 10);
        paint(&f);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double micros = ((double)(end.tv_sec - start.tv_sec) * 1e9 +
                     (double)(end.tv_nsec - start.tv_nsec)) / 1e3 / FRAMES;
    struct ui_editor_paint_stats stats = ui_editor_view_paint_stats(&f.view);
    printf("scroll frame: %.1f us, rows %d, rows rebuilt %d\n", micros,
           stats.rows_painted, stats.rows_rebuilt);
    assert(stats.rows_painted > 0);
    destroy(&f);
}

int main(void)
{
    ui_widget_shutdown();
    test_open_10k_lines();
    test_scroll_to_end();
    test_one_row_repaint();
    test_fold_hides_range();
    test_gutter_mark();
    test_caret_wide_char();
    test_selection_spans_lines();
    test_wrap_and_inlay_height();
    test_sticky_header_and_ticks();
    test_preedit();
    test_edit_typing();
    test_scroll_frame_cost();
    ui_widget_shutdown();
    puts("editor-view-test: all tests passed");
    return 0;
}
