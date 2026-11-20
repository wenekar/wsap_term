#include "ui/anchor.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#define PT(ax, ay) ((struct vector2i){ .x = (ax), .y = (ay) })
#define EXT(ax, ay, aw, ah) \
    ((struct rect2i){ .pos = { .x = (ax), .y = (ay) }, \
                      .size = { .x = (aw), .y = (ah) } })

static void test_below_with_room(void)
{
    struct rect2i r = ui_anchor_place_below(
        PT(10, 20), PT(40, 12), 16, 20, EXT(0, 0, 200, 200));
    assert(r.pos.x == 10 && r.pos.y == 36);
    assert(r.size.x == 40 && r.size.y == 12);
}

static void test_below_clamps_cross_axis(void)
{
    struct rect2i r = ui_anchor_place_below(
        PT(195, 10), PT(40, 12), 16, 10, EXT(0, 0, 200, 200));
    assert(r.pos.x == 160 && r.pos.y == 26);

    r = ui_anchor_place_below(
        PT(30, 10), PT(80, 12), 16, 10, EXT(5, 0, 50, 200));
    assert(r.pos.x == 5);

    r = ui_anchor_place_below(
        PT(2, 10), PT(40, 12), 16, 10, EXT(5, 0, 200, 200));
    assert(r.pos.x == 5);
}

static void test_below_flips_above(void)
{
    struct rect2i r = ui_anchor_place_below(
        PT(10, 190), PT(40, 12), 16, 190, EXT(0, 0, 200, 200));
    assert(r.pos.y == 178);
    assert(rect2i_end(r).y == 190);

    r = ui_anchor_place_below(
        PT(10, 8), PT(40, 30), 16, 8, EXT(0, 0, 100, 20));
    assert(r.pos.y == 0);

    r = ui_anchor_place_below(
        PT(10, 250), PT(40, 12), 16, 250, EXT(0, 0, 200, 200));
    assert(r.pos.y == 238);
}

static void test_below_empty_extent(void)
{
    struct rect2i r = ui_anchor_place_below(
        PT(500, 500), PT(40, 12), 16, 500, EXT(0, 0, 0, 0));
    assert(r.pos.x == 500 && r.pos.y == 516);

    r = ui_anchor_place_below(
        PT(10, INT_MAX), PT(40, 12), 16, INT_MAX, EXT(0, 0, 0, 0));
    assert(r.pos.y == INT_MAX);
}

static void test_right_with_room(void)
{
    struct rect2i r = ui_anchor_place_right(
        PT(50, 10), PT(30, 20), 0, 50, PT(200, 100));
    assert(r.pos.x == 50 && r.pos.y == 10);

    r = ui_anchor_place_right(
        PT(50, 10), PT(30, 20), 4, -1, PT(200, 100));
    assert(r.pos.x == 54);
}

static void test_right_flips_left(void)
{
    struct rect2i r = ui_anchor_place_right(
        PT(190, 10), PT(60, 20), 0, 180, PT(200, 100));
    assert(r.pos.x == 120);
    assert(rect2i_end(r).x == 180);

    r = ui_anchor_place_right(
        PT(190, 10), PT(60, 20), 0, 30, PT(200, 100));
    assert(r.pos.x == 0);
}

static void test_right_flip_disabled(void)
{
    struct rect2i r = ui_anchor_place_right(
        PT(190, 10), PT(60, 20), 0, -1, PT(200, 100));
    assert(r.pos.x == 140);
}

static void test_right_clamps_axes(void)
{
    struct rect2i r = ui_anchor_place_right(
        PT(50, 90), PT(30, 20), 0, -1, PT(200, 100));
    assert(r.pos.x == 50 && r.pos.y == 80);

    r = ui_anchor_place_right(
        PT(-10, -5), PT(30, 20), 0, -1, PT(200, 100));
    assert(r.pos.x == 0 && r.pos.y == 0);
}

static void test_right_degenerate_extent(void)
{
    struct rect2i r = ui_anchor_place_right(
        PT(50, 50), PT(30, 20), 0, -1, PT(0, 0));
    assert(r.pos.x == 0 && r.pos.y == 0);

    r = ui_anchor_place_right(
        PT(50, 50), PT(30, 20), 0, 40, PT(0, 0));
    assert(r.pos.x == 0 && r.pos.y == 0);

    r = ui_anchor_place_right(
        PT(50, 50), PT(30, 20), 0, -1, PT(200, 0));
    assert(r.pos.x == 50 && r.pos.y == 0);
}

int main(void)
{
    test_below_with_room();
    test_below_clamps_cross_axis();
    test_below_flips_above();
    test_below_empty_extent();
    test_right_with_room();
    test_right_flips_left();
    test_right_flip_disabled();
    test_right_clamps_axes();
    test_right_degenerate_extent();

    printf("PASS: anchor tests\n");
    return 0;
}
