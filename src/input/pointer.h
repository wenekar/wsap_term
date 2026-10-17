#ifndef INPUT_POINTER_H
#define INPUT_POINTER_H

#include <stdbool.h>
#include <stdint.h>

#include "util/geom.h"

enum pointer_button {
    POINTER_BUTTON_PRIMARY = 0,
    POINTER_BUTTON_MIDDLE,
    POINTER_BUTTON_SECONDARY,
    POINTER_BUTTON_NONE,
    POINTER_BUTTON_EXTRA_1,
    POINTER_BUTTON_EXTRA_2,
    POINTER_BUTTON_EXTRA_3,
    POINTER_BUTTON_EXTRA_4,
    POINTER_WHEEL_UP = 16,
    POINTER_WHEEL_DOWN,
    POINTER_WHEEL_LEFT,
    POINTER_WHEEL_RIGHT,
};

enum pointer_kind : uint8_t {
    PTR_NONE = 0,
    PTR_PRESS,
    PTR_RELEASE,
    PTR_MOTION,
    PTR_SCROLL,
    PTR_LEAVE,
};

enum pointer_device : uint8_t {
    POINTER_DEVICE_MOUSE = 0,
    POINTER_DEVICE_TOUCH,
    POINTER_DEVICE_PEN,
    POINTER_DEVICE_ERASER,
};

struct pointer_pen {
    uint16_t pressure;
    int8_t tilt_x, tilt_y;
};

struct pointer_event {
    enum pointer_kind kind;
    struct vector2i pos;
    int button;
    int scroll_lines;
    float scroll_delta;
    unsigned mods;
    enum pointer_device device;
    struct pointer_pen pen;
};

static inline bool pointer_is_button_press(const struct pointer_event *event)
{
    return event->kind == PTR_PRESS && event->button < POINTER_WHEEL_UP;
}

static inline struct pointer_event pointer_event(
        enum pointer_kind kind, struct vector2i pos, int button,
        unsigned mods)
{
    return (struct pointer_event){
        .kind = kind, .pos = pos, .button = button, .mods = mods,
    };
}

static inline bool pointer_wheel_is_horizontal(int button)
{
    return button == POINTER_WHEEL_LEFT || button == POINTER_WHEEL_RIGHT;
}

#endif
