#include "util/geom.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>

static void test_vector2i_basic(void)
{
    struct vector2i a = { .x = 10, .y = 20 };
    struct vector2i b = { .x = 5, .y = -15 };

    assert(vector2i_eq(vector2i_add(a, b), ((struct vector2i){ .x = 15, .y = 5 })));
    assert(vector2i_eq(vector2i_sub(a, b), ((struct vector2i){ .x = 5, .y = 35 })));
    assert(vector2i_eq(vector2i_min(a, b), ((struct vector2i){ .x = 5, .y = -15 })));
    assert(vector2i_eq(vector2i_max(a, b), ((struct vector2i){ .x = 10, .y = 20 })));
    assert(vector2i_eq(vector2i_splat(42), ((struct vector2i){ .x = 42, .y = 42 })));
    assert(vector2i_eq(a, ((struct vector2i){ .x = 10, .y = 20 })));
    assert(!vector2i_eq(a, b));
}

static void test_vector2i_clamp(void)
{
    struct vector2i lo = { .x = -10, .y = 0 };
    struct vector2i hi = { .x = 10, .y = 100 };

    struct vector2i v1 = { .x = -50, .y = 50 };
    assert(vector2i_eq(vector2i_clamp(v1, lo, hi), ((struct vector2i){ .x = -10, .y = 50 })));

    struct vector2i v2 = { .x = 50, .y = 150 };
    assert(vector2i_eq(vector2i_clamp(v2, lo, hi), ((struct vector2i){ .x = 10, .y = 100 })));

    struct vector2i v3 = { .x = 0, .y = -20 };
    assert(vector2i_eq(vector2i_clamp(v3, lo, hi), ((struct vector2i){ .x = 0, .y = 0 })));
}

static void test_vector2i_saturation(void)
{
    struct vector2i max_v = { .x = INT_MAX, .y = INT_MAX };
    struct vector2i min_v = { .x = INT_MIN, .y = INT_MIN };
    struct vector2i one = { .x = 1, .y = 1 };
    struct vector2i neg_one = { .x = -1, .y = -1 };

    assert(vector2i_eq(vector2i_add(max_v, one), max_v));
    assert(vector2i_eq(vector2i_add(max_v, max_v), max_v));
    assert(vector2i_eq(vector2i_add(min_v, neg_one), min_v));
    assert(vector2i_eq(vector2i_add(min_v, min_v), min_v));

    assert(vector2i_eq(vector2i_sub(max_v, neg_one), max_v));
    assert(vector2i_eq(vector2i_sub(min_v, one), min_v));
    assert(vector2i_eq(vector2i_sub(min_v, max_v), min_v));
    assert(vector2i_eq(vector2i_sub(max_v, min_v), max_v));
}

static void test_rect2i_layout_and_zero_init(void)
{
    assert(sizeof(struct vector2i) == sizeof(int) * 2);
    assert(sizeof(struct rect2i) == sizeof(int) * 4);

    struct rect2i z = {0};
    assert(z.x == 0 && z.y == 0 && z.width == 0 && z.height == 0);
    assert(z.pos.x == 0 && z.pos.y == 0 && z.size.x == 0 && z.size.y == 0);
    assert(rect2i_is_empty(z));

    struct rect2i r = {
        .pos = { .x = 10, .y = 20 },
        .size = { .x = 100, .y = 200 },
    };
    assert(r.x == 10 && r.y == 20);
    assert(r.width == 100 && r.height == 200);
    assert(r.pos.x == 10 && r.pos.y == 20);
    assert(r.size.x == 100 && r.size.y == 200);

    r.x = 15;
    assert(r.pos.x == 15);
    r.pos.y = 25;
    assert(r.y == 25);
    r.width = 300;
    assert(r.size.x == 300);
    r.size.y = 400;
    assert(r.height == 400);
}

static void test_rect2i_empty_and_end(void)
{
    struct rect2i r_valid = { .pos = { .x = 5, .y = 5 }, .size = { .x = 10, .y = 20 } };
    assert(!rect2i_is_empty(r_valid));
    assert(vector2i_eq(rect2i_end(r_valid), ((struct vector2i){ .x = 15, .y = 25 })));

    struct rect2i r_zero_w = { .pos = { .x = 5, .y = 5 }, .size = { .x = 0, .y = 20 } };
    assert(rect2i_is_empty(r_zero_w));

    struct rect2i r_zero_h = { .pos = { .x = 5, .y = 5 }, .size = { .x = 10, .y = 0 } };
    assert(rect2i_is_empty(r_zero_h));

    struct rect2i r_neg_w = { .pos = { .x = 5, .y = 5 }, .size = { .x = -1, .y = 20 } };
    assert(rect2i_is_empty(r_neg_w));

    struct rect2i r_neg_h = { .pos = { .x = 5, .y = 5 }, .size = { .x = 10, .y = -5 } };
    assert(rect2i_is_empty(r_neg_h));

    struct rect2i r_huge = { .pos = { .x = INT_MAX - 10, .y = INT_MAX - 20 }, .size = { .x = 100, .y = 100 } };
    assert(vector2i_eq(rect2i_end(r_huge), ((struct vector2i){ .x = INT_MAX, .y = INT_MAX })));
}

static void test_rect2i_contains(void)
{
    struct rect2i r = { .pos = { .x = 10, .y = 20 }, .size = { .x = 30, .y = 40 } };

    assert(rect2i_contains(r, (struct vector2i){ .x = 10, .y = 20 }));

    assert(rect2i_contains(r, (struct vector2i){ .x = 11, .y = 21 }));
    assert(rect2i_contains(r, (struct vector2i){ .x = 25, .y = 40 }));
    assert(rect2i_contains(r, (struct vector2i){ .x = 39, .y = 59 }));

    assert(!rect2i_contains(r, (struct vector2i){ .x = 40, .y = 60 }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = 40, .y = 20 }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = 10, .y = 60 }));

    assert(!rect2i_contains(r, (struct vector2i){ .x = 9, .y = 20 }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = 10, .y = 19 }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = 41, .y = 30 }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = 20, .y = 61 }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = INT_MIN, .y = INT_MIN }));
    assert(!rect2i_contains(r, (struct vector2i){ .x = INT_MAX, .y = INT_MAX }));

    struct rect2i r_empty = { .pos = { .x = 10, .y = 20 }, .size = { .x = 0, .y = 0 } };
    assert(!rect2i_contains(r_empty, (struct vector2i){ .x = 10, .y = 20 }));

    struct rect2i r_neg = { .pos = { .x = 10, .y = 20 }, .size = { .x = -5, .y = 10 } };
    assert(!rect2i_contains(r_neg, (struct vector2i){ .x = 10, .y = 20 }));
    assert(!rect2i_contains(r_neg, (struct vector2i){ .x = 8, .y = 25 }));
}

static void test_rect2i_from_corners(void)
{
    struct vector2i start = { .x = 10, .y = 20 };
    struct vector2i end = { .x = 40, .y = 60 };
    struct rect2i r = rect2i_from_corners(start, end);
    assert(vector2i_eq(r.pos, start));
    assert(vector2i_eq(r.size, ((struct vector2i){ .x = 30, .y = 40 })));

    struct rect2i r_zero = rect2i_from_corners(start, start);
    assert(vector2i_eq(r_zero.pos, start));
    assert(vector2i_eq(r_zero.size, ((struct vector2i){ .x = 0, .y = 0 })));

    struct rect2i r_inv = rect2i_from_corners(end, start);
    assert(vector2i_eq(r_inv.pos, end));
    assert(vector2i_eq(r_inv.size, ((struct vector2i){ .x = 0, .y = 0 })));

    struct rect2i r_over = rect2i_from_corners((struct vector2i){ .x = INT_MIN, .y = INT_MIN },
                                              (struct vector2i){ .x = INT_MAX, .y = INT_MAX });
    assert(vector2i_eq(r_over.pos, ((struct vector2i){ .x = INT_MIN, .y = INT_MIN })));
    assert(vector2i_eq(r_over.size, ((struct vector2i){ .x = INT_MAX, .y = INT_MAX })));
}

static void test_rect2i_translate(void)
{
    struct rect2i r = { .pos = { .x = 10, .y = 20 }, .size = { .x = 30, .y = 40 } };
    struct rect2i shifted = rect2i_translate(r, (struct vector2i){ .x = 5, .y = -10 });
    assert(vector2i_eq(shifted.pos, ((struct vector2i){ .x = 15, .y = 10 })));
    assert(vector2i_eq(shifted.size, ((struct vector2i){ .x = 30, .y = 40 })));

    struct rect2i sat = rect2i_translate(r, (struct vector2i){ .x = INT_MAX, .y = INT_MIN });
    assert(sat.pos.x == INT_MAX && sat.pos.y == INT_MIN + 20);
    assert(vector2i_eq(sat.size, r.size));
}

static void test_rect2i_intersect(void)
{
    struct rect2i a = { .pos = { .x = 10, .y = 10 }, .size = { .x = 20, .y = 20 } };
    struct rect2i b = { .pos = { .x = 20, .y = 20 }, .size = { .x = 20, .y = 20 } };

    struct rect2i inter = rect2i_intersect(a, b);
    assert(!rect2i_is_empty(inter));
    assert(vector2i_eq(inter.pos, ((struct vector2i){ .x = 20, .y = 20 })));
    assert(vector2i_eq(inter.size, ((struct vector2i){ .x = 10, .y = 10 })));

    struct rect2i c = { .pos = { .x = 12, .y = 12 }, .size = { .x = 5, .y = 5 } };
    inter = rect2i_intersect(a, c);
    assert(!rect2i_is_empty(inter));
    assert(vector2i_eq(inter.pos, c.pos));
    assert(vector2i_eq(inter.size, c.size));

    struct rect2i d = { .pos = { .x = 100, .y = 100 }, .size = { .x = 10, .y = 10 } };
    inter = rect2i_intersect(a, d);
    assert(rect2i_is_empty(inter));

    struct rect2i touch_edge = { .pos = { .x = 30, .y = 10 }, .size = { .x = 20, .y = 20 } };
    inter = rect2i_intersect(a, touch_edge);
    assert(rect2i_is_empty(inter));

    struct rect2i touch_corner = { .pos = { .x = 30, .y = 30 }, .size = { .x = 20, .y = 20 } };
    inter = rect2i_intersect(a, touch_corner);
    assert(rect2i_is_empty(inter));

    struct rect2i neg_a = { .pos = { .x = -50, .y = -50 }, .size = { .x = 40, .y = 40 } };
    struct rect2i neg_b = { .pos = { .x = -20, .y = -20 }, .size = { .x = 40, .y = 40 } };
    inter = rect2i_intersect(neg_a, neg_b);
    assert(!rect2i_is_empty(inter));
    assert(vector2i_eq(inter.pos, ((struct vector2i){ .x = -20, .y = -20 })));
    assert(vector2i_eq(inter.size, ((struct vector2i){ .x = 10, .y = 10 })));
}

static void test_rect2i_clip(void)
{
    bool ok = rect2i_clip(NULL, (struct vector2i){ .x = 100, .y = 100 });
    assert(!ok);

    struct rect2i empty = { .pos = { .x = 10, .y = 10 }, .size = { .x = 0, .y = 20 } };
    ok = rect2i_clip(&empty, (struct vector2i){ .x = 100, .y = 100 });
    assert(!ok);

    struct rect2i r = { .pos = { .x = 10, .y = 10 }, .size = { .x = 50, .y = 50 } };
    ok = rect2i_clip(&r, (struct vector2i){ .x = 0, .y = 100 });
    assert(!ok);
    ok = rect2i_clip(&r, (struct vector2i){ .x = 100, .y = -10 });
    assert(!ok);

    r = (struct rect2i){ .pos = { .x = 10, .y = 10 }, .size = { .x = 20, .y = 20 } };
    ok = rect2i_clip(&r, (struct vector2i){ .x = 100, .y = 100 });
    assert(ok);
    assert(vector2i_eq(r.pos, ((struct vector2i){ .x = 10, .y = 10 })));
    assert(vector2i_eq(r.size, ((struct vector2i){ .x = 20, .y = 20 })));

    r = (struct rect2i){ .pos = { .x = 80, .y = 80 }, .size = { .x = 50, .y = 50 } };
    ok = rect2i_clip(&r, (struct vector2i){ .x = 100, .y = 100 });
    assert(ok);
    assert(vector2i_eq(r.pos, ((struct vector2i){ .x = 80, .y = 80 })));
    assert(vector2i_eq(r.size, ((struct vector2i){ .x = 20, .y = 20 })));

    r = (struct rect2i){ .pos = { .x = -10, .y = -20 }, .size = { .x = 30, .y = 40 } };
    ok = rect2i_clip(&r, (struct vector2i){ .x = 100, .y = 100 });
    assert(ok);
    assert(vector2i_eq(r.pos, ((struct vector2i){ .x = 0, .y = 0 })));
    assert(vector2i_eq(r.size, ((struct vector2i){ .x = 20, .y = 20 })));

    r = (struct rect2i){ .pos = { .x = 150, .y = 150 }, .size = { .x = 50, .y = 50 } };
    ok = rect2i_clip(&r, (struct vector2i){ .x = 100, .y = 100 });
    assert(!ok);
}

static void test_rect2i_inset(void)
{
    struct rect2i r = { .pos = { .x = 10, .y = 20 }, .size = { .x = 100, .y = 80 } };

    struct rect2i shrunk = rect2i_inset(r, 5);
    assert(vector2i_eq(shrunk.pos, ((struct vector2i){ .x = 15, .y = 25 })));
    assert(vector2i_eq(shrunk.size, ((struct vector2i){ .x = 90, .y = 70 })));

    struct rect2i collapsed = rect2i_inset(r, 50);
    assert(vector2i_eq(collapsed.pos, ((struct vector2i){ .x = 60, .y = 70 })));
    assert(collapsed.width == 0 && collapsed.height == 0);

    struct rect2i unch = rect2i_inset(r, 0);
    assert(vector2i_eq(unch.pos, r.pos));
    assert(vector2i_eq(unch.size, r.size));

    struct rect2i expanded = rect2i_inset(r, -5);
    assert(vector2i_eq(expanded.pos, ((struct vector2i){ .x = 5, .y = 15 })));
    assert(vector2i_eq(expanded.size, ((struct vector2i){ .x = 110, .y = 90 })));

    struct rect2i big = { .pos = { .x = 0, .y = 0 }, .size = { .x = INT_MAX - 10, .y = INT_MAX - 10 } };
    struct rect2i big_exp = rect2i_inset(big, -100);
    assert(big_exp.width == INT_MAX && big_exp.height == INT_MAX);

    struct rect2i big_inset = rect2i_inset(r, INT_MAX);
    assert(big_inset.width == 0 && big_inset.height == 0);
}

static void test_insets2i(void)
{
    struct insets2i in = { .top = 1, .right = 2, .bottom = 3, .left = 4 };
    assert(in.t == 1 && in.r == 2 && in.b == 3 && in.l == 4);
    assert(in.v[0] == 1 && in.v[1] == 2 && in.v[2] == 3 && in.v[3] == 4);

    struct insets2i splat = insets2i_splat(5);
    assert(splat.top == 5 && splat.right == 5 && splat.bottom == 5 && splat.left == 5);

    struct insets2i sym = insets2i_sym(10, 20);
    assert(sym.top == 10 && sym.bottom == 10 && sym.left == 20 && sym.right == 20);
    assert(insets2i_eq(sym, (struct insets2i){ .t = 10, .r = 20, .b = 10, .l = 20 }));
    assert(!insets2i_eq(sym, splat));

    struct rect2i r = { .pos = { .x = 10, .y = 20 }, .size = { .x = 100, .y = 80 } };
    struct rect2i applied = rect2i_apply_insets(r, in);
    assert(vector2i_eq(applied.pos, ((struct vector2i){ .x = 14, .y = 21 })));
    assert(vector2i_eq(applied.size, ((struct vector2i){ .x = 94, .y = 76 })));

    struct rect2i huge_in = rect2i_apply_insets(r, insets2i_splat(100));
    assert(huge_in.width == 0 && huge_in.height == 0);
}

static void test_rect2i_union_saturation(void)
{
    /* An endpoint past INT_MAX must saturate. The plain int addition that
     * rect2i_union used to do overflows here, which UBSan reports. */
    struct rect2i far = { .pos = { .x = INT_MAX - 1, .y = 0 },
                          .size = { .x = 10, .y = 1 } };
    struct rect2i origin = { .pos = { .x = 0, .y = 0 },
                             .size = { .x = 1, .y = 1 } };
    struct rect2i u = rect2i_union(far, origin);
    assert(u.x == 0 && u.y == 0);
    assert(u.width == INT_MAX);
    assert(u.height == 1);

    /* An ordinary union is unchanged. */
    struct rect2i n = rect2i_union(
        (struct rect2i){ .pos = { .x = 0, .y = 0 }, .size = { .x = 2, .y = 2 } },
        (struct rect2i){ .pos = { .x = 5, .y = 5 }, .size = { .x = 3, .y = 3 } });
    assert(n.x == 0 && n.y == 0);
    assert(n.width == 8 && n.height == 8);
}

int main(void)
{
    test_vector2i_basic();
    test_vector2i_clamp();
    test_vector2i_saturation();
    test_rect2i_layout_and_zero_init();
    test_rect2i_empty_and_end();
    test_rect2i_contains();
    test_rect2i_from_corners();
    test_rect2i_translate();
    test_rect2i_intersect();
    test_rect2i_clip();
    test_rect2i_inset();
    test_insets2i();
    test_rect2i_union_saturation();

    printf("PASS: geom tests\n");
    return 0;
}
