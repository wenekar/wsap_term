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
    s->n_geoms = 0;
    s->n_points = 0;
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
    free(s->geoms);
    free(s->points);
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
    ui_scene_init(dst);
    if (!copy_into(&dst->ops, &dst->ops_cap, src->ops, src->n_ops,
                   sizeof(*src->ops)) ||
        !copy_into(&dst->scopes, &dst->scopes_cap, src->scopes,
                   src->n_scopes, sizeof(*src->scopes)) ||
        !copy_into(&dst->clips, &dst->clips_cap, src->clips,
                   src->clip_depth, sizeof(*src->clips)) ||
        !copy_into(&dst->text, &dst->text_cap, src->text,
                   (int)src->text_len, 1) ||
        !copy_into(&dst->geoms, &dst->geoms_cap, src->geoms,
                   src->n_geoms, sizeof(*src->geoms)) ||
        !copy_into(&dst->points, &dst->points_cap, src->points,
                   src->n_points, sizeof(*src->points))) {
        ui_scene_init(dst);
        dst->overflow = true;
        return false;
    }
    dst->n_ops = src->n_ops;
    dst->n_scopes = src->n_scopes;
    dst->clip_depth = src->clip_depth;
    dst->text_len = src->text_len;
    dst->n_geoms = src->n_geoms;
    dst->n_points = src->n_points;
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
        .geom = -1,
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
    /* Keep the hash of a scene that uses no blend mode as it was. */
    if (op->blend != UI_SCENE_BLEND_PREMUL)
        h = hash_bytes(h, &op->blend, sizeof(op->blend));
    h = hash_bytes(h, &op->flags, sizeof(op->flags));
    h = hash_bytes(h, &op->rect, sizeof(op->rect));
    h = hash_bytes(h, &op->color, sizeof(op->color));
    h = hash_bytes(h, &op->radius, sizeof(op->radius));
    if (op->kind == UI_SCENE_OP_SHADOW)
        h = hash_bytes(h, &op->blur, sizeof(op->blur));
    if (op->kind == UI_SCENE_OP_IMAGE) {
        const struct ui_image *image = op->image.src;
        h = hash_bytes(h, &image, sizeof(image));
        h = hash_bytes(h, &op->image.uv, sizeof(op->image.uv));
        if (image)
            h = hash_bytes(h, &image->gen, sizeof(image->gen));
    }
    if (ui_scene_op_has_geom(op) && op->geom < s->n_geoms) {
        const struct ui_scene_geom *g = &s->geoms[op->geom];
        h = hash_bytes(h, g, sizeof(*g));
        if (g->point_len && g->point_off + g->point_len <= (uint32_t)s->n_points)
            h = hash_bytes(h, s->points + g->point_off,
                           (size_t)g->point_len * sizeof(*s->points));
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

bool ui_scene_rect_stroke(struct ui_scene *s, struct rect2i box, int width,
                          uint32_t color)
{
    if (!ui_scene_ok(s))
        return false;
    if (width <= 0 || rect2i_is_empty(box) || !(color >> 24))
        return true;
    struct rect2i inner = rect2i_inset(box, width);
    if (rect2i_is_empty(inner))
        return ui_scene_rect(s, box, color);
    int right_x = geom_sat_i((int64_t)box.x + box.width - width);
    int bottom_y = geom_sat_i((int64_t)box.y + box.height - width);
    int side_y = geom_sat_i((int64_t)box.y + width);
    return ui_scene_rect(s, (struct rect2i){
            .pos = box.pos,
            .size = { .x = box.width, .y = width },
        }, color) &&
        ui_scene_rect(s, (struct rect2i){
            .pos = { .x = box.x, .y = bottom_y },
            .size = { .x = box.width, .y = width },
        }, color) &&
        ui_scene_rect(s, (struct rect2i){
            .pos = { .x = box.x, .y = side_y },
            .size = { .x = width, .y = inner.height },
        }, color) &&
        ui_scene_rect(s, (struct rect2i){
            .pos = { .x = right_x, .y = side_y },
            .size = { .x = width, .y = inner.height },
        }, color);
}

bool ui_scene_rect_border(struct ui_scene *s, struct rect2i box,
                          struct ui_scene_border_width width, uint32_t color)
{
    if (!ui_scene_ok(s))
        return false;
    if (rect2i_is_empty(box) || !(color >> 24))
        return true;
    if (width.left < 0) width.left = 0;
    if (width.top < 0) width.top = 0;
    if (width.right < 0) width.right = 0;
    if (width.bottom < 0) width.bottom = 0;
    if (width.top > box.height) width.top = box.height;
    if (width.bottom > box.height - width.top)
        width.bottom = box.height - width.top;
    if (width.left > box.width) width.left = box.width;
    if (width.right > box.width - width.left)
        width.right = box.width - width.left;
    int top = width.top, bottom = width.bottom;
    int left = width.left, right = width.right;
    bool ok = true;
    if (top)
        ok = ok && ui_scene_rect(s, (struct rect2i){ .pos = box.pos,
            .size = { .x = box.width, .y = top } }, color);
    if (bottom)
        ok = ok && ui_scene_rect(s, (struct rect2i){
            .pos = { .x = box.x, .y = geom_sat_i((int64_t)box.y + box.height - bottom) },
            .size = { .x = box.width, .y = bottom } }, color);
    int side_y = geom_sat_i((int64_t)box.y + top);
    int side_h = box.height - top - bottom;
    if (left && side_h > 0)
        ok = ok && ui_scene_rect(s, (struct rect2i){
            .pos = { .x = box.x, .y = side_y },
            .size = { .x = left, .y = side_h } }, color);
    if (right && side_h > 0)
        ok = ok && ui_scene_rect(s, (struct rect2i){
            .pos = { .x = geom_sat_i((int64_t)box.x + box.width - right), .y = side_y },
            .size = { .x = right, .y = side_h } }, color);
    return ok;
}

bool ui_scene_wavy_underline(struct ui_scene *s, struct rect2i run,
                             int amplitude, int period, int thickness,
                             uint32_t color)
{
    if (!ui_scene_ok(s))
        return false;
    if (run.width <= 0 || !(color >> 24))
        return true;
    if (amplitude < 1) amplitude = 1;
    if (thickness < 1) thickness = 1;
    int half = period / 2;
    if (half < 1) half = 1;
    int steps = amplitude < half ? amplitude : half;
    int x_end = geom_sat_i((int64_t)run.x + run.width);
    bool ok = true;
    for (int px = run.x; px < x_end && ok; ) {
        int next = geom_sat_i((int64_t)px + 2 * half);
        for (int phase = 0; phase < 2 && ok; phase++) {
            for (int i = 0; i < steps && ok; i++) {
                int base = px + phase * half;
                int x0 = base + i * half / steps;
                int x1 = base + (i + 1) * half / steps;
                if (x0 >= x_end)
                    break;
                if (x1 > x_end)
                    x1 = x_end;
                if (x1 <= x0)
                    continue;
                int y_off = phase ? i : steps - 1 - i;
                ok = ui_scene_rect(s, (struct rect2i){
                    .pos = { .x = x0, .y = geom_sat_i((int64_t)run.y + y_off) },
                    .size = { .x = x1 - x0, .y = thickness } }, color);
            }
        }
        px = next > px ? next : px + 1;
    }
    return ok;
}

/* ---- Float geometry --------------------------------------------------- */

static int floor_to_i(float v)
{
    int i = (int)v;
    return v < (float)i ? i - 1 : i;
}

static int ceil_to_i(float v)
{
    int i = (int)v;
    return v > (float)i ? i + 1 : i;
}

static struct ui_scene_point xform_point(struct ui_scene_xform t,
                                         struct ui_scene_point p)
{
    return (struct ui_scene_point){
        .x = p.x + t.xx * p.x + t.xy * p.y + t.dx,
        .y = p.y + t.yx * p.x + t.yy * p.y + t.dy,
    };
}

static struct rect2i point_ink(const struct ui_scene_point *p, int n,
                               float pad)
{
    if (!p || n <= 0)
        return (struct rect2i){0};
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].x < x0) x0 = p[i].x;
        if (p[i].x > x1) x1 = p[i].x;
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    x0 -= pad;
    y0 -= pad;
    x1 += pad;
    y1 += pad;
    struct vector2i pos = { .x = floor_to_i(x0), .y = floor_to_i(y0) };
    struct vector2i end = { .x = ceil_to_i(x1), .y = ceil_to_i(y1) };
    return (struct rect2i){ .pos = pos,
                            .size = { .x = end.x - pos.x, .y = end.y - pos.y } };
}

static struct ui_scene_geom *geom_add(struct ui_scene *s,
                                      const struct ui_scene_style *style)
{
    if (!s || s->overflow ||
        !UTIL_GROW(s->geoms, s->geoms_cap, s->n_geoms + 1)) {
        if (s)
            s->overflow = true;
        return NULL;
    }
    struct ui_scene_geom *g = &s->geoms[s->n_geoms++];
    *g = (struct ui_scene_geom){0};
    if (style)
        g->xform = style->xform;
    return g;
}

static bool points_add(struct ui_scene *s, const struct ui_scene_point *pts,
                       int n, uint32_t *off)
{
    if (!n)
        return true;
    if (!pts || !s || s->overflow ||
        !UTIL_GROW(s->points, s->points_cap, s->n_points + n)) {
        if (s)
            s->overflow = true;
        return false;
    }
    *off = (uint32_t)s->n_points;
    for (int i = 0; i < n; i++)
        s->points[s->n_points++] = pts[i];
    return true;
}

static struct ui_scene_op *op_add_geom(struct ui_scene *s,
                                       enum ui_scene_op_kind kind,
                                       const struct ui_scene_style *style,
                                       struct rect2i ink)
{
    struct ui_scene_op *op = scene_alloc(s, kind, ink);
    if (!op)
        return NULL;
    struct ui_scene_geom *g = geom_add(s, style);
    if (!g)
        return NULL;
    op->geom = s->n_geoms - 1;
    op->blend = style ? style->blend : UI_SCENE_BLEND_PREMUL;
    op->color = style ? style->color : 0;
    op->alpha = (uint8_t)(op->color >> 24);
    return op;
}

const struct ui_scene_geom *ui_scene_op_geom(const struct ui_scene *s,
                                             const struct ui_scene_op *op)
{
    if (!s || !ui_scene_op_has_geom(op) || op->geom >= s->n_geoms)
        return NULL;
    return &s->geoms[op->geom];
}

bool ui_scene_rect_f(struct ui_scene *s, struct rect2f r,
                     const struct ui_scene_style *style)
{
    if (!s)
        return false;
    if (r.size.x <= 0.0f || r.size.y <= 0.0f)
        return true;
    struct ui_scene_point corners[4] = {
        { r.pos.x, r.pos.y },
        { r.pos.x + r.size.x, r.pos.y },
        { r.pos.x + r.size.x, r.pos.y + r.size.y },
        { r.pos.x, r.pos.y + r.size.y },
    };
    struct ui_scene_xform xf = style ? style->xform : (struct ui_scene_xform){0};
    for (int i = 0; i < 4; i++)
        corners[i] = xform_point(xf, corners[i]);
    struct rect2i ink = point_ink(corners, 4, 0.0f);
    struct ui_scene_op *op = op_add_geom(s, UI_SCENE_OP_RECT, style, ink);
    if (!op)
        return false;
    struct ui_scene_geom *g = &s->geoms[op->geom];
    g->rect = r;
    g->flags = UI_GEOM_RECT_F;
    return scene_commit(s, op);
}

static bool path_emit(struct ui_scene *s, const struct ui_scene_point *pts,
                      int count, uint16_t flags, float width,
                      const struct ui_scene_style *style)
{
    if (!s)
        return false;
    if (count < 2)
        return true;
    struct ui_scene_xform xf = style ? style->xform : (struct ui_scene_xform){0};
    struct ui_scene_point local[64];
    struct ui_scene_point *tmp = local;
    if (count > (int)(sizeof(local) / sizeof(local[0]))) {
        tmp = malloc((size_t)count * sizeof(*tmp));
        if (!tmp) {
            s->overflow = true;
            return false;
        }
    }
    for (int i = 0; i < count; i++)
        tmp[i] = xform_point(xf, pts[i]);
    bool filled = (flags & UI_PATH_FILLED) != 0;
    struct rect2i ink = point_ink(tmp, count,
                                  filled ? 1.0f : (width > 0.0f ? width * 0.5f : 0.5f));
    free(tmp == local ? NULL : tmp);
    struct ui_scene_op *op = op_add_geom(s, UI_SCENE_OP_RECT, style, ink);
    if (!op)
        return false;
    struct ui_scene_geom *g = &s->geoms[op->geom];
    g->width = width > 0.0f ? width : 1.0f;
    g->flags = UI_GEOM_PATH | flags;
    if (!points_add(s, pts, count, &g->point_off))
        return false;
    g->point_len = (uint32_t)count;
    return scene_commit(s, op);
}

bool ui_scene_line(struct ui_scene *s, struct ui_scene_point a,
                   struct ui_scene_point b, float width,
                   const struct ui_scene_style *style)
{
    struct ui_scene_point pts[2] = { a, b };
    return path_emit(s, pts, 2, 0, width, style);
}

bool ui_scene_polyline(struct ui_scene *s, const struct ui_scene_point *points,
                       int count, bool closed, float width,
                       const struct ui_scene_style *style)
{
    return path_emit(s, points, count, closed ? UI_PATH_CLOSED : 0, width, style);
}

bool ui_scene_triangle(struct ui_scene *s, struct ui_scene_point a,
                       struct ui_scene_point b, struct ui_scene_point c,
                       const struct ui_scene_style *style)
{
    struct ui_scene_point pts[3] = { a, b, c };
    return path_emit(s, pts, 3, UI_PATH_CLOSED | UI_PATH_FILLED, 0.0f, style);
}

bool ui_scene_path(struct ui_scene *s, const struct ui_scene_point *points,
                   int count, bool closed, bool filled, float width,
                   const struct ui_scene_style *style)
{
    uint16_t flags = (uint16_t)((closed ? UI_PATH_CLOSED : 0) |
                                (filled ? UI_PATH_FILLED : 0));
    return path_emit(s, points, count, flags, width, style);
}

static bool gradient_emit(struct ui_scene *s, bool radial, struct rect2f r,
                          struct ui_scene_point from,
                          struct ui_scene_point to, float radius,
                          uint32_t color0, uint32_t color1,
                          const struct ui_scene_style *style)
{
    if (!s)
        return false;
    if (r.size.x <= 0.0f || r.size.y <= 0.0f)
        return true;
    struct ui_scene_xform xf = style ? style->xform : (struct ui_scene_xform){0};
    struct ui_scene_point corners[4] = {
        { r.pos.x, r.pos.y },
        { r.pos.x + r.size.x, r.pos.y },
        { r.pos.x + r.size.x, r.pos.y + r.size.y },
        { r.pos.x, r.pos.y + r.size.y },
    };
    for (int i = 0; i < 4; i++)
        corners[i] = xform_point(xf, corners[i]);
    struct rect2i ink = point_ink(corners, 4, 0.0f);
    struct ui_scene_op *op = op_add_geom(s, UI_SCENE_OP_RECT, style, ink);
    if (!op)
        return false;
    struct ui_scene_geom *g = &s->geoms[op->geom];
    g->rect = r;
    g->flags = radial ? UI_GEOM_RADIAL : UI_GEOM_LINEAR;
    g->from = from;
    g->to = to;
    g->radius = radius;
    g->color0 = color0;
    g->color1 = color1;
    op->alpha = (uint8_t)(color0 >> 24);
    return scene_commit(s, op);
}

bool ui_scene_linear_gradient(struct ui_scene *s, struct rect2f r,
                              struct ui_scene_point from,
                              struct ui_scene_point to, uint32_t color0,
                              uint32_t color1,
                              const struct ui_scene_style *style)
{
    return gradient_emit(s, false, r, from, to, 0.0f, color0, color1, style);
}

bool ui_scene_radial_gradient(struct ui_scene *s, struct rect2f r,
                              struct ui_scene_point center, float radius,
                              uint32_t color0, uint32_t color1,
                              const struct ui_scene_style *style)
{
    return gradient_emit(s, true, r, center, center, radius, color0, color1,
                         style);
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
    if (radius <= 0)
        return ui_scene_rect(s, r, color);
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

bool ui_scene_image_rect(struct ui_scene *s, struct rect2i dst,
                         const struct ui_image *image,
                         const struct ui_image_options *opt)
{
    if (!image || !image->pixels || image->size.x <= 0 || image->size.y <= 0)
        return true;
    struct ui_image_options options = opt ? *opt : (struct ui_image_options){0};
    struct rect2i uv = rect2i_is_empty(options.uv)
        ? (struct rect2i){ .size = image->size }
        : rect2i_intersect(options.uv, (struct rect2i){ .size = image->size });
    if (rect2i_is_empty(dst) || rect2i_is_empty(uv))
        return true;
    struct ui_scene_op *op = scene_alloc(s, UI_SCENE_OP_IMAGE, dst);
    if (!op)
        return false;
    op->image = (struct ui_image_draw){ .src = image, .uv = uv };
    op->color = options.tint;
    op->alpha = options.opacity ? options.opacity : 255;
    op->flags = (uint16_t)(options.flags & UI_IMAGE_LINEAR);
    return scene_commit(s, op);
}

bool ui_scene_image(struct ui_scene *s, struct vector2i pos,
                    const struct ui_image *image)
{
    if (!image)
        return true;
    return ui_scene_image_rect(s, (struct rect2i){ .pos = pos, .size = image->size },
                               image, NULL);
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

static bool op_visible(const struct ui_scene_op *op)
{
    return ui_scene_op_draws(op);
}

bool ui_scene_op_draws(const struct ui_scene_op *op)
{
    switch (op->kind) {
    case UI_SCENE_OP_PUSH_CLIP:
    case UI_SCENE_OP_POP_CLIP:
        return false;
    case UI_SCENE_OP_BLEND_RECT:
    case UI_SCENE_OP_CIRCLE:
        return op->alpha > 0;
    case UI_SCENE_OP_IMAGE:
        /* ui_scene_image() sets the tint to 0. The image draws by its source
         * and its opacity, not by its colour. */
        return op->image.src != NULL && op->alpha > 0;
    case UI_SCENE_OP_RECT:
        if (ui_scene_op_has_geom(op))
            return true;
        break;
    default:
        break;
    }
    return (op->color >> 24) > 0;
}

bool ui_scene_intersects_rect(const struct ui_scene *s, struct rect2i r)
{
    if (!ui_scene_is_valid(s) || rect2i_is_empty(r))
        return false;
    struct rect2i clips[32];
    int depth = 0;
    for (int i = 0; i < s->n_ops; i++) {
        const struct ui_scene_op *op = &s->ops[i];
        if (op->kind == UI_SCENE_OP_PUSH_CLIP) {
            if (depth >= 32)
                return true;
            struct rect2i c = op->rect;
            if (depth > 0)
                c = rect2i_intersect(c, clips[depth - 1]);
            clips[depth++] = c;
            continue;
        }
        if (op->kind == UI_SCENE_OP_POP_CLIP) {
            if (depth > 0)
                depth--;
            continue;
        }
        if (!op_visible(op))
            continue;
        struct rect2i ink = op_ink(s, op);
        if (depth > 0)
            ink = rect2i_intersect(ink, clips[depth - 1]);
        if (!rect2i_is_empty(rect2i_intersect(ink, r)))
            return true;
    }
    return false;
}
