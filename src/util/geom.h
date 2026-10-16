#ifndef WSAP_UTIL_GEOM_H
#define WSAP_UTIL_GEOM_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

struct vector2f {
    float x, y;
};

struct rect2f {
    struct vector2f pos;
    struct vector2f size;
};

struct vector2i {
    union {
        struct { int x, y; };
        int v[2];
    };
};

struct rect2i {
    union {
        struct { int x, y; };
        struct vector2i pos;
    };
    union {
        struct { int width, height; };
        struct vector2i size;
    };
};

struct insets2i {
    union {
        struct { int top, right, bottom, left; };
        struct { int t, r, b, l; };
        int v[4];
    };
};

static inline int geom_clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int geom_sat_i(int64_t v)
{
    return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;
}

static inline int64_t geom_half_up(int64_t v)
{
    int64_t n = v + 1;
    return n >= 0 ? n / 2 : -((-n + 1) / 2);
}

static inline struct vector2i vector2i_add(struct vector2i a, struct vector2i b)
{
    return (struct vector2i){ .x = geom_sat_i((int64_t)a.x + b.x), .y = geom_sat_i((int64_t)a.y + b.y) };
}

static inline struct vector2i vector2i_sub(struct vector2i a, struct vector2i b)
{
    return (struct vector2i){ .x = geom_sat_i((int64_t)a.x - b.x), .y = geom_sat_i((int64_t)a.y - b.y) };
}

static inline struct vector2i vector2i_min(struct vector2i a, struct vector2i b)
{
    return (struct vector2i){ .x = a.x < b.x ? a.x : b.x, .y = a.y < b.y ? a.y : b.y };
}

static inline struct vector2i vector2i_max(struct vector2i a, struct vector2i b)
{
    return (struct vector2i){ .x = a.x > b.x ? a.x : b.x, .y = a.y > b.y ? a.y : b.y };
}

static inline struct vector2i vector2i_clamp(struct vector2i v,
                                             struct vector2i lo,
                                             struct vector2i hi)
{
    return (struct vector2i){ .x = geom_clamp_i(v.x, lo.x, hi.x), .y = geom_clamp_i(v.y, lo.y, hi.y) };
}

static inline struct vector2i vector2i_splat(int v)
{
    return (struct vector2i){ .x = v, .y = v };
}

static inline bool vector2i_eq(struct vector2i a, struct vector2i b)
{
    return a.x == b.x && a.y == b.y;
}

static inline bool rect2i_eq(struct rect2i a, struct rect2i b)
{
    return vector2i_eq(a.pos, b.pos) && vector2i_eq(a.size, b.size);
}

static inline bool rect2i_is_empty(struct rect2i r)
{
    return r.width <= 0 || r.height <= 0;
}

static inline struct vector2i rect2i_end(struct rect2i r)
{
    return vector2i_add(r.pos, r.size);
}

static inline bool rect2i_contains(struct rect2i r, struct vector2i p)
{
    struct vector2i end = rect2i_end(r);
    return p.x >= r.x && p.x < end.x && p.y >= r.y && p.y < end.y;
}

/* True when the rects overlap or share an edge. */
static inline bool rect2i_touches(struct rect2i a, struct rect2i b)
{
    struct vector2i ae = rect2i_end(a), be = rect2i_end(b);
    return a.x <= be.x && b.x <= ae.x && a.y <= be.y && b.y <= ae.y;
}

static inline struct rect2i rect2i_from_corners(struct vector2i start,
                                                struct vector2i end)
{
    struct vector2i size = vector2i_sub(end, start);
    return (struct rect2i){
        .pos = start,
        .size = vector2i_max(size, (struct vector2i){ .x = 0, .y = 0 }),
    };
}

static inline struct rect2i rect2i_translate(struct rect2i r,
                                             struct vector2i delta)
{
    r.pos = vector2i_add(r.pos, delta);
    return r;
}

static inline struct rect2i rect2i_intersect(struct rect2i a, struct rect2i b)
{
    return rect2i_from_corners(vector2i_max(a.pos, b.pos),
                               vector2i_min(rect2i_end(a), rect2i_end(b)));
}

__attribute__((always_inline))
static inline struct rect2i rect2i_union(struct rect2i a, struct rect2i b)
{
    if (rect2i_is_empty(a)) return b;
    if (rect2i_is_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    /* Compute the endpoints and the spans in int64_t. A rect near INT_MAX
     * overflows a plain int addition, and the width subtraction too. */
    int64_t a_end_x = (int64_t)a.x + a.width;
    int64_t a_end_y = (int64_t)a.y + a.height;
    int64_t b_end_x = (int64_t)b.x + b.width;
    int64_t b_end_y = (int64_t)b.y + b.height;
    int64_t x1 = a_end_x > b_end_x ? a_end_x : b_end_x;
    int64_t y1 = a_end_y > b_end_y ? a_end_y : b_end_y;
    return (struct rect2i){
        .pos = { .x = geom_sat_i(x0), .y = geom_sat_i(y0) },
        .size = { .x = geom_sat_i(x1 - x0), .y = geom_sat_i(y1 - y0) },
    };
}

static inline bool rect2i_clip(struct rect2i *r, struct vector2i limit)
{
    if (!r || rect2i_is_empty(*r))
        return false;
    *r = rect2i_intersect(*r, (struct rect2i){
        .pos = { .x = 0, .y = 0 },
        .size = limit,
    });
    return !rect2i_is_empty(*r);
}

static inline struct rect2i rect2i_inset(struct rect2i r, int inset)
{
    int64_t twice = (int64_t)inset * 2;
    r.pos = vector2i_add(r.pos, vector2i_splat(inset));
    r.width = twice >= r.width ? 0 : geom_sat_i((int64_t)r.width - twice);
    r.height = twice >= r.height ? 0 : geom_sat_i((int64_t)r.height - twice);
    return r;
}

static inline struct insets2i insets2i_splat(int v)
{
    return (struct insets2i){ .top = v, .right = v, .bottom = v, .left = v };
}

static inline struct insets2i insets2i_sym(int v, int h)
{
    return (struct insets2i){ .top = v, .right = h, .bottom = v, .left = h };
}

static inline bool insets2i_eq(struct insets2i a, struct insets2i b)
{
    return a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom && a.left == b.left;
}

static inline struct vector2i insets2i_lead(struct insets2i i)
{
    return (struct vector2i){ .x = i.left, .y = i.top };
}

static inline struct vector2i insets2i_extent(struct insets2i i)
{
    return (struct vector2i){ .x = geom_sat_i((int64_t)i.left + i.right), .y = geom_sat_i((int64_t)i.top + i.bottom) };
}

static inline struct rect2i rect2i_apply_insets(struct rect2i r, struct insets2i insets)
{
    struct vector2i extent = insets2i_extent(insets);
    int64_t w = (int64_t)r.width - extent.x;
    int64_t h = (int64_t)r.height - extent.y;
    r.pos = vector2i_add(r.pos, insets2i_lead(insets));
    r.width = w <= 0 ? 0 : geom_sat_i(w);
    r.height = h <= 0 ? 0 : geom_sat_i(h);
    return r;
}

#endif
