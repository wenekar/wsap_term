#ifndef WSAP_UI_SCENE_H
#define WSAP_UI_SCENE_H

#include "util/geom.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum ui_scene_op_kind : uint8_t {
    UI_SCENE_OP_PUSH_CLIP,
    UI_SCENE_OP_POP_CLIP,
    UI_SCENE_OP_RECT,
    UI_SCENE_OP_BLEND_RECT,
    UI_SCENE_OP_ROUNDED_RECT,
    UI_SCENE_OP_CIRCLE,
    UI_SCENE_OP_SHADOW,
    UI_SCENE_OP_TEXT,
    UI_SCENE_OP_IMAGE,
};

/*
 * The blend mode of an op. All source colors are straight ARGB words.
 *
 * The modes are defined on 8-bit sRGB channels. They are the reference for
 * the GPU backends: the CPU rule is one integer expression, and the GPU
 * blend state and the shader give the same expression. So a scene gives the
 * same pixels on the CPU and the GPU, up to one unit of rounding.
 *
 *   PREMUL   The source channels are premultiplied. The result is
 *            src + dst * (1 - src.a). A straight color with alpha must use
 *            ALPHA, not PREMUL.
 *   ALPHA    The source channels are straight. Scale them by src.a, then
 *            blend as PREMUL. For an opaque color PREMUL and ALPHA agree.
 *   ADDITIVE The result is min (255, src + dst). The alpha adds too.
 *   MULTIPLY The result is src * dst / 255. The alpha blends as PREMUL.
 *   NONE     The source replaces the destination. The alpha replaces too.
 */
enum ui_scene_blend : uint8_t {
    UI_SCENE_BLEND_PREMUL = 0,
    UI_SCENE_BLEND_ALPHA,
    UI_SCENE_BLEND_ADDITIVE,
    UI_SCENE_BLEND_MULTIPLY,
    UI_SCENE_BLEND_NONE,
};

/*
 * A 2D affine transform. The all-zero value is the identity:
 *   x' = x + xx * x + xy * y + dx
 *   y' = y + yx * x + yy * y + dy
 */
struct ui_scene_xform {
    float xx, xy, yx, yy, dx, dy;
};

struct ui_scene_point {
    float x, y;
};

/* The style of a float op: its color, its blend mode, and its transform. */
struct ui_scene_style {
    uint32_t color;
    enum ui_scene_blend blend;
    struct ui_scene_xform xform;
};

/*
 * The kinds of an op that has a geom entry. Such an op has the kind
 * UI_SCENE_OP_RECT: its geom holds the real geometry, and its rect is the
 * covering box. The UI planner and the debug tool thus need no new case.
 * Read the `geom` field of an op only through ui_scene_op_has_geom().
 */
#define UI_GEOM_RECT_F 1u
#define UI_GEOM_PATH 2u
#define UI_GEOM_LINEAR 4u
#define UI_GEOM_RADIAL 8u

/* The geom flags of a path op. */
#define UI_PATH_CLOSED 16u
#define UI_PATH_FILLED 32u

/*
 * The float geometry of an op. The op holds its index in `geom`.
 * `rect` is the float bounds before the transform. `width` is the stroke
 * width of a path. `point_off` and `point_len` name the points in
 * `ui_scene.points`. A gradient holds its two colors and its axis.
 */
struct ui_scene_geom {
    struct rect2f rect;
    struct ui_scene_xform xform;
    struct ui_scene_point from, to;
    float width;
    float radius;
    uint32_t point_off, point_len;
    uint32_t color0, color1;
    uint16_t flags;
};

/*
 * A picture for a scene. `pixels` holds `size.x * size.y` words of
 * premultiplied ARGB: each color channel is already multiplied by the
 * alpha channel. The words match the BGRA texels of the GPU backends.
 * The painter blends a texel over the canvas with `src + dst * (1 - src.a)`.
 *
 * `gen` identifies the pixels. It goes up when the pixels change in place.
 * A gen of 0 means that the pixels have no identity between frames, so a
 * GPU backend loads them in every frame.
 */
struct ui_image {
    const uint32_t *pixels;
    struct vector2i size;
    uint64_t gen;
};

/*
 * How an image op samples the source texels. Destination pixel `i` of `n`
 * samples the source coordinate `(i + 0.5) * src / n`: nearest takes the
 * texel that holds the coordinate, linear mixes four texels by the rest of
 * the way. The CPU painter and the GPU shaders use the same rule.
 */
#define UI_IMAGE_LINEAR 1u

/*
 * The parts of an image op that are not its rectangle. A zero struct draws
 * the whole image, one texel for each pixel, opaque, and with no tint.
 */
struct ui_image_options {
    /* The source texel rect. A zero size samples the whole image. */
    struct rect2i uv;
    /* A straight tint color. 0 is white. Its alpha scales the opacity. */
    uint32_t tint;
    /* 0 to 255. Zero gives full opacity. */
    uint8_t opacity;
    /* UI_IMAGE_LINEAR selects a bilinear sample. */
    uint16_t flags;
};

/* The source of an image op: the image and the texel rect of its quad. */
struct ui_image_draw {
    const struct ui_image *src;
    struct rect2i uv;
};

enum ui_text_flags {
    UI_TEXT_PLAIN  = 0,
    UI_TEXT_BOLD   = 1 << 0,
    UI_TEXT_ITALIC = 1 << 1,
};

struct ui_text_style {
    int advance;
    int cell_h;
    uint16_t flags;
    uint16_t _pad;
};

struct ui_scene_op {
    enum ui_scene_op_kind kind;
    uint8_t alpha;
    /* The blend mode of the op. See enum ui_scene_blend. */
    enum ui_scene_blend blend;
    /* The image flags of an image op. Zero for every other kind. */
    uint16_t flags;
    uint32_t color;
    /* The integer bounds of the op. A float op keeps its real bounds in
     * its geom entry; this rect is the covering box. */
    struct rect2i rect;
    union {
        struct { uint32_t text_off, text_len; };
        struct { int32_t blur; };
        struct ui_image_draw image;
        /* The index in ui_scene.geoms, or -1. */
        int32_t geom;
    };
    int radius;
    struct ui_text_style style;
};

_Static_assert(sizeof(struct ui_scene_op) == 72,
               "ui_scene_op should occupy exactly 72 bytes");

struct ui_scene_scope {
    uint64_t key;
    uint64_t hash;
    const void *owner;
    struct rect2i ink;
    int parent;
    int depth;
    int first_op, end_op;
    int children;
};

struct ui_scene_frame {
    int scope;
    int clip_base;
    struct vector2i origin;
};

struct ui_scene {
    struct ui_scene_op *ops;
    struct ui_scene_scope *scopes;
    struct rect2i *clips;
    struct ui_scene_frame *stack;
    char *text;
    struct ui_scene_geom *geoms;
    struct ui_scene_point *points;
    struct vector2i origin;
    int n_ops, ops_cap;
    int n_scopes, scopes_cap;
    int scope_depth, stack_cap;
    int clip_depth, clips_cap;
    int n_geoms, geoms_cap;
    int n_points, points_cap;
    uint32_t text_len;
    int text_cap;
    bool overflow;
};

void ui_scene_init(struct ui_scene *s);
void ui_scene_destroy(struct ui_scene *s);
bool ui_scene_copy(struct ui_scene *dst, const struct ui_scene *src);

bool ui_scene_push_clip(struct ui_scene *s, struct rect2i r);
bool ui_scene_pop_clip(struct ui_scene *s);
bool ui_scene_rect(struct ui_scene *s, struct rect2i r, uint32_t color);
/* Emits four non-overlapping edge rectangles to outline `box`. */
bool ui_scene_rect_stroke(struct ui_scene *s, struct rect2i box, int width,
                          uint32_t color);

/* The width of each side of a border. Zero draws no side. */
struct ui_scene_border_width {
    int left, top, right, bottom;
};
/*
 * Emits one rectangle for each side with a width above zero. The sides do
 * not overlap. A corner belongs to the top or bottom side.
 */
bool ui_scene_rect_border(struct ui_scene *s, struct rect2i box,
                          struct ui_scene_border_width width, uint32_t color);
/*
 * Emits a wavy underline. The wave is a triangle staircase of the `amplitude`
 * rows below `run.y`, with one period of `period` columns. `thickness` is the
 * height of each step. The run starts at `run.x` and takes `run.width`
 * columns. The steps do not overlap.
 */
bool ui_scene_wavy_underline(struct ui_scene *s, struct rect2i run,
                             int amplitude, int period, int thickness,
                             uint32_t color);
/*
 * The float ops. `ui_scene_rect_f` fills the transformed rect with its
 * center inside the rect. The path ops stroke lines of `width` pixels, and
 * a closed filled path covers the polygon of its points. A gradient paints
 * the rect of the last argument. A zero `style` gives the identity
 * transform and UI_SCENE_BLEND_PREMUL.
 */
bool ui_scene_rect_f(struct ui_scene *s, struct rect2f r,
                     const struct ui_scene_style *style);
bool ui_scene_line(struct ui_scene *s, struct ui_scene_point a,
                   struct ui_scene_point b, float width,
                   const struct ui_scene_style *style);
bool ui_scene_polyline(struct ui_scene *s, const struct ui_scene_point *points,
                       int count, bool closed, float width,
                       const struct ui_scene_style *style);
bool ui_scene_triangle(struct ui_scene *s, struct ui_scene_point a,
                       struct ui_scene_point b, struct ui_scene_point c,
                       const struct ui_scene_style *style);
bool ui_scene_path(struct ui_scene *s, const struct ui_scene_point *points,
                   int count, bool closed, bool filled, float width,
                   const struct ui_scene_style *style);
bool ui_scene_linear_gradient(struct ui_scene *s, struct rect2f r,
                              struct ui_scene_point from,
                              struct ui_scene_point to, uint32_t color0,
                              uint32_t color1,
                              const struct ui_scene_style *style);
bool ui_scene_radial_gradient(struct ui_scene *s, struct rect2f r,
                              struct ui_scene_point center, float radius,
                              uint32_t color0, uint32_t color1,
                              const struct ui_scene_style *style);
const struct ui_scene_geom *ui_scene_op_geom(const struct ui_scene *s,
                                             const struct ui_scene_op *op);
/*
 * Returns true when `op` has a geom entry. Only a RECT op owns the `geom`
 * field. The union of `op` shares that field with the text offsets, the
 * blur radius, and the image of the other kinds, so an op of any other
 * kind reads a value that is not an index.
 */
static inline bool ui_scene_op_has_geom(const struct ui_scene_op *op)
{
    return op->kind == UI_SCENE_OP_RECT && op->geom >= 0;
}
bool ui_scene_blend_rect(struct ui_scene *s, struct rect2i r,
                         uint32_t color, uint8_t alpha);
bool ui_scene_rounded_rect(struct ui_scene *s, struct rect2i r,
                           int radius, uint32_t color);
bool ui_scene_circle(struct ui_scene *s, struct vector2i center,
                     int radius, uint32_t color, uint8_t alpha);
bool ui_scene_shadow(struct ui_scene *s, struct rect2i box, int radius,
                     int blur, uint32_t color);
/* Draws the whole image at `pos`, one texel for each pixel. */
bool ui_scene_image(struct ui_scene *s, struct vector2i pos,
                    const struct ui_image *image);
/* Draws the image into `dst`. `opt` may be NULL. */
bool ui_scene_image_rect(struct ui_scene *s, struct rect2i dst,
                         const struct ui_image *image,
                         const struct ui_image_options *opt);
bool ui_scene_text_run(struct ui_scene *s, struct rect2i bounds,
                       struct ui_text_style style, uint32_t color,
                       const char *text, size_t len);

bool ui_scene_begin_node(struct ui_scene *s, const void *owner,
                         struct vector2i origin);
bool ui_scene_begin_part(struct ui_scene *s, uint64_t id);
bool ui_scene_end_scope(struct ui_scene *s);

bool ui_scene_is_valid(const struct ui_scene *s);
static inline bool ui_scene_ok(const struct ui_scene *s)
{
    return s && !s->overflow;
}
struct rect2i ui_scene_bounds(const struct ui_scene *s);
bool ui_scene_intersects_rect(const struct ui_scene *s, struct rect2i r);

const char *ui_scene_op_text(const struct ui_scene *s,
                             const struct ui_scene_op *op);
struct rect2i ui_scene_text_box(const struct ui_scene *s,
                                const struct ui_scene_op *op);
struct rect2i ui_scene_text_ink(const struct ui_scene *s,
                                const struct ui_scene_op *op);

#endif
