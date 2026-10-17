#define _POSIX_C_SOURCE 200809L
#include "common/backend.h"

#include <assert.h>
#include <limits.h>

typedef struct yasl_private_rect2i rect;
typedef struct yasl_private_damage_list list;

static bool covers(const list *l, int x, int y)
{
    for (int i = 0; i < l->n; i++) {
        const rect *r = &l->rects[i];
        if (x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height)
            return true;
    }
    return false;
}

static void test_two_presents_keep_both(void)
{
    list l = {0};
    rect a = {0, 0, 10, 10}, b = {100, 100, 10, 10};
    yasl_private_damage_union(&l, &a, 1);
    yasl_private_damage_union(&l, &b, 1);
    assert(l.n == 2);
    assert(covers(&l, 5, 5) && covers(&l, 105, 105));
    assert(!covers(&l, 50, 50));
}

static void test_full_is_sticky(void)
{
    list l = {0};
    rect a = {0, 0, 10, 10};
    yasl_private_damage_union(&l, &a, 1);
    yasl_private_damage_union(&l, NULL, 0);
    assert(l.n == -1);
    yasl_private_damage_union(&l, &a, 1);
    assert(l.n == -1);
    l = (list){0};
    yasl_private_damage_union(&l, &a, 1);
    assert(l.n == 1);
}

static void test_empty_rects_skipped(void)
{
    list l = {0};
    rect empty[] = {{5, 5, 0, 10}, {5, 5, 10, 0}, {5, 5, -1, 3}};
    yasl_private_damage_union(&l, empty, 3);
    assert(l.n == 0);
    rect a = {1, 2, 3, 4};
    yasl_private_damage_union(&l, &a, 1);
    yasl_private_damage_union(&l, empty, 3);
    assert(l.n == 1 && l.rects[0].x == 1 && l.rects[0].width == 3);
}

static void test_overflow_collapses_to_bounds(void)
{
    list l = {0};
    for (int i = 0; i < YASL_PRIVATE_DAMAGE_MAX; i++) {
        rect r = {i * 20, 0, 10, 10};
        yasl_private_damage_union(&l, &r, 1);
    }
    assert(l.n == YASL_PRIVATE_DAMAGE_MAX);
    rect extra = {0, 500, 10, 10};
    yasl_private_damage_union(&l, &extra, 1);
    assert(l.n == 1);
    int right = (YASL_PRIVATE_DAMAGE_MAX - 1) * 20 + 10;
    assert(l.rects[0].x == 0 && l.rects[0].y == 0);
    assert(l.rects[0].width == right && l.rects[0].height == 510);
    /* Later presents keep growing the one rect. */
    rect far = {right + 100, 0, 10, 10};
    yasl_private_damage_union(&l, &far, 1);
    assert(l.n == 1 && l.rects[0].width == right + 110 && l.rects[0].height == 510);
}

static void test_many_rects_in_one_call(void)
{
    list l = {0};
    rect many[YASL_PRIVATE_DAMAGE_MAX + 6];
    for (int i = 0; i < YASL_PRIVATE_DAMAGE_MAX + 6; i++)
        many[i] = (rect){i, i, 1, 1};
    yasl_private_damage_union(&l, many, YASL_PRIVATE_DAMAGE_MAX + 6);
    assert(l.n == 1 && l.rects[0].x == 0 && l.rects[0].width == YASL_PRIVATE_DAMAGE_MAX + 6);
}

static void test_extreme_edges(void)
{
    list l = {0};
    rect a = {INT_MIN, INT_MIN, 10, 10}, b = {INT_MAX - 5, INT_MAX - 5, 5, 5};
    yasl_private_damage_union(&l, &a, 1);
    yasl_private_damage_union(&l, &b, 1);
    assert(l.n == 2 && l.bounds.x == INT_MIN && l.bounds.width == INT_MAX);
}

int main(void)
{
    test_two_presents_keep_both();
    test_full_is_sticky();
    test_empty_rects_skipped();
    test_overflow_collapses_to_bounds();
    test_many_rects_in_one_call();
    test_extreme_edges();
    return 0;
}
