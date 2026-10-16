#ifndef WSAP_TEST_SCENE_TEXT_CONTRACT_H
#define WSAP_TEST_SCENE_TEXT_CONTRACT_H

#include "font/font.h"
#include "paint/scene.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCENE_CELL_W 4
#define SCENE_CELL_H 4
#define SCENE_CELLS  4
#define SCENE_WIDTH  (SCENE_CELL_W * SCENE_CELLS)

#define SCENE_RIGHT_CP 'B'

struct scene_backend {
    const char *name;
    void *ctx;
    bool (*paint)(void *ctx, const struct ui_scene *scene);
    bool (*inked)(void *ctx, int x, int y);
};

static uint8_t scene_ink[2][SCENE_CELL_H][SCENE_CELL_W];
static uint8_t *scene_rows[2][SCENE_CELL_H];
static struct glyph scene_glyphs[2];
static bool scene_glyphs_ready;

static void scene_contract_build_glyphs(void)
{
    for (int which = 0; which < 2; which++) {
        int col = which ? SCENE_CELL_W - 1 : 0;
        for (int y = 0; y < SCENE_CELL_H; y++) {
            memset(scene_ink[which][y], 0, SCENE_CELL_W);
            scene_ink[which][y][col] = 0xFF;
            scene_rows[which][y] = scene_ink[which][y];
        }
        scene_glyphs[which] = (struct glyph){
            .width = SCENE_CELL_W,
            .alpha = scene_rows[which],
        };
    }
    scene_glyphs_ready = true;
}

static const struct glyph *scene_contract_glyph(uint32_t cp, uint16_t flags)
{
    (void)flags;
    if (!scene_glyphs_ready)
        scene_contract_build_glyphs();
    return &scene_glyphs[cp == SCENE_RIGHT_CP];
}

static bool scene_cell_shows(const struct scene_backend *b, int cell,
                             uint32_t cp)
{
    int want = (int)(cp == SCENE_RIGHT_CP) ? SCENE_CELL_W - 1 : 0;
    for (int y = 0; y < SCENE_CELL_H; y++)
        for (int x = 0; x < SCENE_CELL_W; x++)
            if (b->inked(b->ctx, cell * SCENE_CELL_W + x, y) != (x == want))
                return false;
    return true;
}

static bool scene_cell_blank(const struct scene_backend *b, int cell)
{
    for (int y = 0; y < SCENE_CELL_H; y++)
        for (int x = 0; x < SCENE_CELL_W; x++)
            if (b->inked(b->ctx, cell * SCENE_CELL_W + x, y))
                return false;
    return true;
}

static void scene_text_run(struct ui_scene *s, const char *text, size_t len)
{
    ui_scene_init(s);
    struct ui_text_style style = {
        .advance = SCENE_CELL_W,
        .cell_h = SCENE_CELL_H,
        .flags = UI_TEXT_PLAIN,
    };
    struct rect2i r = { .size = { .x = SCENE_WIDTH, .y = SCENE_CELL_H } };
    assert(ui_scene_text_run(s, r, style, 0xFFFFFFFFu, text, len));
}

static void scene_text_contract_check(const struct scene_backend *b)
{
    struct ui_scene s = {0};

    scene_text_run(&s, "AB", 2);
    assert(b->paint(b->ctx, &s));
    assert(scene_cell_shows(b, 0, 'A'));
    assert(scene_cell_shows(b, 1, 'B'));
    assert(scene_cell_blank(b, 2));

    scene_text_run(&s, "A\xc3\x97", 3);
    assert(b->paint(b->ctx, &s));
    assert(scene_cell_shows(b, 0, 'A'));
    assert(scene_cell_shows(b, 1, 0x00D7));
    assert(scene_cell_blank(b, 2));

    scene_text_run(&s, "\xe4\xb8\x80" "B", 4);
    assert(b->paint(b->ctx, &s));
    assert(scene_cell_shows(b, 0, 0x4E00));
    assert(scene_cell_blank(b, 1));
    assert(scene_cell_shows(b, 2, 'B'));

    scene_text_run(&s, "\xff\xff", 2);
    assert(b->paint(b->ctx, &s));
    assert(scene_cell_blank(b, 2));
    assert(scene_cell_blank(b, 3));

    ui_scene_init(&s);
    assert(b->paint(b->ctx, &s));
    for (int cell = 0; cell < SCENE_CELLS; cell++)
        assert(scene_cell_blank(b, cell));
    ui_scene_destroy(&s);
}

#endif
