#include "scene_text_contract.h"

#include "font/font.h"
#include "render/scene_paint.h"

extern struct font_metrics scene_paint_test_font;
#define TEST_FONT ((struct font_instance *)&scene_paint_test_font)

#include <assert.h>
#include <stdio.h>
#include <string.h>

extern const struct glyph *(*scene_paint_glyph_hook)(uint32_t cp, uint16_t flags);

static uint32_t target[SCENE_CELL_H][SCENE_WIDTH];

static bool sw_paint(void *ctx, const struct ui_scene *scene)
{
    (void)ctx;
    memset(target, 0, sizeof(target));
    return ui_scene_paint(TEST_FONT, &target[0][0], SCENE_WIDTH, SCENE_CELL_H, scene);
}

static bool sw_inked(void *ctx, int x, int y)
{
    (void)ctx;
    return (target[y][x] & 0x00FFFFFFu) != 0;
}

static void paint_clip_test(void)
{
    enum { W = 16, H = 8 };
    struct ui_scene s = {0};
    ui_scene_init(&s);
    assert(ui_scene_rect(&s, (struct rect2i){ .size = { .x = W, .y = H } },
                         0xFF112233));
    assert(ui_scene_push_clip(&s, (struct rect2i){
        .pos = { .x = 4, .y = 2 }, .size = { .x = 4, .y = 4 } }));
    assert(ui_scene_rect(&s, (struct rect2i){
        .pos = { .x = 2, .y = 0 }, .size = { .x = 8, .y = 8 } }, 0xFF445566));
    assert(ui_scene_pop_clip(&s));

    uint32_t full[W * H], clipped[W * H];
    memset(full, 0xAA, sizeof(full));
    memset(clipped, 0xAA, sizeof(clipped));
    assert(ui_scene_paint(TEST_FONT, full, W, H, &s));
    struct rect2i clip = {
        .pos = { .x = 6, .y = 1 }, .size = { .x = 5, .y = 5 } };
    assert(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s, clip));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bool in = x >= clip.x && x < clip.x + clip.width &&
                      y >= clip.y && y < clip.y + clip.height;
            assert(clipped[y * W + x] ==
                   (in ? full[y * W + x] : 0xAAAAAAAAu));
        }

    memset(clipped, 0xAA, sizeof(clipped));
    assert(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s, (struct rect2i){0}));
    for (int i = 0; i < W * H; i++)
        assert(clipped[i] == 0xAAAAAAAAu);

    memset(clipped, 0xAA, sizeof(clipped));
    assert(ui_scene_paint_clipped(TEST_FONT, clipped, W, H, &s,
        (struct rect2i){ .size = { .x = W, .y = H } }));
    assert(!memcmp(clipped, full, sizeof(full)));
    ui_scene_destroy(&s);
}

int main(void)
{
    scene_paint_glyph_hook = scene_contract_glyph;
    paint_clip_test();
    struct scene_backend software = {
        .name = "ui_scene_paint",
        .paint = sw_paint,
        .inked = sw_inked,
    };
    scene_text_contract_check(&software);
    puts("PASS: scene text contract (software painter)");
    return 0;
}
