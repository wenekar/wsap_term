/*
 * Tests of the scene part of the GPU frame build, with the real font.
 *
 * gpu_build_frame() calls no GPU API, so the tests run with no display and
 * no device. They check the frame data that a backend reads.
 *
 * Two builder rules have regression coverage:
 * - Text, shadow, and image ops build. The builder must not read the `geom`
 *   field of an op kind that has none: text offsets, the blur radius, and the
 *   image share that field.
 * - UI text starts at the top of its op box. Row 0 of a cell raster is the
 *   cell top, so the builder must not add the row offset a second time.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "font/font.h"
#include "paint/scene.h"
#include "render/gpu/build.h"
#include "render/render.h"

enum { CANVAS_W = 200, CANVAS_H = 100 };

/* The glyph instance at index `i` of the UI stream, or NULL. */
static const struct gpu_glyph_inst *ui_glyph(const struct gpu_build *b, uint32_t i)
{
    const struct gpu_stream *s = &b->streams[GPU_ST_UI];
    if (i >= s->n)
        return NULL;
    return (const struct gpu_glyph_inst *)(const void *)(s->data +
                                                          (size_t)i * sizeof(*s));
}

static void text_scene(struct ui_scene *s, struct rect2i box, const char *text)
{
    const struct font_metrics *m = font_instance_metrics(font_instance_default());
    ui_scene_init(s);
    assert(ui_scene_text_run(s, box,
                             (struct ui_text_style){
                                 .advance = m->cell_w,
                                 .cell_h = m->cell_h,
                             },
                             0xFFFFFFFFu, text, strlen(text)));
}

/* A scene with text, a shadow, and an image builds. */
static void test_scene_kinds_build(void)
{
    struct ui_scene scene = {0};
    text_scene(&scene, (struct rect2i){ .pos = { .x = 8, .y = 6 },
                                        .size = { .x = 40, .y = 12 } }, "Ag");
    assert(ui_scene_shadow(&scene, (struct rect2i){ .pos = { .x = 60, .y = 20 },
                                                   .size = { .x = 40, .y = 30 } },
                           6, 10, 0x80000000u));
    static uint32_t texels[4 * 4];
    struct ui_image image = { .pixels = texels, .size = { .x = 4, .y = 4 }, .gen = 1 };
    assert(ui_scene_image(&scene, (struct vector2i){ .x = 120, .y = 10 }, &image));

    struct gpu_build b = {0};
    assert(gpu_build_init(&b));
    struct renderer_frame frame = {
        .ui = &scene,
        .ui_font = font_instance_default(),
    };
    assert(gpu_build_frame(&b, &frame,
                           (struct vector2i){ .x = CANVAS_W, .y = CANVAS_H }));

    /* The shadow made one shape, the image made one slot, the text made quads. */
    assert(b.streams[GPU_ST_SHAPE].n > 0);
    assert(b.n_images == 1);
    assert(b.streams[GPU_ST_UI].n > 0);
    assert(b.n_cmds > 0);
    gpu_build_commit(&b);
    gpu_build_release(&b);
    ui_scene_destroy(&scene);
    puts("PASS: a GPU frame with text, a shadow, and an image builds");
}

/* A float rect has no GPU instance path, so the build fails on purpose and
 * the CPU painter draws that frame. */
static void test_float_rect_still_fails(void)
{
    struct ui_scene scene = {0};
    ui_scene_init(&scene);
    assert(ui_scene_rect_f(&scene, (struct rect2f){
        .pos = { .x = 4.0f, .y = 4.0f }, .size = { .x = 10.0f, .y = 10.0f } }, NULL));

    struct gpu_build b = {0};
    assert(gpu_build_init(&b));
    struct renderer_frame frame = {
        .ui = &scene,
        .ui_font = font_instance_default(),
    };
    assert(!gpu_build_frame(&b, &frame,
                            (struct vector2i){ .x = CANVAS_W, .y = CANVAS_H }));
    gpu_build_release(&b);
    ui_scene_destroy(&scene);
    puts("PASS: a float rect still fails the GPU build");
}

/*
 * The UI text of the frame starts at the top of its op box. The glyph
 * raster holds its own ink rows, so the builder adds no row offset.
 */
static void test_text_rows(void)
{
    const struct rect2i box = { .pos = { .x = 4, .y = 9 }, .size = { .x = 40, .y = 16 } };
    struct ui_scene scene = {0};
    text_scene(&scene, box, "H");
    const struct glyph *g = glyph_cache_fetch(font_instance_default(), 'H', 0);
    assert(g);
    /* Some backends keep raster row 0 at the cell top and write no row
     * offset. Such a glyph cannot tell the two rules apart, and the check
     * below still holds for it. */
    const bool row_offset = g->y_offset > 0;

    struct gpu_build b = {0};
    assert(gpu_build_init(&b));
    struct renderer_frame frame = {
        .ui = &scene,
        .ui_font = font_instance_default(),
    };
    assert(gpu_build_frame(&b, &frame,
                           (struct vector2i){ .x = CANVAS_W, .y = CANVAS_H }));
    const struct gpu_glyph_inst *in = ui_glyph(&b, 0);
    assert(in);
    assert(in->rect[1] == (float)box.y);
    /* The builder must not add the row offset a second time. */
    if (row_offset)
        assert(in->rect[1] != (float)(box.y + g->y_offset));
    assert(in->rect[3] > 0.0f);
    gpu_build_commit(&b);
    gpu_build_release(&b);
    ui_scene_destroy(&scene);
    puts("PASS: UI text starts at the top of its op box");
}

int main(void)
{
    assert(render_init());
    assert(font_instance_default());
    test_scene_kinds_build();
    test_float_rect_still_fails();
    test_text_rows();
    render_shutdown();
    return 0;
}
