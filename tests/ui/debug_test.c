#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/debug.h"
#include "ui/internal/tree.h"
#include "ui/paint.h"
#include "paint/scene.h"
#include "ui/widget.h"
#include "util/json.h"

/* A widget that paints its bounds and, optionally, one text run at a
 * fixed offset inside a named part. */
struct probe {
    struct ui_widget widget;
    uint32_t color;
    const char *text;
    struct vector2i text_at;
    struct rect2i part;
    bool has_part;
};

static void probe_paint(const struct ui_widget *w, struct ui_paint_context *paint)
{
    const struct probe *p = w->userdata;
    ui_paint_rect(paint, ui_paint_bounds(paint), p->color);
    if (p->text) {
        struct ui_text_style style = { .advance = 8, .cell_h = 16 };
        struct rect2i r = { .pos = p->text_at,
                            .size = { .x = 8 * (int)strlen(p->text), .y = 16 } };
        ui_paint_text(paint, r, style, 0xFFFFFFFF, p->text, strlen(p->text));
    }
}

static void probe_parts(const struct ui_widget *w, struct ui_debug_parts *out)
{
    const struct probe *p = w->userdata;
    if (p->has_part)
        ui_debug_part(out, "row", 0, p->part);
}

static const struct ui_widget_ops PROBE_OPS = {
    .name = "probe",
    .paint = probe_paint,
    .debug_parts = probe_parts,
};

static void probe_init(struct probe *p, struct rect2i bounds, uint32_t color)
{
    *p = (struct probe){ .color = color };
    ui_widget_init(&p->widget, &PROBE_OPS, p);
    p->widget.bounds = bounds;
}

static struct rect2i R(int x, int y, int w, int h)
{
    return (struct rect2i){ .pos = { .x = x, .y = y }, .size = { .x = w, .y = h } };
}

struct traced {
    struct ui_scene scene;
};

static struct ui_debug_view build(struct traced *t, struct ui_widget *root)
{
    assert(ui_widget_build_scene(root, &t->scene));
    return (struct ui_debug_view){ .root = root, .scene = &t->scene };
}

static void selectors_resolve_test(void)
{
    struct probe root, a, b;
    probe_init(&root, R(0, 0, 100, 100), 0xFF000000);
    probe_init(&a, R(0, 0, 50, 50), 0xFF111111);
    probe_init(&b, R(50, 0, 50, 50), 0xFF222222);
    ui_widget_set_style_identity(&b.widget, "chip", "second", NULL);
    assert(ui_widget_add_child(&root.widget, &a.widget));
    assert(ui_widget_add_child(&root.widget, &b.widget));

    assert(ui_debug_find(&root.widget, "/") == &root.widget);
    assert(ui_debug_find(&root.widget, "/1") == &b.widget);
    assert(ui_debug_find(&root.widget, "/2") == NULL);
    assert(ui_debug_find(&root.widget, ".chip") == &b.widget);
    assert(ui_debug_find(&root.widget, "#second") == &b.widget);
    assert(ui_debug_find(&root.widget, "probe") == &root.widget);

    char sel[32];
    snprintf(sel, sizeof(sel), "%p", (void *)&a.widget);
    assert(ui_debug_find(&root.widget, sel) == &a.widget);
    struct probe stray;
    probe_init(&stray, R(0, 0, 1, 1), 0);
    snprintf(sel, sizeof(sel), "%p", (void *)&stray.widget);
    assert(ui_debug_find(&root.widget, sel) == NULL);

    char path[16];
    assert(ui_debug_path(&root.widget, &b.widget, path, sizeof(path)));
    assert(!strcmp(path, "/1"));
    assert(!ui_debug_path(&root.widget, &stray.widget, path, sizeof(path)));
}

/* The visible widget wins: later siblings paint on top. A widget that
 * reports hit regions occupies only them, not its whole bounds. */
static void widget_at_matches_what_is_visible_test(void)
{
    struct probe root, under, over, canvas;
    probe_init(&root, R(0, 0, 200, 100), 0xFF000000);
    probe_init(&under, R(0, 0, 100, 100), 0xFF111111);
    probe_init(&over, R(50, 0, 100, 100), 0xFF222222);
    probe_init(&canvas, R(0, 0, 200, 100), 0xFF333333);
    canvas.has_part = true;
    canvas.part = R(180, 0, 20, 20);
    assert(ui_widget_add_child(&root.widget, &under.widget));
    assert(ui_widget_add_child(&root.widget, &over.widget));
    assert(ui_widget_add_child(&root.widget, &canvas.widget));

    struct vector2i shared = { .x = 75, .y = 50 };
    assert(ui_debug_widget_at(&root.widget, shared) == &over.widget);
    assert(ui_debug_widget_at(&root.widget, (struct vector2i){ .x = 10, .y = 10 })
           == &under.widget);
    assert(ui_debug_widget_at(&root.widget, (struct vector2i){ .x = 190, .y = 5 })
           == &canvas.widget);
    assert(ui_debug_widget_at(&root.widget, (struct vector2i){ .x = 190, .y = 90 })
           == &root.widget);
}

static void ops_are_attributed_to_their_widget_test(void)
{
    struct probe root, child;
    probe_init(&root, R(0, 0, 100, 100), 0xFF000000);
    probe_init(&child, R(10, 10, 20, 20), 0xFF00FF00);
    assert(ui_widget_add_child(&root.widget, &child.widget));

    static struct traced t;
    struct ui_debug_view view = build(&t, &root.widget);
    int child_op = -1;
    for (int i = 0; i < t.scene.n_ops; i++)
        if (t.scene.ops[i].color == 0xFF00FF00)
            child_op = i;
    assert(child_op >= 0);
    assert(ui_debug_op_owner(&view, child_op) == &child.widget);
    assert(ui_debug_op_owner(&view, 0) == &root.widget);

    /* A destroyed widget is never reported, even from an old trace. */
    ui_widget_remove_child(&root.widget, &child.widget);
    assert(ui_debug_op_owner(&view, child_op) != &child.widget);
}

static int lint_count(struct probe *root, enum ui_debug_lint_kind kind)
{
    static struct traced t;
    struct ui_debug_view view = build(&t, &root->widget);
    struct ui_debug_lint found[8];
    int n = ui_debug_lint_scene(&view, found, 8);
    int k = 0;
    for (int i = 0; i < n && i < 8; i++)
        k += found[i].kind == kind;
    return k;
}

static void lint_flags_off_center_text_test(void)
{
    struct probe root;
    probe_init(&root, R(0, 0, 200, 200), 0xFF000000);
    root.has_part = true;
    root.part = R(0, 0, 200, 24);
    root.text = "row";

    root.text_at = (struct vector2i){ .x = 8, .y = 4 };
    assert(lint_count(&root, UI_DEBUG_LINT_V_CENTER) == 0);
    root.text_at = (struct vector2i){ .x = 8, .y = 0 };
    assert(lint_count(&root, UI_DEBUG_LINT_V_CENTER) == 1);
    root.text_at = (struct vector2i){ .x = 190, .y = 4 };
    assert(lint_count(&root, UI_DEBUG_LINT_OVERFLOW) == 1);
}

/* The overlay draws after the UI's own ops, leaves them untouched, and
 * never makes a scene invalid, however large it already is. */
static void overlay_never_breaks_the_scene_test(void)
{
    struct probe root, child;
    probe_init(&root, R(0, 0, 100, 100), 0xFF000000);
    probe_init(&child, R(10, 10, 20, 20), 0xFF00FF00);
    child.text = "hi";
    assert(ui_widget_add_child(&root.widget, &child.widget));
    root.widget.style_scale = (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    };

    static struct traced t;
    struct ui_debug_view view = build(&t, &root.widget);
    struct ui_debug_overlay overlay = {
        .flags = ~0u, .pointer = { .x = 15, .y = 15 }, .has_pointer = true,
    };
    int before = t.scene.n_ops;
    size_t bytes = (size_t)before * sizeof(t.scene.ops[0]);
    struct ui_scene_op *snapshot = malloc(bytes);
    assert(snapshot);
    memcpy(snapshot, t.scene.ops, bytes);
    assert(ui_debug_overlay_append(&t.scene, &view, &overlay));
    assert(ui_scene_is_valid(&t.scene));
    assert(t.scene.n_ops > before);
    assert(!memcmp(t.scene.ops, snapshot, bytes));
    free(snapshot);

    for (int i = 0; i < 5000; i++)
        ui_scene_rect(&t.scene, R(0, 0, 1, 1), 0);
    assert(ui_debug_overlay_append(&t.scene, &view, &overlay));
    assert(ui_scene_is_valid(&t.scene));
}

static void json_describes_the_tree_test(void)
{
    struct probe root, child;
    probe_init(&root, R(0, 0, 100, 100), 0xFF000000);
    probe_init(&child, R(10, 10, 20, 20), 0xFF00FF00);
    ui_widget_set_style_identity(&child.widget, "chip", NULL, NULL);
    assert(ui_widget_add_child(&root.widget, &child.widget));
    static struct traced t;
    struct ui_debug_view view = build(&t, &root.widget);

    struct json_writer j = {0};
    ui_debug_write_tree(&j, &view, NULL, -1);
    char *tree = json_take(&j, NULL);
    assert(tree);
    assert(strstr(tree, "\"path\":\"/0\""));
    assert(strstr(tree, "\"class\":\"chip\""));
    assert(strstr(tree, "\"bounds\":[10,10,20,20]"));
    free(tree);

    ui_debug_write_hit(&j, &view, (struct vector2i){ .x = 15, .y = 15 });
    char *hit = json_take(&j, NULL);
    assert(hit && strstr(hit, "\"target\":{\"type\":\"probe\",\"path\":\"/0\""));
    free(hit);
}

static void json_writer_escapes_and_nests_test(void)
{
    struct json_writer j = {0};
    json_obj_begin(&j);
    json_key(&j, "s"); json_str(&j, "a\"b\\c\n\x01");
    json_key(&j, "a");
    json_arr_begin(&j);
    json_int(&j, -3);
    json_bool(&j, true);
    json_null(&j);
    json_arr_end(&j);
    json_obj_end(&j);
    size_t len;
    char *s = json_take(&j, &len);
    assert(s && !strcmp(s, "{\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"a\":[-3,true,null]}"));
    assert(len == strlen(s));
    free(s);

    json_arr_begin(&j);
    assert(!json_take(&j, NULL));
}

int main(void)
{
    selectors_resolve_test();
    widget_at_matches_what_is_visible_test();
    ops_are_attributed_to_their_widget_test();
    lint_flags_off_center_text_test();
    overlay_never_breaks_the_scene_test();
    json_describes_the_tree_test();
    json_writer_escapes_and_nests_test();
    puts("PASS: ui debug tests");
    return 0;
}
