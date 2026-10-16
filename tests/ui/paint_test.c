#include <assert.h>
#include <stdint.h>

#include "ui/damage.h"
#include "ui/internal/tree.h"
#include "ui/paint.h"
#include "ui/widgets/label.h"
#include "ui/widget.h"

struct paint_probe {
    uint32_t color;
    bool clip;
    bool unbalanced;
};

static void paint_probe_widget(const struct ui_widget *widget,
                               struct ui_paint_context *paint)
{
    const struct paint_probe *probe = widget->userdata;
    struct rect2i r = {
        .pos = { .x = 1, .y = 2 },
        .size = { .x = 3, .y = 4 },
    };
    if (probe->clip) {
        assert(ui_paint_push_clip(paint, (struct rect2i){
            .pos = { .x = 0, .y = 0 }, .size = paint->size,
        }));
    }
    assert(ui_paint_rect(paint, r, probe->color));
    if (probe->clip && !probe->unbalanced)
        assert(ui_paint_pop_clip(paint));
}

static const struct ui_widget_ops PAINT_OPS = {
    .paint = paint_probe_widget,
};

static void paint_order_translation_test(void)
{
    struct paint_probe parent_probe = { .color = 0xff000001 };
    struct paint_probe child_probe = { .color = 0xff000002 };
    struct ui_widget parent = {0};
    struct ui_widget child = {0};
    ui_widget_init(&parent, &PAINT_OPS, &parent_probe);
    ui_widget_init(&child, &PAINT_OPS, &child_probe);
    assert(ui_widget_add_child(&parent, &child));
    ui_widget_layout(&parent, (struct rect2i){
        .pos = { .x = 10, .y = 20 }, .size = { .x = 80, .y = 60 },
    });
    child.bounds = (struct rect2i){
        .pos = { .x = 30, .y = 50 }, .size = { .x = 20, .y = 10 },
    };

    static struct ui_scene scene;
    assert(ui_widget_build_scene(&parent, &scene));
    assert(scene.n_ops == 2);
    assert(scene.ops[0].kind == UI_SCENE_OP_RECT);
    assert(scene.ops[0].rect.x == 11 && scene.ops[0].rect.y == 22);
    assert(scene.ops[0].color == parent_probe.color);
    assert(scene.ops[1].rect.x == 31 && scene.ops[1].rect.y == 52);
    assert(scene.ops[1].color == child_probe.color);
    assert(!((&parent)->impl.invalidation & UI_WIDGET_INVALIDATE_PAINT));
    assert(!((&child)->impl.invalidation & UI_WIDGET_INVALIDATE_PAINT));

    ui_widget_destroy(&parent);
}

static void clipping_and_zero_test(void)
{
    static struct ui_scene scene;
    assert(ui_widget_build_scene(NULL, &scene));
    assert(scene.n_ops == 0);

    struct paint_probe probe = { .color = 1, .clip = true };
    struct ui_widget widget = {0};
    ui_widget_init(&widget, &PAINT_OPS, &probe);
    ui_widget_layout(&widget, (struct rect2i){
        .pos = { .x = 4, .y = 5 }, .size = { .x = 8, .y = 9 },
    });
    assert(ui_widget_build_scene(&widget, &scene));
    assert(scene.n_ops == 3);
    assert(scene.ops[0].kind == UI_SCENE_OP_PUSH_CLIP);
    assert(scene.ops[0].rect.x == 4 && scene.ops[0].rect.y == 5);
    assert(scene.ops[1].kind == UI_SCENE_OP_RECT);
    assert(scene.ops[2].kind == UI_SCENE_OP_POP_CLIP);

    probe.unbalanced = true;
    ui_widget_invalidate(&widget, UI_WIDGET_INVALIDATE_PAINT);
    assert(!ui_widget_build_scene(&widget, &scene));
    assert((&widget)->impl.invalidation & UI_WIDGET_INVALIDATE_PAINT);
    ui_widget_destroy(&widget);
}

static bool damage_covers(const struct ui_damage *damage, struct rect2i r)
{
    for (int i = 0; i < damage->n; i++) {
        struct rect2i d = damage->rects[i];
        if (d.x <= r.x && d.y <= r.y &&
            d.x + d.width >= r.x + r.width &&
            d.y + d.height >= r.y + r.height)
            return true;
    }
    return false;
}

static bool damage_within(const struct ui_damage *damage, struct rect2i r)
{
    if (damage->all)
        return false;
    for (int i = 0; i < damage->n; i++) {
        struct rect2i d = damage->rects[i];
        if (d.x < r.x || d.y < r.y ||
            d.x + d.width > r.x + r.width ||
            d.y + d.height > r.y + r.height)
            return false;
    }
    return true;
}

static struct ui_damage track(struct ui_damage_tracker *t,
                              const struct ui_widget *root)
{
    static struct ui_scene scene;
    assert(ui_widget_build_scene(root, &scene));
    struct ui_damage d;
    ui_damage_track(t, &scene, &d);
    return d;
}

static struct rect2i label_text(const struct ui_widget *widget, struct rect2i *ink)
{
    struct ui_scene scene = {0};
    assert(ui_widget_build_scene(widget, &scene));
    struct rect2i box = {0};
    for (int i = 0; i < scene.n_ops; i++)
        if (scene.ops[i].kind == UI_SCENE_OP_TEXT) {
            box = ui_scene_text_box(&scene, &scene.ops[i]);
            if (ink) *ink = ui_scene_text_ink(&scene, &scene.ops[i]);
            break;
        }
    assert(!rect2i_is_empty(box));
    ui_scene_destroy(&scene);
    return box;
}

static void label_text_damage_test(void)
{
    struct ui_label label = {0};
    ui_label_init(&label);
    ui_widget_layout(&label.widget, (struct rect2i){
        .pos = { .x = 20, .y = 12 }, .size = { .x = 400, .y = 20 },
    });
    static struct ui_damage_tracker t;
    track(&t, &label.widget);
    ui_label_set_text(&label, "A long label");
    struct rect2i near;
    struct rect2i long_text = label_text(&label.widget, &near);
    struct ui_damage d = track(&t, &label.widget);
    assert(damage_covers(&d, long_text));
    assert(damage_within(&d, near));

    ui_label_set_text(&label, "x");
    struct rect2i short_text = label_text(&label.widget, NULL);
    d = track(&t, &label.widget);
    assert(damage_covers(&d, long_text));
    assert(damage_within(&d, near));

    ui_label_set_text(&label, "");
    d = track(&t, &label.widget);
    assert(damage_covers(&d, short_text));

    d = track(&t, &label.widget);
    assert(ui_damage_is_empty(&d));
    ui_widget_destroy(&label.widget);
}

static void moved_widget_damage_test(void)
{
    struct paint_probe probe = { .color = 0xff000003 };
    struct ui_widget widget = {0};
    ui_widget_init(&widget, &PAINT_OPS, &probe);
    ui_widget_layout(&widget, (struct rect2i){
        .pos = { .x = 1, .y = 2 }, .size = { .x = 10, .y = 20 },
    });
    static struct ui_damage_tracker t;
    track(&t, &widget);
    struct ui_damage d = track(&t, &widget);
    assert(ui_damage_is_empty(&d));

    ui_widget_layout(&widget, (struct rect2i){
        .pos = { .x = 50, .y = 60 }, .size = { .x = 10, .y = 20 },
    });
    d = track(&t, &widget);
    assert(damage_covers(&d, (struct rect2i){
        .pos = { .x = 2, .y = 4 }, .size = { .x = 3, .y = 4 } }));
    assert(damage_covers(&d, (struct rect2i){
        .pos = { .x = 51, .y = 62 }, .size = { .x = 3, .y = 4 } }));
    ui_widget_destroy(&widget);
}

int main(void)
{
    paint_order_translation_test();
    clipping_and_zero_test();
    label_text_damage_test();
    moved_widget_damage_test();
    return 0;
}
