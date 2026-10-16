#ifndef WSAP_UI_ANIM_H
#define WSAP_UI_ANIM_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "util/geom.h"

enum ui_ease : uint8_t {
    UI_EASE_LINEAR = 0,
    UI_EASE_IN_QUAD,
    UI_EASE_OUT_QUAD,
    UI_EASE_OUT_CUBIC,
    UI_EASE_IN_OUT_CUBIC,
    UI_EASE_OUT_EXPO,
    UI_EASE_OUT_BACK,
};

struct ui_tween {
    float from;
    float to;
    uint64_t start_ms;
    uint32_t duration_ms;
    enum ui_ease ease;
};

static inline float ui_ease_eval(enum ui_ease ease, float t)
{
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    switch (ease) {
    case UI_EASE_IN_QUAD:      return t * t;
    case UI_EASE_OUT_QUAD:     return t * (2.0f - t);
    case UI_EASE_OUT_CUBIC:    { float f = t - 1.0f; return f * f * f + 1.0f; }
    case UI_EASE_IN_OUT_CUBIC: return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    case UI_EASE_OUT_EXPO:     return t >= 1.0f ? 1.0f : 1.0f - exp2f(-10.0f * t);
    case UI_EASE_OUT_BACK:     { float f = t - 1.0f; return 1.0f + 2.70158f * f * f * f + 1.70158f * f * f; }
    case UI_EASE_LINEAR:
    default:                   return t;
    }
}

static inline float ui_tween_eval(struct ui_tween t, uint64_t now_ms, bool *finished)
{
    if (t.duration_ms == 0 || now_ms >= t.start_ms + t.duration_ms) {
        if (finished) *finished = true;
        return t.to;
    }
    if (now_ms <= t.start_ms) {
        if (finished) *finished = false;
        return t.from;
    }
    if (finished) *finished = false;
    float p = (float)(now_ms - t.start_ms) / (float)t.duration_ms;
    return t.from + (t.to - t.from) * ui_ease_eval(t.ease, p);
}

struct ui_hover_anim {
    float value;
    float from;
    float to;
    uint64_t start_ms;
};

static inline void ui_hover_anim_set_target(struct ui_hover_anim *a,
                                            float target, uint64_t now_ms)
{
    if (!a || !isfinite(target) || a->to == target) return;
    a->from = a->value;
    a->to = target;
    a->start_ms = now_ms;
}

static inline bool ui_hover_anim_tick(struct ui_hover_anim *a,
                                      uint64_t now_ms,
                                      uint32_t duration_ms,
                                      enum ui_ease ease)
{
    if (!a || a->value == a->to)
        return false;
    if (!a->start_ms || !duration_ms) {
        a->value = a->to;
        return false;
    }
    bool finished = false;
    a->value = ui_tween_eval(
        (struct ui_tween){ .from = a->from, .to = a->to,
                           .start_ms = a->start_ms,
                           .duration_ms = duration_ms,
                           .ease = ease },
        now_ms, &finished);
    return !finished;
}

static inline float ui_hover_anim_value(const struct ui_hover_anim *a)
{
    return a ? a->value : 0.0f;
}

static inline uint32_t ui_color_lerp(uint32_t c1, uint32_t c2, float t)
{
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    uint32_t a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint32_t a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;

    uint32_t a = (uint32_t)((float)a1 + (float)((int)a2 - (int)a1) * t + 0.5f);
    uint32_t r = (uint32_t)((float)r1 + (float)((int)r2 - (int)r1) * t + 0.5f);
    uint32_t g = (uint32_t)((float)g1 + (float)((int)g2 - (int)g1) * t + 0.5f);
    uint32_t b = (uint32_t)((float)b1 + (float)((int)b2 - (int)b1) * t + 0.5f);

    return (a << 24) | (r << 16) | (g << 8) | b;
}

struct ui_ripple {
    struct vector2i origin;
    uint64_t start_ms;
    uint32_t duration_ms;
    int max_radius;
    uint32_t color;
    bool active;
};

static inline void ui_ripple_start(struct ui_ripple *r, struct rect2i bounds,
                                  struct vector2i click_pos, uint32_t color,
                                  uint64_t now_ms, uint32_t duration_ms)
{
    if (!r) return;
    r->origin = click_pos;
    r->start_ms = now_ms;
    r->duration_ms = duration_ms > 0 ? duration_ms : 300;
    r->color = color;
    r->active = true;

    struct vector2i end = rect2i_end(bounds);
    int dx = (click_pos.x - bounds.x) > (end.x - click_pos.x)
           ? (click_pos.x - bounds.x) : (end.x - click_pos.x);
    int dy = (click_pos.y - bounds.y) > (end.y - click_pos.y)
           ? (click_pos.y - bounds.y) : (end.y - click_pos.y);
    double d2 = (double)dx * dx + (double)dy * dy;
    r->max_radius = d2 > 0.0 ? (int)ceil(sqrt(d2)) + 4 : 4;
}

static inline bool ui_ripple_eval(const struct ui_ripple *r, uint64_t now_ms,
                                  int *radius_out, uint8_t *alpha_out)
{
    if (!r || !r->active)
        return false;
    if (now_ms < r->start_ms) {
        if (radius_out) *radius_out = 0;
        if (alpha_out) *alpha_out = (uint8_t)(r->color >> 24);
        return true;
    }
    if (now_ms >= r->start_ms + r->duration_ms) {
        return false;
    }
    float p = (float)(now_ms - r->start_ms) / (float)r->duration_ms;
    float rad_p = ui_ease_eval(UI_EASE_OUT_CUBIC, p);
    float alpha_p = 1.0f - ui_ease_eval(UI_EASE_OUT_QUAD, p);

    if (radius_out)
        *radius_out = (int)((float)r->max_radius * rad_p);
    uint8_t base_alpha = (uint8_t)(r->color >> 24);
    if (alpha_out)
        *alpha_out = (uint8_t)((float)base_alpha * alpha_p + 0.5f);
    return true;
}

#endif
