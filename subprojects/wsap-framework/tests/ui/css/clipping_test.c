#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "paint/scene.h"
#include "font/font.h"
#include "render/scene_paint.h"

extern struct font_metrics scene_paint_test_font;
#define TEST_FONT ((struct font_instance *)&scene_paint_test_font)

static void test_scene_failure_is_not_painted(void)
{
    uint32_t pixels[4] = {0xFF010203, 0xFF010203, 0xFF010203, 0xFF010203};
    struct ui_scene s = {0};
    ui_scene_init(&s);
    assert(ui_scene_push_clip(&s, (struct rect2i){ .size = { .x = 2, .y = 2 } }));
    assert(!ui_scene_is_valid(&s));
    assert(!ui_scene_paint(TEST_FONT, pixels, 2, 2, &s));
    for (size_t i = 0; i < 4; i++) assert(pixels[i] == 0xFF010203);
    ui_scene_destroy(&s);
}

static void test_rounded_rect_clipping_preserves_shape(void)
{
    uint32_t clipped[10 * 10] = {0};
    uint32_t reference[20 * 20] = {0};
    struct ui_scene s = {0};
    ui_scene_init(&s);
    ui_scene_rounded_rect(&s, (struct rect2i){ .pos = { .x = -3, .y = -3 },
                                               .size = { .x = 10, .y = 10 } },
                          4, 0xFFFFFFFF);
    ui_scene_paint(TEST_FONT, clipped, 10, 10, &s);
    ui_scene_init(&s);
    ui_scene_rounded_rect(&s, (struct rect2i){ .pos = { .x = 2, .y = 2 },
                                               .size = { .x = 10, .y = 10 } },
                          4, 0xFFFFFFFF);
    ui_scene_paint(TEST_FONT, reference, 20, 20, &s);
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 7; x++)
            assert(clipped[y * 10 + x] == reference[(y + 5) * 20 + x + 5]);
    ui_scene_destroy(&s);
}

static void test_node_draws_in_its_frame_under_the_clip(void)
{
    static int node;
    struct ui_scene s = {0};
    ui_scene_init(&s);
    struct rect2i all = { .size = { .x = 8, .y = 8 } };
    struct rect2i clip = { .pos = { .x = 2, .y = 2 }, .size = { .x = 2, .y = 2 } };
    assert(ui_scene_rect(&s, all, 0xFF000000));
    assert(ui_scene_push_clip(&s, clip));
    assert(ui_scene_begin_node(&s, &node, clip.pos));
    assert(ui_scene_rect(&s, (struct rect2i){ .size = { .x = 4, .y = 4 } },
                         0xFFFF0000));
    assert(ui_scene_end_scope(&s));
    assert(ui_scene_pop_clip(&s));
    uint32_t pixels[64] = {0};
    assert(ui_scene_paint(TEST_FONT, pixels, 8, 8, &s));
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            assert(pixels[y * 8 + x] ==
                   (x >= 2 && x < 4 && y >= 2 && y < 4 ? 0xFFFF0000 : 0xFF000000));
    ui_scene_destroy(&s);
}

int main(void)
{
    test_scene_failure_is_not_painted();
    test_rounded_rect_clipping_preserves_shape();
    test_node_draws_in_its_frame_under_the_clip();
    printf("PASS: CSS clipping tests\n");
    return 0;
}
