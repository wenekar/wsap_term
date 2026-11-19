#include <assert.h>
#include <stdint.h>

#include "paint/scene.h"
#include "render/plan.h"

/*
 * The planner ink includes every op that draws. An image op has no tint
 * colour, so the colour test alone misses it. A removed image then stays on a
 * retained canvas.
 */
static void image_op_makes_ink_test(void)
{
    static const uint32_t pixel = 0xFFFFFFFFu;
    struct ui_image image = {
        .pixels = &pixel,
        .size = { .x = 1, .y = 1 },
    };
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 12, .y = 10 },
                          &image));

    struct plan_span ink[4] = {0};
    struct rect2i grid = {
        .pos = { .x = 0, .y = 0 },
        .size = { .x = 40, .y = 40 },
    };
    plan_ui_ink(&scene, grid, 10, 3, ink);

    /* The image covers row 1, columns 12 and 13. It is the only ink. */
    assert(ink[1].x0 == 12 && ink[1].x1 == 13);
    assert(plan_span_empty(ink[0]));
    assert(plan_span_empty(ink[2]));
    assert(plan_span_empty(ink[3]));
    ui_scene_destroy(&scene);
}

int main(void)
{
    image_op_makes_ink_test();
    return 0;
}
