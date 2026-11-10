#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/damage.h"
#include "ui/internal/tree.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "paint/scene.h"

static struct rect2i r_at(int x, int y, int w, int h)
{
    return (struct rect2i){
        .pos = { .x = x, .y = y }, .size = { .x = w, .y = h } };
}

static void scene_builder_bounds_test(void)
{
    struct ui_scene s = {0};
    ui_scene_init(&s);
    assert(s.n_ops == 0 && !s.overflow);

    assert(ui_scene_push_clip(&s, (struct rect2i){ .size = { .x = 10, .y = 10 } }));
    assert(ui_scene_rect(&s, (struct rect2i){ .pos = { .x = 1, .y = 1 }, .size = { .x = 4, .y = 4 } },
                         0xFF00FF00));
    assert(ui_scene_pop_clip(&s));
    assert(s.n_ops == 3 && !s.overflow);

    assert(!ui_scene_pop_clip(&s));
    assert(s.overflow);

    ui_scene_init(&s);
    for (int i = 0; i < 20000; i++)
        assert(ui_scene_rect(&s, (struct rect2i){ .size = { .x = 1, .y = 1 } }, 0));
    assert(s.n_ops == 20000 && ui_scene_is_valid(&s));

    ui_scene_init(&s);
    for (int i = 0; i < 100; i++)
        assert(ui_scene_push_clip(&s, (struct rect2i){ .size = { .x = 9, .y = 9 } }));
    for (int i = 0; i < 100; i++)
        assert(ui_scene_pop_clip(&s));
    assert(ui_scene_is_valid(&s));

    ui_scene_init(&s);
    assert(ui_scene_text_run(&s, (struct rect2i){ .size = { .x = 8, .y = 16 } },
        (struct ui_text_style){ .advance = 8, .cell_h = 16 }, 0xFFFFFFFF, "ab", 2));
    assert(s.ops[0].kind == UI_SCENE_OP_TEXT && s.ops[0].text_len == 2);
    const char *t = ui_scene_op_text(&s, &s.ops[0]);
    assert(t && !memcmp(t, "ab", 2));

    ui_scene_init(&s);
    static char text[100000];
    memset(text, 'x', sizeof(text));
    assert(ui_scene_text_run(&s, (struct rect2i){ .size = { .x = 1, .y = 1 } },
        (struct ui_text_style){ .advance = 1, .cell_h = 1 }, 0, text, sizeof(text)));
    assert(ui_scene_text_run(&s, (struct rect2i){ .size = { .x = 1, .y = 1 } },
        (struct ui_text_style){ .advance = 1, .cell_h = 1 }, 0, "y", 1));
    assert(s.text_len == sizeof(text) + 1 && ui_scene_is_valid(&s));
    assert(*ui_scene_op_text(&s, &s.ops[1]) == 'y');

    ui_scene_init(&s);
    assert(ui_scene_push_clip(&s, (struct rect2i){ .size = { .x = 2, .y = 2 } }));
    assert(!ui_scene_is_valid(&s));
    ui_scene_destroy(&s);
}

static void scene_node_origin_test(void)
{
    static int owner;
    struct ui_scene s = {0};
    ui_scene_init(&s);
    assert(ui_scene_begin_node(&s, &owner, (struct vector2i){ .x = 100, .y = 50 }));
    assert(ui_scene_rect(&s, r_at(1, 2, 3, 4), 0xFF000000));
    assert(ui_scene_end_scope(&s));
    assert(ui_scene_rect(&s, r_at(1, 2, 3, 4), 0xFF000000));
    assert(ui_scene_is_valid(&s));
    assert(s.ops[0].rect.x == 101 && s.ops[0].rect.y == 52);
    assert(s.ops[1].rect.x == 1 && s.ops[1].rect.y == 2);

    ui_scene_init(&s);
    assert(ui_scene_begin_node(&s, &owner, (struct vector2i){0}));
    assert(!ui_scene_is_valid(&s));
    assert(ui_scene_push_clip(&s, r_at(0, 0, 4, 4)));
    assert(!ui_scene_end_scope(&s));
    assert(s.overflow);
    ui_scene_destroy(&s);
}

static bool damage_covers(const struct ui_damage *d, struct rect2i r)
{
    if (d->all)
        return true;
    for (int i = 0; i < d->n; i++) {
        struct rect2i dr = d->rects[i];
        if (dr.x <= r.x && dr.y <= r.y &&
            dr.x + dr.width >= r.x + r.width &&
            dr.y + dr.height >= r.y + r.height)
            return true;
    }
    return false;
}

static bool damage_touches(const struct ui_damage *d, struct rect2i r)
{
    if (d->all)
        return true;
    for (int i = 0; i < d->n; i++)
        if (!rect2i_is_empty(rect2i_intersect(d->rects[i], r)))
            return true;
    return false;
}

static void emit_two(struct ui_scene *s, struct rect2i ra, struct rect2i rb,
                     bool with_b)
{
    static int a, b;
    ui_scene_init(s);
    ui_scene_begin_node(s, &a, (struct vector2i){0});
    ui_scene_rect(s, ra, 0xFF112233);
    ui_scene_end_scope(s);
    if (with_b) {
        ui_scene_begin_node(s, &b, (struct vector2i){0});
        ui_scene_rect(s, rb, 0xFF445566);
        ui_scene_end_scope(s);
    }
    assert(ui_scene_is_valid(s));
}

static void scene_damage_test(void)
{
    const struct rect2i ra = r_at(0, 0, 20, 10);
    const struct rect2i rb = r_at(40, 30, 10, 5);
    const struct rect2i rb2 = r_at(44, 34, 10, 5);
    static struct ui_scene s;
    static struct ui_damage_tracker t;
    struct ui_damage d;

    emit_two(&s, ra, rb, true);
    ui_damage_track(&t, &s, &d);
    assert(d.all);

    emit_two(&s, ra, rb, true);
    ui_damage_track(&t, &s, &d);
    assert(ui_damage_is_empty(&d));

    emit_two(&s, ra, rb2, true);
    ui_damage_track(&t, &s, &d);
    assert(damage_covers(&d, rb) && damage_covers(&d, rb2));
    assert(!damage_touches(&d, ra));

    emit_two(&s, ra, rb2, false);
    ui_damage_track(&t, &s, &d);
    assert(damage_covers(&d, rb2));
    assert(!damage_touches(&d, ra));

    ui_damage_tracker_reset(&t);
    ui_damage_track(&t, &s, &d);
    assert(d.all);
}

static void emit_tabs(struct ui_scene *s, const char *second)
{
    static int bar;
    const struct ui_text_style style = { .advance = 8, .cell_h = 16 };
    const char *titles[] = { "one", second, "three" };
    ui_scene_init(s);
    ui_scene_begin_node(s, &bar, (struct vector2i){ .x = 0, .y = 0 });
    ui_scene_rect(s, r_at(0, 0, 600, 30), 0xFF101010);
    for (int i = 0; i < 3; i++) {
        ui_scene_begin_part(s, (uint64_t)i);
        ui_scene_rect(s, r_at(i * 200, 4, 190, 22), 0xFF202020);
        ui_scene_text_run(s, r_at(i * 200 + 16, 7, 150, 16), style,
                          0xFFFFFFFF, titles[i], strlen(titles[i]));
        ui_scene_end_scope(s);
    }
    ui_scene_end_scope(s);
    assert(ui_scene_is_valid(s));
}

static void scene_part_damage_test(void)
{
    static struct ui_scene s;
    static struct ui_damage_tracker t;
    struct ui_damage d;
    emit_tabs(&s, "two");
    ui_damage_track(&t, &s, &d);
    emit_tabs(&s, "a much longer title");
    ui_damage_track(&t, &s, &d);
    assert(!d.all && d.n > 0);
    assert(damage_covers(&d, r_at(200, 4, 190, 22)));
    assert(!damage_touches(&d, r_at(0, 0, 190, 30)));
    assert(!damage_touches(&d, r_at(400, 0, 200, 30)));
}

static void scene_clip_damage_test(void)
{
    static int outer, inner;
    static struct ui_scene s;
    static struct ui_damage_tracker t;
    struct ui_damage d;
    for (int pass = 0; pass < 2; pass++) {
        ui_scene_init(&s);
        ui_scene_begin_node(&s, &outer, (struct vector2i){0});
        ui_scene_push_clip(&s, r_at(0, 0, pass ? 50 : 100, 100));
        ui_scene_begin_node(&s, &inner, (struct vector2i){0});
        ui_scene_rect(&s, r_at(0, 0, 100, 100), 0xFF00FF00);
        ui_scene_end_scope(&s);
        ui_scene_pop_clip(&s);
        ui_scene_end_scope(&s);
        ui_damage_track(&t, &s, &d);
    }
    assert(damage_covers(&d, r_at(50, 0, 50, 100)));
}

/* A scene without scopes builds no key table. The next scene with scopes
   must find no record and must damage every scope. */
static void scene_damage_empty_scene_test(void)
{
    static int owner;
    static struct ui_scene s;
    static struct ui_damage_tracker t;
    struct ui_damage d;

    ui_scene_init(&s);
    assert(ui_scene_is_valid(&s));
    ui_damage_track(&t, &s, &d);
    assert(d.all);

    ui_scene_init(&s);
    assert(ui_scene_begin_node(&s, &owner, (struct vector2i){0}));
    assert(ui_scene_rect(&s, r_at(0, 0, 4, 4), 0xFF101010));
    assert(ui_scene_end_scope(&s));
    ui_damage_track(&t, &s, &d);
    assert(!d.all && damage_covers(&d, r_at(0, 0, 4, 4)));
    ui_damage_tracker_destroy(&t);
}

/* A new part before a node moves the scene position of every later node,
   but not their pixels. The tracker must match those nodes by key. */
static void scene_damage_shifted_scope_test(void)
{
    static int parent, part, later;
    static struct ui_scene s;
    static struct ui_damage_tracker t;
    const struct rect2i later_rect = r_at(20, 0, 10, 10);
    struct ui_damage d;

    for (int pass = 0; pass < 2; pass++) {
        ui_scene_init(&s);
        assert(ui_scene_begin_node(&s, &parent, (struct vector2i){0}));
        if (pass) {
            assert(ui_scene_begin_node(&s, &part, (struct vector2i){0}));
            assert(ui_scene_end_scope(&s));
        }
        assert(ui_scene_end_scope(&s));
        assert(ui_scene_begin_node(&s, &later, (struct vector2i){0}));
        assert(ui_scene_rect(&s, later_rect, 0xFF445566));
        assert(ui_scene_end_scope(&s));
        assert(ui_scene_is_valid(&s));
        ui_damage_track(&t, &s, &d);
        if (pass)
            assert(!d.all && ui_damage_is_empty(&d));
    }
    ui_damage_tracker_destroy(&t);
}

enum { MANY_SCOPES = 300 };

static void emit_row(struct ui_scene *s, int *owners, int n, int omit, int moved)
{
    ui_scene_init(s);
    for (int i = 0; i < n; i++) {
        if (i == omit)
            continue;
        int y = i == moved ? 40 : 0;
        assert(ui_scene_begin_node(s, &owners[i], (struct vector2i){0}));
        assert(ui_scene_rect(s, r_at(i * 4, y, 4, 4), 0xFF101010));
        assert(ui_scene_end_scope(s));
    }
    assert(ui_scene_is_valid(s));
}

/* The tracker indexes its records by key. A scene that outgrows the first
   table size must still match every record, and a node that goes away must
   shift the lookup of every later node. */
static void scene_damage_many_scopes_test(void)
{
    static int owners[MANY_SCOPES];
    static struct ui_scene s;
    static struct ui_damage_tracker t;
    struct ui_damage d;
    for (int i = 0; i < MANY_SCOPES; i++)
        owners[i] = i;

    emit_row(&s, owners, MANY_SCOPES, -1, -1);
    ui_damage_track(&t, &s, &d);
    assert(d.all);

    ui_damage_track(&t, &s, &d);
    assert(ui_damage_is_empty(&d));

    emit_row(&s, owners, MANY_SCOPES, -1, 7);
    ui_damage_track(&t, &s, &d);
    assert(damage_covers(&d, r_at(7 * 4, 0, 4, 4)));
    assert(damage_covers(&d, r_at(7 * 4, 40, 4, 4)));
    assert(!damage_touches(&d, r_at(8 * 4, 0, 4, 4)));

    emit_row(&s, owners, MANY_SCOPES, 10, -1);
    ui_damage_track(&t, &s, &d);
    assert(damage_covers(&d, r_at(10 * 4, 0, 4, 4)));
    assert(damage_covers(&d, r_at((MANY_SCOPES - 1) * 4, 0, 4, 4)));
    assert(!damage_touches(&d, r_at(0, 0, 4, 4)));
    assert(!damage_touches(&d, r_at(9 * 4, 0, 4, 4)));
    ui_damage_tracker_destroy(&t);
}

static void damage_bounds_test(void)
{
    struct ui_damage d = {0};
    struct rect2i b = ui_damage_bounds(&d);
    assert(rect2i_is_empty(b));

    ui_damage_add(&d, r_at(10, 20, 30, 40));
    b = ui_damage_bounds(&d);
    assert(b.x == 10 && b.y == 20 && b.width == 30 && b.height == 40);

    ui_damage_add(&d, r_at(5, 10, 10, 10));
    b = ui_damage_bounds(&d);
    assert(b.x == 5 && b.y == 10 && b.width == 35 && b.height == 50);

    ui_damage_add(&d, r_at(100, 100, 10, 10));
    assert(d.n == 2);
    b = ui_damage_bounds(&d);
    assert(b.x == 5 && b.y == 10 && b.width == 105 && b.height == 100);
}

static void scene_copy_test(void)
{
    const struct ui_text_style style = { .advance = 8, .cell_h = 16 };
    struct ui_scene src = {0}, dst = {0};
    assert(ui_scene_rect(&src, r_at(0, 0, 10, 10), 0xFF112233));
    assert(ui_scene_text_run(&src, r_at(0, 0, 10, 10), style, 0xFFFFFFFF, "test", 4));

    static int owner;
    assert(ui_scene_begin_node(&dst, &owner, (struct vector2i){0}));
    assert(ui_scene_rect(&dst, r_at(5, 5, 20, 20), 0xFF445566));

    assert(ui_scene_copy(&dst, &src));
    assert(dst.n_ops == 2 && ui_scene_is_valid(&dst));
    assert(!memcmp(dst.ops, src.ops, 2 * sizeof(*dst.ops)));
    const char *text = ui_scene_op_text(&dst, &dst.ops[1]);
    assert(text && !memcmp(text, "test", 4));

    ui_scene_init(&src);
    assert(ui_scene_text_run(&src, r_at(0, 0, 10, 10), style, 0xFF000000, "gone", 4));
    assert(dst.n_ops == 2 && dst.ops[0].color == 0xFF112233);
    text = ui_scene_op_text(&dst, &dst.ops[1]);
    assert(text && !memcmp(text, "test", 4));

    assert(ui_scene_copy(&dst, &dst));
    assert(dst.n_ops == 2 && dst.ops[0].color == 0xFF112233);

    ui_scene_destroy(&src);
    ui_scene_destroy(&dst);
}

int main(void)
{
    scene_builder_bounds_test();
    scene_copy_test();
    scene_node_origin_test();
    scene_damage_test();
    damage_bounds_test();
    scene_part_damage_test();
    scene_clip_damage_test();
    scene_damage_empty_scene_test();
    scene_damage_shifted_scope_test();
    scene_damage_many_scopes_test();
    printf("PASS: UI scene tests\n");
    return 0;
}
