#ifndef RENDER_GPU_BUILD_H
#define RENDER_GPU_BUILD_H

/*
 * The CPU half of a GPU backend. Every backend shares it.
 *
 * The frame builder turns one render_frame into data that a GPU can draw:
 *
 *   1. the grid area in default_bg, and the cell backgrounds that differ;
 *   2. the glyphs of the grid, clipped to the cell area;
 *   3. the decorations and cursor marks of the grid;
 *   4. the UI scene ops, in scene order, each with its own scissor.
 *
 * The result is instance streams, draw commands, glyph pixels for the
 * atlas, and the UI images of the frame. The builder calls no GPU API. The
 * backend reads the result, moves it to the GPU, and draws the commands in
 * order.
 *
 * The atlas. Glyph pixels live in one square texture of `atlas.size` texels.
 * A glyph enters the atlas once, under its glyph id and font version
 * (render/cell.h). The builder decides where each glyph goes, and it
 * stages the pixels in `atlas.stage`. The backend copies each
 * `atlas.uploads` rect into its texture before it draws. The builder also
 * decides the size: when the atlas is full, it forgets every glyph and
 * doubles `atlas.size`, up to the maximum. The backend compares
 * `atlas.size` with the size of its texture before each frame, and it makes
 * a new texture when they differ.
 *
 * Images. A UI image has a slot in `images`. The backend keeps one texture
 * for each slot of `n_images`. A slot with `frame == build.frame` is drawn
 * by this frame. When that slot is also `dirty`, the backend must load
 * `pixels` into the texture of the slot before it draws. `pixels` is valid
 * only in that case. A frame with more images than slots grows the table,
 * so a backend must read both `images` and `n_images` in every frame.
 *
 * The frame is all or nothing.
 * - gpu_build_frame() false: the CPU paints the frame. The builder state
 *   is consistent, and the backend calls nothing more.
 * - gpu_build_frame() true: the backend calls gpu_build_commit() after the
 *   GPU took the frame, or gpu_build_abort() when it did not. An abort
 *   forgets the atlas, because its map holds glyphs that were staged and
 *   never uploaded.
 *
 * Threads: main thread only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/grid.h"
#include "util/geom.h"

struct damage_list;
struct decor_cache;
struct framebuffer;
struct render_frame;

/* The side of the atlas texture, in texels: the first size and the largest
 * that a doubled atlas reaches. */
#define GPU_ATLAS_START_SIZE 512
#define GPU_ATLAS_MAX_SIZE 4096
/* The most shelves of the atlas packer. A frame that needs more shelves sets
 * atlas.full. */
#define GPU_SHELVES_MAX 1024
/* Each stream starts at a multiple of this in the instance buffer. */
#define GPU_STREAM_ALIGN 256

/*
 * The blend modes of an instance. They are enum ui_scene_blend of
 * paint/scene.h. The shaders and the blend state of each backend use these
 * numbers. An instance that starts a command decides the blend state of the
 * command, so a command never mixes two modes.
 */
enum gpu_blend {
    GPU_BLEND_PREMUL = 0,
    GPU_BLEND_ALPHA = 1,
    GPU_BLEND_ADDITIVE = 2,
    GPU_BLEND_MULTIPLY = 3,
    GPU_BLEND_NONE = 4,
    GPU_BLEND_COUNT = 5,
};

/* The instance modes. The shaders of each backend use the same numbers. */
enum gpu_glyph_mode {
    GPU_MODE_SOLID = 0,
    GPU_MODE_ALPHA = 1,
    GPU_MODE_COLOR = 2,
    GPU_MODE_IMAGE = 3,
    GPU_MODE_SUBPX = 4,
    /* An image that samples four texels. */
    GPU_MODE_IMAGE_LINEAR = 5,
};

/* The shape modes. The shaders of each backend use the same numbers, and 0
 * is not a mode: a shader that reads any other value draws a shadow. */
enum gpu_shape_mode {
    GPU_SHAPE_ROUNDED = 1,
    GPU_SHAPE_CIRCLE = 2,
    GPU_SHAPE_SHADOW = 3,
};

/*
 * One textured or solid quad. `rect` is x, y, width, height in pixels.
 * `uv` is the texel rect x0, y0, x1, y1. `color` is a straight color, and
 * its alpha is the opacity. An image mode multiplies its premultiplied
 * texels by `color`. `blend` is one of enum gpu_blend.
 */
struct gpu_glyph_inst {
    float rect[4];
    float uv[4];
    float color[4];
    uint32_t mode;
    uint32_t blend;
    uint32_t pad[2];
};

/*
 * One shape. `rect` is the shape and the quad. `blur` is in pixels.
 * `blend` is one of enum gpu_blend.
 */
struct gpu_shape_inst {
    float rect[4];
    float color[4];
    float radius;
    uint32_t mode;
    uint32_t blur;
    uint32_t blend;
};

_Static_assert(sizeof(struct gpu_glyph_inst) == 64, "the shaders read 64 byte glyph instances");
_Static_assert(sizeof(struct gpu_shape_inst) == 48, "the shaders read 48 byte shape instances");

/*
 * An instance stream is a list of instances of one type. The grid fills the
 * first four. The UI fills the last two, in the order of the scene.
 */
enum gpu_stream_id : uint8_t {
    GPU_ST_BG,
    GPU_ST_INK,
    GPU_ST_SUBPX,
    GPU_ST_DECO,
    GPU_ST_UI,
    GPU_ST_SHAPE,
    GPU_ST_COUNT,
};

struct gpu_stream {
    uint8_t *data;
    uint32_t n;
    uint32_t cap;
};

/* The bytes of one instance of a stream. */
static inline size_t gpu_stream_stride(enum gpu_stream_id id)
{
    return id == GPU_ST_SHAPE ? sizeof(struct gpu_shape_inst) : sizeof(struct gpu_glyph_inst);
}

enum gpu_cmd_kind : uint8_t {
    GPU_CMD_GLYPH,
    /* Subpixel glyphs. They draw in three passes, one for each color channel. */
    GPU_CMD_SUBPX,
    GPU_CMD_SHAPE,
};

/*
 * One draw: `count` instances of `stream` from `start`, inside `scissor`.
 * `image` 0 samples the atlas. `image` n samples the texture of
 * images[n - 1].
 */
struct gpu_cmd {
    struct rect2i scissor;
    uint32_t start;
    uint32_t count;
    enum gpu_cmd_kind kind;
    enum gpu_stream_id stream;
    int16_t image;
};

_Static_assert(sizeof(struct gpu_cmd) == 28, "struct gpu_cmd packs to 28 bytes");

/* A staged rect: where it goes in the atlas, and where its texels wait. */
struct gpu_upload {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    size_t offset;
};

/* The backend reads `size`, `stage`, `stage_used`, `uploads`, and
 * `n_uploads`. The rest is private to build.c. */
struct gpu_atlas {
    uint8_t *stage;
    size_t stage_used;
    size_t stage_cap;
    struct gpu_upload *uploads;
    uint32_t n_uploads;
    uint32_t cap_uploads;
    struct gpu_atlas_entry *map;
    struct gpu_shelf *shelves;
    uint32_t map_cap;
    uint32_t count;
    int n_shelves;
    int next_y;
    int size;
    /* The build of the frame ran out of room. */
    bool full;
};

/* One UI image slot. See "Images" at the top of this file. */
struct gpu_image {
    const uint32_t *pixels;
    uint64_t gen;
    struct vector2i size;
    uint64_t used;
    uint64_t frame;
    bool dirty;
};

/* The frame builder of one GPU backend. */
struct gpu_build {
    struct gpu_stream streams[GPU_ST_COUNT];
    struct gpu_cmd *cmds;
    uint32_t n_cmds;
    uint32_t cap_cmds;
    struct gpu_atlas atlas;
    /* The image slots of this build. See "Images" at the top of this file. */
    struct gpu_image *images;
    int n_images;
    int cap_images;
    /* The number of the last gpu_build_frame() call. It advances even when
     * that build fails. */
    uint64_t frame;

    /* The rest is private to build.c. */
    struct decor_cache *decor;
    struct rect2i *clips;
    uint64_t image_clock;
    int cap_clips;
    /* The build ran out of memory, or out of image slots. */
    bool failed;
};

/* Creates the atlas tables and the decoration cache of builder `b`. `b`
 * must be zeroed first. Returns false when an allocation fails;
 * gpu_build_release() then frees what was created. */
bool gpu_build_init(struct gpu_build *b);
/* Releases all memory allocated by builder `b`. */
void gpu_build_release(struct gpu_build *b);

/*
 * Generates the instance streams and draw commands of `frame` for
 * `canvas`, with the glyphs staged for the atlas. A full atlas is cleared
 * and doubled when it is below GPU_ATLAS_MAX_SIZE, and the build runs once
 * more. Returns false when the CPU must paint the frame; the builder has
 * then cleared its atlas state.
 */
bool gpu_build_frame(struct gpu_build *b, const struct render_frame *frame,
                     const struct render_grid_view *grid, struct vector2i canvas);
/* Clears the staged atlas uploads after the backend copied them into its
 * texture, and marks the UI images of the frame as loaded. */
void gpu_build_commit(struct gpu_build *b);
/* Forgets every atlas entry and drops the staged uploads when the backend did
 * not take the frame, so that no entry points to pixels that were never
 * uploaded. */
void gpu_build_abort(struct gpu_build *b);

/* Returns true when the backend must load the pixels of slot `index` before
 * it draws this frame. */
static inline bool gpu_image_needs_upload(const struct gpu_build *b, int index)
{
    return b->images[index].frame == b->frame && b->images[index].dirty;
}

/*
 * Calculates the aligned byte offset of each stream in the instance buffer,
 * and returns the buffer size, at least GPU_STREAM_ALIGN bytes.
 */
size_t gpu_build_instance_layout(const struct gpu_build *b, size_t offsets[GPU_ST_COUNT]);
/* Copies instance data from all streams into contiguous buffer `dst` using `offsets`. */
void gpu_build_copy_instances(const struct gpu_build *b, const size_t offsets[GPU_ST_COUNT],
                              void *dst);

/*
 * Returns the blend mode of instance `index` in `stream`, one of enum
 * gpu_blend. A backend reads the mode of the first instance of a command,
 * because a command holds one mode.
 */
uint32_t gpu_stream_blend(const struct gpu_build *b, enum gpu_stream_id stream, uint32_t index);

/* Converts an ARGB8888 color integer and opacity value to normalized RGBA floats. */
static inline void gpu_color(uint32_t argb, float opacity, float out[4])
{
    out[0] = ((argb >> 16) & 0xff) / 255.0f;
    out[1] = ((argb >> 8) & 0xff) / 255.0f;
    out[2] = (argb & 0xff) / 255.0f;
    out[3] = ((argb >> 24) & 0xff) / 255.0f * opacity;
}

/*
 * Clips the rects of `damage` to `fb->size` and measures the pixels that they
 * cover. `whole`, a NULL list, or an all-damage list gives the whole
 * framebuffer as one rect. `rects` must hold DAMAGE_RECTS_MAX entries.
 * Returns the number of rects and writes the total byte count to `bytes`.
 */
int gpu_pixels_rects(const struct framebuffer *fb, const struct damage_list *damage, bool whole,
                     struct rect2i *rects, size_t *bytes);
/* Copies the pixels of the `n` rects of gpu_pixels_rects() from `fb` into
 * `dst`, row by row without gaps, in rect order. `dst` must hold the byte
 * count that gpu_pixels_rects() wrote. */
void gpu_pixels_pack(const struct framebuffer *fb, const struct rect2i *rects, int n, void *dst);
/* Builds the quad that draws a texture of `size` texels over `size` pixels
 * of the canvas. */
void gpu_pixels_quad(struct vector2i size, struct gpu_glyph_inst *quad);

#endif
