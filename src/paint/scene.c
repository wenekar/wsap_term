#include "scene.h"
#include "shadow.h"
#include "text_run.h"
#include "util/grow.h"

#include <limits.h>
#include <string.h>

#define HASH_SEED 1469598103934665603ULL
#define HASH_PRIME 1099511628211ULL

void ui_scene_init(struct ui_scene *s)
{
    if (!s)
        return;
    s->n_ops = 0;
    s->n_scopes = 0;
    s->scope_depth = 0;
    s->clip_depth = 0;
    s->text_len = 0;
    s->origin = (struct vector2i){0};
    s->overflow = false;
}

void ui_scene_destroy(struct ui_scene *s)
{
    if (!s)
        return;
    free(s->ops);
    free(s->scopes);
    free(s->clips);
    free(s->stack);
    free(s->text);
    *s = (struct ui_scene){0};
}

static bool copy_into(void *buf, int *cap, const void *src, int n, size_t size)
{
    if (!util_grow(buf, cap, n, size))
        return false;
    void *dst;
    memcpy(&dst, buf, sizeof(dst));
    if (n)
        memcpy(dst, src, (size_t)n * size);
    return true;
}

bool ui_scene_copy(struct ui_scene *dst, const struct ui_scene *src)
{
    if (!dst || !src)
        return false;
    if (dst == src)
        return true;
    free(dst->stack);
    dst->stack = NULL;
    dst->scope_depth = 0;
    dst->stack_cap = 0;
    dst->n_ops = 0;
    dst->n_scopes = 0;
    dst->clip_depth = 0;
    dst->text_len = 0;
    dst->origin = (struct vector2i){0};
    dst->overflow = false;
    if (!copy_into(&dst->ops, &dst->ops_cap, src->ops, src->n_ops,
                   sizeof(*src->ops)) ||
        !copy_into(&dst->scopes, &dst->scopes_cap, src->scopes,
                   src->n_scopes, sizeof(*src->scopes)) ||
        !copy_into(&dst->clips, &dst->clips_cap, src->clips,
                   src->clip_depth, sizeof(*src->clips)) ||
        !copy_into(&dst->text, &dst->text_cap, src->text,
                   (int)src->text_len, 1)) {
        ui_scene_destroy(dst);
        dst->overflow = true;
        return false;
    }
    dst->n_ops = src->n_ops;
    dst->n_scopes = src->n_scopes;
    dst->clip_depth = src->clip_depth;
    dst->text_len = src->text_len;
    dst->origin = src->origin;
    dst->overflow = src->overflow;
    return true;
}

static uint64_t hash_bytes(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, sizeof(w));
        h = (h ^ w) * HASH_PRIME;
    }
    for (; i < n; i++)
        h = (h ^ b[i]) * HASH_PRIME;
    return h;
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static int scope_innermost(const struct ui_scene *s)
{
    return s->scope_depth ? s->stack[s->scope_depth - 1].scope : -1;
}

static bool scope_push(struct ui_scene *s, uint64_t key, const void *owner,
                       struct vector2i origin)
{
    if (!UTIL_GROW(s->stack, s->stack_cap, s->scope_depth + 1) ||
        !UTIL_GROW(s->scopes, s->scopes_cap, s->n_scopes + 1)) {
        s->overflow = true;
        return false;
    }
    int parent = scope_innermost(s);
    int idx = s->n_scopes++;
    int ordinal = parent >= 0 ? s->scopes[parent].children++ : 0;
    s->scopes[idx] = (struct ui_scene_scope){
        .key = key,
        .hash = hash_bytes(HASH_SEED, &ordinal, sizeof(ordinal)),
        .owner = owner,
        .parent = parent,
        .depth = s->scope_depth,
        .first_op = s->n_ops,
        .end_op = s->n_ops,
    };
    s->stack[s->scope_depth++] = (struct ui_scene_frame){
        .scope = idx, .clip_base = s->clip_depth, .origin = s->origin,
    };
    s->origin = origin;
    return true;
}

static struct ui_scene_scope *scope_current(struct ui_scene *s)
{
    if (!s->scope_depth && !scope_push(s, 0, NULL, s->origin))
        return NULL;
    int idx = scope_innermost(s);
    return idx >= 0 ? &s->scopes[idx] : NULL;
}

static bool scope_begin(struct ui_scene *s, uint64_t key, const void *owner,
                        struct vector2i origin)
{
    if (!s || s->overflow)
        return false;
    scope_current(s);
    return scope_push(s, key, owner, origin);
}

bool ui_scene_begin_node(struct ui_scene *s, const void *owner,
                         struct vector2i origin)
{
    return scope_begin(s, (uint64_t)(uintptr_t)owner, owner, origin);
}

bool ui_scene_begin_part(struct ui_scene *s, uint64_t id)
{
    if (!s)
        return false;
    struct ui_scene_scope *cur = scope_current(s);
    uint64_t parent = cur ? cur->key : 0;
    return scope_begin(s, mix64(parent ^ mix64(id + 1)), NULL, s->origin);
}

bool ui_scene_end_scope(struct ui_scene *s)
{
    if (!s || s->scope_depth <= 1) {
        if (s)
            s->overflow = true;
        return false;
    }
    struct ui_scene_frame f = s->stack[--s->scope_depth];
    s->origin = f.origin;
    s->scopes[f.scope].end_op = s->n_ops;
    if (s->clip_depth != f.clip_base) {
        s->overflow = true;
        return false;
    }
    return true;
}

static struct rect2i translate_sat(struct rect2i r, struct vector2i d)
{
    r.pos.x = geom_sat_i((int64_t)r.pos.x + d.x);
    r.pos.y = geom_sat_i((int64_t)r.pos.y + d.y);
    return r;
}

static struct ui_scene_op *scene_alloc(struct ui_scene *s,
                                       enum ui_scene_op_kind kind,
                                       struct rect2i local)
{
    if (!s || s->overflow || !UTIL_GROW(s->ops, s->ops_cap, s->n_ops + 1)) {
        if (s)
            s->overflow = true;
        return NULL;
    }
    struct ui_scene_op *op = &s->ops[s->n_ops];
    *op = (struct ui_scene_op){
        .kind = kind,
        .rect = translate_sat(local, s->origin),
    };
    return op;
}

static struct rect2i op_ink(const struct ui_scene *s,
                            const struct ui_scene_op *op)
{
    switch (op->kind) {
    case UI_SCENE_OP_PUSH_CLIP:
    case UI_SCENE_OP_POP_CLIP:
        return (struct rect2i){0};
    case UI_SCENE_OP_TEXT:
        return ui_scene_text_ink(s, op);
    default:
        return op->rect;
    }
}

static uint64_t hash_op(uint64_t h, const struct ui_scene *s,
                        const struct ui_scene_op *op)
{
    h = hash_bytes(h, &op->kind, sizeof(op->kind));
    h = hash_bytes(h, &op->alpha, sizeof(op->alpha));
    h = hash_bytes(h, &op->rect, sizeof(op->rect));
    h = hash_bytes(h, &op->color, sizeof(op->color));
    h = hash_bytes(h, &op->radius, sizeof(op->radius));
    if (op->kind == UI_SCENE_OP_SHADOW)
        h = hash_bytes(h, &op->blur, sizeof(op->blur));
    if (op->kind == UI_SCENE_OP_IMAGE) {
        h = hash_bytes(h, &op->image, sizeof(op->image));
        h = hash_bytes(h, &op->image->gen, sizeof(op->image->gen));
    }
    if (op->kind == UI_SCENE_OP_TEXT) {
        h = hash_bytes(h, &op->style.advance, sizeof(op->style.advance));
        h = hash_bytes(h, &op->style.cell_h, sizeof(op->style.cell_h));
        h = hash_bytes(h, &op->style.flags, sizeof(op->style.flags));
        h = hash_bytes(h, &op->text_len, sizeof(op->text_len));
        h = hash_bytes(h, s->text + op->text_off, op->text_len);
    }
    return h;
}

static bool scene_commit(struct ui_scene *s, struct ui_scene_op *op)
{
    struct ui_scene_scope *sc = scope_current(s);
    s->n_ops++;
    struct rect2i ink = op_ink(s, op);
    uint64_t h = sc ? sc->hash : HASH_SEED;
    if (s->clip_depth) {
        struct rect2i clip = s->clips[s->clip_depth - 1];
        h = hash_bytes(h, &clip, sizeof(clip));
        ink = rect2i_intersect(ink, clip);
    }
    h = hash_op(h, s, op);
    if (sc) {
        sc->hash = h;
        sc->ink = rect2i_union(sc->ink, ink);
        sc->end_op = s->n_ops;
    }
    if (s->n_scopes)
        s->scopes[0].end_op = s->n_ops;
    return true;
}

bool ui_scene_push_clip(struct ui_scene *s, struct rect2i r)
{
    if (s && !UTIL_GROW(s->clips, s->clips_cap, s->clip_depth + 1)) {
        s->overflow = true;
        return false;
    }
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_PUSH_CLIP, r);
    if (!op)
        return false;
    scene_commit(s, op);
    struct rect2i clip = op->rect;
    if (s->clip_depth)
        clip = rect2i_intersect(clip, s->clips[s->clip_depth - 1]);
    s->clips[s->clip_depth++] = clip;
    return true;
}

bool ui_scene_pop_clip(struct ui_scene *s)
{
    int base = s && s->scope_depth
        ? s->stack[s->scope_depth - 1].clip_base : 0;
    if (!s || s->clip_depth <= base) {
        if (s)
            s->overflow = true;
        return false;
    }
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_POP_CLIP,
                                         (struct rect2i){0});
    if (!op)
        return false;
    s->clip_depth--;
    return scene_commit(s, op);
}

bool ui_scene_rect(struct ui_scene *s, struct rect2i r, uint32_t color)
{
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_RECT, r);
    if (!op)
        return false;
    op->color = color;
    return scene_commit(s, op);
}

bool ui_scene_blend_rect(struct ui_scene *s, struct rect2i r,
                         uint32_t color, uint8_t alpha)
{
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_BLEND_RECT, r);
    if (!op)
        return false;
    op->color = color;
    op->alpha = alpha;
    return scene_commit(s, op);
}

bool ui_scene_rounded_rect(struct ui_scene *s, struct rect2i r,
                           int radius, uint32_t color)
{
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_ROUNDED_RECT, r);
    if (!op)
        return false;
    op->radius = radius;
    op->color = color;
    return scene_commit(s, op);
}

bool ui_scene_circle(struct ui_scene *s, struct vector2i center,
                     int radius, uint32_t color, uint8_t alpha)
{
    if (radius <= 0)
        return true;
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_CIRCLE,
        (struct rect2i){
            .pos = { .x = center.x - radius, .y = center.y - radius },
            .size = { .x = radius * 2, .y = radius * 2 },
        });
    if (!op)
        return false;
    op->radius = radius;
    op->color = color;
    op->alpha = alpha;
    return scene_commit(s, op);
}

bool ui_scene_shadow(struct ui_scene *s, struct rect2i box, int radius,
                     int blur, uint32_t color)
{
    if (rect2i_is_empty(box) || !(color >> 24))
        return true;
    if (blur > UI_SHADOW_MAX_BLUR)
        blur = UI_SHADOW_MAX_BLUR;
    if (radius < 0)
        radius = 0;
    if (radius > UI_SHADOW_MAX_RADIUS)
        radius = UI_SHADOW_MAX_RADIUS;
    if (radius > box.width / 2)
        radius = box.width / 2;
    if (radius > box.height / 2)
        radius = box.height / 2;
    if (blur <= 0)
        return ui_scene_rounded_rect(s, box, radius, color);

    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_SHADOW,
        rect2i_inset(box, -ui_shadow_extent(blur)));
    if (!op)
        return false;
    op->radius = radius;
    op->blur = blur;
    op->color = color;
    return scene_commit(s, op);
}

bool ui_scene_image(struct ui_scene *s, struct vector2i pos,
                    const struct ui_image *image)
{
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return true;
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_IMAGE,
        (struct rect2i){ .pos = pos, .size = image->size });
    if (!op)
        return false;
    op->image = image;
    return scene_commit(s, op);
}

bool ui_scene_text_run(struct ui_scene *s, struct rect2i bounds,
                       struct ui_text_style style, uint32_t color,
                       const char *text, size_t len)
{
    if (!s || (!text && len))
        return false;
    if (!len)
        return true;
    if (len > INT32_MAX - s->text_len ||
        !UTIL_GROW(s->text, s->text_cap, (int)(s->text_len + len))) {
        s->overflow = true;
        return false;
    }
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_TEXT, bounds);
    if (!op)
        return false;
    op->color = color;
    op->style = style;
    op->text_off = s->text_len;
    op->text_len = (uint32_t)len;
    memcpy(s->text + s->text_len, text, len);
    s->text_len += (uint32_t)len;
    return scene_commit(s, op);
}

bool ui_scene_is_valid(const struct ui_scene *s)
{
    return s && !s->overflow && s->clip_depth == 0 && s->scope_depth <= 1;
}

struct rect2i ui_scene_bounds(const struct ui_scene *s)
{
    struct rect2i bounds = {0};
    if (!ui_scene_is_valid(s))
        return bounds;
    for (int i = 0; i < s->n_scopes; i++)
        bounds = rect2i_union(bounds, s->scopes[i].ink);
    return bounds;
}

const char *ui_scene_op_text(const struct ui_scene *s,
                             const struct ui_scene_op *op)
{
    if (!s || !op || op->kind != UI_SCENE_OP_TEXT ||
        op->text_off > s->text_len ||
        op->text_len > s->text_len - op->text_off)
        return NULL;
    return s->text + op->text_off;
}

struct rect2i ui_scene_text_box(const struct ui_scene *s,
                                const struct ui_scene_op *op)
{
    const char *text = ui_scene_op_text(s, op);
    if (!text || op->style.advance <= 0 || op->style.cell_h <= 0)
        return (struct rect2i){0};
    int64_t cells = ui_text_run_cells(text, op->text_len);
    return (struct rect2i){
        .pos = op->rect.pos,
        .size = { .x = geom_sat_i(cells * op->style.advance),
                  .y = op->style.cell_h },
    };
}

struct rect2i ui_scene_text_ink(const struct ui_scene *s,
                                const struct ui_scene_op *op)
{
    struct rect2i box = ui_scene_text_box(s, op);
    if (rect2i_is_empty(box))
        return box;
    int adv = op->style.advance;
    box.x = geom_sat_i((int64_t)box.x - adv);
    box.width = geom_sat_i((int64_t)box.width + 2 * (int64_t)adv);
    return box;
}
