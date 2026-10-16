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

struct ui_image {
    const uint32_t *pixels;
    struct vector2i size;
    uint64_t gen;
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
    uint16_t pad;
    uint32_t color;
    struct rect2i rect;
    union {
        struct { uint32_t text_off, text_len; };
        struct { int32_t blur; };
        const struct ui_image *image;
    };
    int radius;
    struct ui_text_style style;
};

_Static_assert(sizeof(struct ui_scene_op) == 48,
               "ui_scene_op should occupy exactly 48 bytes");

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
    struct vector2i origin;
    int n_ops, ops_cap;
    int n_scopes, scopes_cap;
    int scope_depth, stack_cap;
    int clip_depth, clips_cap;
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
bool ui_scene_blend_rect(struct ui_scene *s, struct rect2i r,
                         uint32_t color, uint8_t alpha);
bool ui_scene_rounded_rect(struct ui_scene *s, struct rect2i r,
                           int radius, uint32_t color);
bool ui_scene_circle(struct ui_scene *s, struct vector2i center,
                     int radius, uint32_t color, uint8_t alpha);
bool ui_scene_shadow(struct ui_scene *s, struct rect2i box, int radius,
                     int blur, uint32_t color);
bool ui_scene_image(struct ui_scene *s, struct vector2i pos,
                    const struct ui_image *image);
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

const char *ui_scene_op_text(const struct ui_scene *s,
                             const struct ui_scene_op *op);
struct rect2i ui_scene_text_box(const struct ui_scene *s,
                                const struct ui_scene_op *op);
struct rect2i ui_scene_text_ink(const struct ui_scene *s,
                                const struct ui_scene_op *op);

#endif
