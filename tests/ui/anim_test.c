#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui/anim.h"

static void test_easing_curves(void)
{
    for (int e = UI_EASE_LINEAR; e <= UI_EASE_OUT_EXPO; e++) {
        enum ui_ease ease = (enum ui_ease)e;
        assert(fabsf(ui_ease_eval(ease, 0.0f) - 0.0f) < 0.001f);
        assert(fabsf(ui_ease_eval(ease, 1.0f) - 1.0f) < 0.001f);
        assert(ui_ease_eval(ease, -0.5f) == 0.0f);
        assert(ui_ease_eval(ease, 1.5f) == 1.0f);
    }

    assert(ui_ease_eval(UI_EASE_OUT_CUBIC, 0.5f) > 0.7f);
    assert(ui_ease_eval(UI_EASE_IN_QUAD, 0.5f) == 0.25f);
}

static void test_tweens(void)
{
    struct ui_tween t = {
        .from = 10.0f,
        .to = 100.0f,
        .start_ms = 1000,
        .duration_ms = 500,
        .ease = UI_EASE_LINEAR,
    };

    bool finished = false;
    assert(ui_tween_eval(t, 900, &finished) == 10.0f && !finished);
    assert(ui_tween_eval(t, 1000, &finished) == 10.0f && !finished);
    assert(fabsf(ui_tween_eval(t, 1250, &finished) - 55.0f) < 0.001f && !finished);
    assert(ui_tween_eval(t, 1500, &finished) == 100.0f && finished);
    assert(ui_tween_eval(t, 2000, &finished) == 100.0f && finished);

    struct ui_tween instant = { .from = 0.0f, .to = 50.0f, .start_ms = 1000, .duration_ms = 0 };
    assert(ui_tween_eval(instant, 1000, &finished) == 50.0f && finished);
    assert(ui_tween_eval(instant, 1001, &finished) == 50.0f && finished);
}

static void test_color_lerp(void)
{
    uint32_t black = 0xFF000000;
    uint32_t white = 0xFFFFFFFF;
    assert(ui_color_lerp(black, white, 0.0f) == black);
    assert(ui_color_lerp(black, white, 1.0f) == white);

    uint32_t mid = ui_color_lerp(black, white, 0.5f);
    assert(((mid >> 16) & 0xFF) >= 127 && ((mid >> 16) & 0xFF) <= 128);
    assert(((mid >> 8) & 0xFF) >= 127 && ((mid >> 8) & 0xFF) <= 128);
    assert((mid & 0xFF) >= 127 && (mid & 0xFF) <= 128);
}

static void test_ripple_math(void)
{
    struct rect2i btn = { .pos = { .x = 10, .y = 20 }, .size = { .x = 100, .y = 40 } };
    struct ui_ripple r = {0};

    struct vector2i click = { .x = 20, .y = 30 };
    ui_ripple_start(&r, btn, click, 0x80FFFFFFu, 1000, 300);
    assert(r.active);
    assert(r.max_radius > 80);

    int radius = 0;
    uint8_t alpha = 0;

    assert(ui_ripple_eval(&r, 900, &radius, &alpha));
    assert(radius == 0 && alpha == 0x80);

    assert(ui_ripple_eval(&r, 1150, &radius, &alpha));
    assert(radius > 0 && radius < r.max_radius);
    assert(alpha > 0 && alpha < 0x80);

    assert(!ui_ripple_eval(&r, 1300, &radius, &alpha));
}


int main(void)
{
    test_easing_curves();
    test_tweens();
    test_color_lerp();
    test_ripple_math();

    printf("PASS: anim tests\n");
    return 0;
}
