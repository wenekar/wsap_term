/* Exercise native XI2 samples through the backend and public input queue. */
#include "../../src/platform/x11/window.c"

static struct yasl_private_event next(struct x11_surface *surface)
{
    struct yasl_private_event event;
    /* This backend unit harness owns a standalone input queue, without a
     * registered display event FIFO. Read rich private events directly. */
    static unsigned count;
    count++;
    if (!input_queue_pop(&surface->input, &event)) {
        fprintf(stderr, "missing private event at expectation %u (queued=%zu reset=%d)\n",
                count, surface->input.count, surface->input.reset_pending);
        abort();
    }
    assert(event.surface == &surface->base);
    return event;
}

struct drag_test_recorder {
    int over_count;
    int leave_count;
    int drop_count;
    struct yasl_private_vector2i last_pos;
};

struct drop_test_recorder {
    int over_count;
    int leave_count;
    struct yasl_private_vector2i last_pos;
};

static void collect(struct yasl_private_display *display, struct yasl_surface *first,
        struct yasl_surface *second, struct drag_test_recorder *a,
        struct drag_test_recorder *b, struct drop_test_recorder *drop)
{
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(display, &event)) {
        if (event.type == YASL_PRIVATE_EVENT_DRAG) {
            assert(event.surface == first || event.surface == second);
            struct drag_test_recorder *rec = event.surface == first ? a : b;
            rec->last_pos = event.drag.pos;
            if (event.drag.action == YASL_PRIVATE_DRAG_OVER) rec->over_count++;
            else if (event.drag.action == YASL_PRIVATE_DRAG_LEAVE) rec->leave_count++;
            else if (event.drag.action == YASL_PRIVATE_DRAG_DROP) rec->drop_count++;
        } else if (event.type == YASL_PRIVATE_EVENT_DROP_HOVER) {
            assert(event.surface == first);
            drop->last_pos = event.drop.pos;
            if (event.drop.action == YASL_PRIVATE_DROP_OVER) drop->over_count++;
            else if (event.drop.action == YASL_PRIVATE_DROP_LEAVE) drop->leave_count++;
        }
    }
}

static void on_test_drag_end(void *ud, enum yasl_private_drag_end how)
{
    enum yasl_private_drag_end *res = ud;
    *res = how;
}

static void test_x11_gaps(struct x11_display *d, struct x11_surface *s)
{
    /* 1. Caps */
    assert(caps(&d->base) & YASL_PRIVATE_CAP_POPUPS);
    assert(caps(&d->base) & YASL_PRIVATE_CAP_DRAG);
    assert(caps(&d->base) & YASL_PRIVATE_CAP_DRAG_CARRY);

    /* 2. Popups: spec validation, calculation, creation, reposition, destruction */
    struct yasl_private_popup_spec invalid_specs[] = {
        {.size = {0, 100}},
        {.size = {100, 0}},
        {.size = {35000, 100}},
        {.size = {100, 100}, .content_offset = {-1, 0}},
        {.size = {100, 100}, .content_offset = {100, 0}, .content_size = {10, 0}},
        {.size = {100, 100}, .anchor = {INT_MIN, 0, 1, 1}},
    };
    for (size_t i = 0; i < sizeof(invalid_specs)/sizeof(invalid_specs[0]); i++) {
        assert(!popup_spec_valid(s, &invalid_specs[i]));
        assert(!surface_create_popup(&s->base, &invalid_specs[i]));
    }

    struct yasl_private_popup_spec valid_spec = {
        .size = {100, 120},
        .anchor = {30, 40, 50, 60},
        .anchor_edges = 6, .gravity = 8, .constraints = 15,
        .content_offset = {5, 10},
    };
    assert(popup_spec_valid(s, &valid_spec));
    int x = 0, y = 0;
    struct yasl_private_vector2i actual_size;
    popup_calculate_position(d, s, &valid_spec, &x, &y, &actual_size);
    assert(x == 30 - 5);
    assert(y == 40 + 60 - 10);

    /* Explicit XDG-style anchor/gravity stays exact unless a constraint allows a fit. */
    struct yasl_private_popup_spec positioned = {

        .anchor_edges = 2, /* bottom */
        .gravity = 2, /* below */
        .size = {100, 120}, .anchor = {100, 100, 40, 20},
        .content_offset = {5, 10},
    };
    popup_calculate_position(d, s, &positioned, &x, &y, &actual_size);
    assert(x == 68 && y == 110 && actual_size.x == 100 && actual_size.y == 120);
    positioned.anchor_edges = 4; /* right edge */
    positioned.gravity = 4; /* to the right */
    positioned.anchor = (struct yasl_private_rect2i){1900, 100, 20, 20};
    positioned.constraints = 4; /* flip X */
    popup_calculate_position(d, s, &positioned, &x, &y, &actual_size);
    assert(x == 1800 && actual_size.x == 100);
    positioned.gravity = 0;
    positioned.constraints = 1; /* slide X */
    popup_calculate_position(d, s, &positioned, &x, &y, &actual_size);
    assert(x + actual_size.x == 1920);
    positioned.content_offset = (struct yasl_private_vector2i){0, 0};
    positioned.gravity = 4;
    positioned.constraints = 16; /* resize X */
    positioned.anchor = (struct yasl_private_rect2i){1880, 100, 20, 20};
    popup_calculate_position(d, s, &positioned, &x, &y, &actual_size);
    assert(x == 1920 - actual_size.x && actual_size.x == 20);

    valid_spec.anchor_edges = 7;
    popup_calculate_position(d, s, &valid_spec, &x, &y, &actual_size);
    assert(x == 30 + 50 - 5);
    assert(y == 40 - 10);

    struct yasl_surface *popup_base = surface_create_popup(&s->base, &valid_spec);
    assert(popup_base != NULL);
    struct x11_surface *popup = (struct x11_surface *)popup_base;
    assert(popup->is_popup);
    assert(popup->parent == s);
    assert(popup->size.x == 100 && popup->size.y == 120);

    struct yasl_private_popup_spec repo_spec = {
        .size = {150, 180},
        .anchor = {10, 20, 30, 40},
        .anchor_edges = 6, .gravity = 8, .constraints = 15,
        .content_offset = {0, 0},
    };
    assert(surface_popup_reposition(popup_base, &repo_spec));
    assert(popup->size.x == 150 && popup->size.y == 180);

    surface_destroy(popup_base);
    assert(d->surfaces == s);

    /* 3. Drag-and-drop target events */
    struct x11_surface s2 = {.base.display = &d->base, .window = 43};
    s->next = &s2;
    s2.next = NULL;

    struct drag_test_recorder rec1 = {0};
    struct drag_test_recorder rec2 = {0};
    struct drop_test_recorder drec = {0};
    assert(yasl_private_events_open(&d->base));
    assert(yasl_private_events_register(&s->base));
    assert(yasl_private_events_register(&s2.base));

    enum yasl_private_drag_end end_reason = (enum yasl_private_drag_end)-1;
    struct yasl_private_drag_listener l = {.end = on_test_drag_end};
    assert(drag_start(&d->base, &s->base, NULL, &l, &end_reason));
    assert(d->drag.active);

    /* Motion on s (window 42) -> YASL_PRIVATE_DRAG_OVER on s */
    XIDeviceEvent mev = {.evtype = XI_Motion, .sourceid = 8, .event = 42, .event_x = 12.0, .event_y = 34.0};
    XGenericEventCookie cookie = {.evtype = XI_Motion, .data = &mev};
    xi_event(d, &cookie);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(rec1.over_count == 1);
    assert(rec1.last_pos.x == 12 && rec1.last_pos.y == 34);

    /* Motion on s2 (window 43) -> YASL_PRIVATE_DRAG_LEAVE on s, YASL_PRIVATE_DRAG_OVER on s2 */
    mev.event = 43;
    mev.event_x = 56.0;
    mev.event_y = 78.0;
    xi_event(d, &cookie);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(rec1.leave_count == 1);
    assert(rec2.over_count == 1);
    assert(rec2.last_pos.x == 56 && rec2.last_pos.y == 78);

    /* ButtonRelease -> YASL_PRIVATE_DRAG_DROP on s2 and drag end */
    XIDeviceEvent rel = {.evtype = XI_ButtonRelease, .sourceid = 8, .event = 43, .detail = 1};
    cookie = (XGenericEventCookie){.evtype = XI_ButtonRelease, .data = &rel};
    xi_event(d, &cookie);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(rec2.drop_count == 1);
    assert(end_reason == YASL_PRIVATE_DRAG_DROPPED);
    assert(!d->drag.active);

    /* Drag cancel */
    assert(drag_start(&d->base, &s->base, NULL, &l, &end_reason));
    cookie = (XGenericEventCookie){.evtype = XI_Motion, .data = &mev};
    mev.event = 42;
    xi_event(d, &cookie);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(rec1.over_count == 2);
    drag_cancel(&d->base);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(rec1.leave_count == 2);
    assert(end_reason == YASL_PRIVATE_DRAG_CANCELLED);
    assert(!d->drag.active);

    /* Hierarchy change cancels drag and pushes reset */
    input_queue_clear(&s->input);
    assert(drag_start(&d->base, &s->base, NULL, &l, &end_reason));
    XIHierarchyEvent hiev = {.evtype = XI_HierarchyChanged, .flags = XISlaveRemoved};
    cookie = (XGenericEventCookie){.evtype = XI_HierarchyChanged, .data = &hiev};
    xi_event(d, &cookie);
    assert(end_reason == YASL_PRIVATE_DRAG_CANCELLED);
    assert(!d->drag.active);
    struct yasl_private_event reset_ev;
    /* The display stream also carries the reset of the second surface. */
    do { assert(yasl_private_display_poll_event(&d->base, &reset_ev)); }
    while (reset_ev.type != YASL_PRIVATE_EVENT_INPUT_RESET || reset_ev.surface != &s->base);
    assert(reset_ev.type == YASL_PRIVATE_EVENT_INPUT_RESET);

    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);

    /* 4. Drag image parameter */
    uint32_t pixels[16] = {0xFF00FF00};
    struct yasl_private_drag_image img = {.pixels = pixels, .size = {4, 4}, .hotspot = {2, 2}};
    assert(drag_start(&d->base, &s->base, &img, &l, &end_reason));
    assert(d->drag.active);
    assert(drag_carry(&s->base, (struct yasl_private_vector2i){10, 10}));
    drag_cancel(&d->base);

    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);

    /* 5. XDND drop-target events and URI parsing */
    size_t out_len = 0;
    char *parsed = parse_file_uris("file:///tmp/doc.txt\r\n", 21, &out_len);
    assert(parsed && !strcmp(parsed, "file:///tmp/doc.txt\r\n") && out_len == 21);
    free(parsed);

    parsed = parse_file_uris("# comment\nfile:///a\nfile:///b\n", 30, &out_len);
    assert(parsed && !strcmp(parsed, "file:///a\r\nfile:///b\r\n") && out_len == 22);
    free(parsed);

    parsed = parse_file_uris("http://example.com\r\n", 20, &out_len);
    assert(parsed == NULL && out_len == 0);

    /* Test XDND ClientMessages */
    d->protocols = 1;
    d->delete_window = 2;
    d->xdnd_enter = 10;
    d->xdnd_position = 11;
    d->xdnd_leave = 12;
    d->xdnd_drop = 13;
    d->uri_list = 20;


    XClientMessageEvent enter_msg = {
        .type = ClientMessage,
        .message_type = d->xdnd_enter,
        .format = 32,
        .data.l = {99, 5 << 24, d->uri_list, 0, 0},
    };
    handle_client_message(d, s, &enter_msg);
    assert(d->xdnd.accepts);

    XClientMessageEvent pos_msg = {
        .type = ClientMessage,
        .message_type = d->xdnd_position,
        .format = 32,
        .data.l = {99, 0, (100 << 16) | 200, 1000, 0},
    };
    handle_client_message(d, s, &pos_msg);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(drec.over_count == 1);
    assert(drec.last_pos.x == 100 && drec.last_pos.y == 200);

    XClientMessageEvent leave_msg = {
        .type = ClientMessage,
        .message_type = d->xdnd_leave,
        .format = 32,
        .data.l = {99, 0, 0, 0, 0},
    };
    handle_client_message(d, s, &leave_msg);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(drec.leave_count == 1);

    handle_client_message(d, s, &enter_msg);
    handle_client_message(d, s, &pos_msg);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(drec.over_count == 2);

    XClientMessageEvent drop_msg = {
        .type = ClientMessage,
        .message_type = d->xdnd_drop,
        .format = 32,
        .data.l = {99, 0, 1005, 0, 0},
    };
    handle_client_message(d, s, &drop_msg);
    collect(&d->base, &s->base, &s2.base, &rec1, &rec2, &drec);
    assert(drec.leave_count == 2);
    assert(d->pending_drop.active);

    /* Push YASL_PRIVATE_EVENT_DROP */
    push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_DROP}, "file:///sample.png\r\n", 20);
    struct yasl_private_event drop_ev;
    assert(yasl_private_display_poll_event(&d->base, &drop_ev));
    assert(drop_ev.type == YASL_PRIVATE_EVENT_DROP);
    assert(drop_ev.text.len == 20);
    assert(!strcmp(drop_ev.text.data, "file:///sample.png\r\n"));

    yasl_private_events_unregister(&s->base);
    yasl_private_events_unregister(&s2.base);
    yasl_private_events_close(&d->base);

    /* Clean up s2 */
    s->next = NULL;
    input_queue_clear(&s->input);
    input_queue_clear(&s2.input);
}

int main(void)
{
    /* External GPU surfaces retain geometry without allocating CPU targets. */
    struct x11_surface external = {0};
    external.base.presenter = YASL_PRIVATE_PRESENTER_NONE;
    assert(resize(&external, (struct yasl_private_vector2i){640, 480}));
    assert(external.size.x == 640 && external.size.y == 480 && !external.image && !external.pixels);

    struct x11_display display = {
        .base.ops = &yasl_private_x11_backend, .abs_pressure = 10, .abs_tilt_x = 11, .abs_tilt_y = 12,
    };
    struct x11_surface surface = {.base = {.display = &display.base,
        .creation_flags = YASL_PRIVATE_SURFACE_CPU,
        .presenter = YASL_PRIVATE_PRESENTER_CPU}, .window = 42};
    display.surfaces = &surface;
    XIValuatorClassInfo pressure = {.type = XIValuatorClass, .number = 2,
        .label = 10, .min = 100, .max = 1100, .mode = XIModeAbsolute};
    XIValuatorClassInfo tilt_x = {.type = XIValuatorClass, .number = 4,
        .label = 11, .min = -64, .max = 63, .mode = XIModeAbsolute};
    XIValuatorClassInfo tilt_y = {.type = XIValuatorClass, .number = 7,
        .label = 12, .min = -64, .max = 63, .mode = XIModeAbsolute};
    XIAnyClassInfo *classes[] = {(void *)&pressure, (void *)&tilt_x, (void *)&tilt_y};
    XIDeviceInfo info = {.deviceid = 8, .name = "Tablet pen", .num_classes = 3, .classes = classes};
    struct x11_pointer pointer = {0};
    pointer_info(&display, &pointer, &info);
    assert(pointer.device == YASL_PRIVATE_PTR_DEV_PEN);
    display.pointers = &pointer; /* Cache avoids requiring a real X server. */

    unsigned char mask[1] = {0};
    XISetMask(mask, 2);
    XISetMask(mask, 4);
    XISetMask(mask, 7);
    double values[] = {600, -35, 42};
    XIDeviceEvent sample = {.evtype = XI_ButtonPress, .sourceid = 8, .event = 42,
        .detail = 1, .event_x = 200.5, .event_y = 175.25,
        .mods.effective = ShiftMask, .valuators = {sizeof(mask), mask, values}};
    XGenericEventCookie cookie = {.evtype = XI_ButtonPress, .data = &sample};
    xi_event(&display, &cookie);
    struct yasl_private_event ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_PEN && ev.pen.device_id == 8 && !ev.pen.tool_id);
    assert(ev.pen.tool == 1 && ev.pen.action == YASL_PRIVATE_PEN_DOWN && ev.pen.in_contact);
    assert((ev.pen.axes_valid & (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT)) ==
           (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT));
    assert(ev.pen.axes_changed == (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT));
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_DOWN && ev.pointer.button == YASL_PRIVATE_BUTTON_PRIMARY && ev.pointer.device == YASL_PRIVATE_PTR_DEV_PEN);
    assert(ev.pointer.pos.x == 201 && ev.pointer.pos.y == 175 && ev.pointer.mods == YASL_PRIVATE_MOD_SHIFT);
    assert(ev.pointer.pen.pressure == 32768 && ev.pointer.pen.tilt_x == -35 && ev.pointer.pen.tilt_y == 42);

    /* XI2 sends only changed axes, with values packed in mask order. */
    mask[0] = 0;
    XISetMask(mask, 2);
    values[0] = 1100;
    sample.evtype = cookie.evtype = XI_Motion;
    xi_event(&display, &cookie);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_PEN && ev.pen.action == YASL_PRIVATE_PEN_MOVE &&
           ev.pen.axes_valid == (YASL_PRIVATE_PEN_PRESSURE | YASL_PRIVATE_PEN_TILT) &&
           ev.pen.axes_changed == YASL_PRIVATE_PEN_PRESSURE && ev.pen.pressure == 1.0);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_MOTION && ev.pointer.pen.pressure == 65535);
    assert(ev.pointer.pen.tilt_x == -35 && ev.pointer.pen.tilt_y == 42);
    values[0] = 5000;
    xi_event(&display, &cookie);
    assert(next(&surface).type == YASL_PRIVATE_EVENT_PEN);
    assert(next(&surface).pointer.pen.pressure == 65535);
    values[0] = -50;
    xi_event(&display, &cookie);
    assert(next(&surface).type == YASL_PRIVATE_EVENT_PEN);
    assert(next(&surface).pointer.pen.pressure == 0);

    info.name = "Tablet ERASER";
    pointer_info(&display, &pointer, &info);
    assert(pointer.device == YASL_PRIVATE_PTR_DEV_ERASER);
    values[0] = 900;
    sample.evtype = cookie.evtype = XI_ButtonPress;
    xi_event(&display, &cookie);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_PEN && ev.pen.tool == 2 && ev.pen.action == YASL_PRIVATE_PEN_DOWN);
    ev = next(&surface);
    assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER && ev.pointer.pen.pressure > 0);
    /* Release must clear pressure even when the driver omits that valuator. */
    sample.valuators.mask_len = 0;
    sample.evtype = cookie.evtype = XI_ButtonRelease;
    xi_event(&display, &cookie);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_PEN && ev.pen.action == YASL_PRIVATE_PEN_UP && !ev.pen.in_contact);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_UP && ev.pointer.button == YASL_PRIVATE_BUTTON_PRIMARY && ev.pointer.pen.pressure == 0);
    assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER);

    XIEnterEvent crossing = {.sourceid = 8, .event = 42, .event_x = 200, .event_y = 175};
    cookie = (XGenericEventCookie){.evtype = XI_Leave, .data = &crossing};
    xi_event(&display, &cookie);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_PEN && ev.pen.action == YASL_PRIVATE_PEN_LEAVE);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_LEAVE && ev.pointer.device == YASL_PRIVATE_PTR_DEV_ERASER && !ev.pointer.pen.pressure);

    /* No tablet axes must produce an ordinary, pressure-free mouse. */
    info.num_classes = 0;
    pointer_info(&display, &pointer, &info);
    sample.evtype = XI_ButtonPress;
    sample.detail = 3;
    xi_pointer_event(&surface, &sample);
    ev = next(&surface);
    assert(ev.pointer.device == YASL_PRIVATE_PTR_DEV_MOUSE && ev.pointer.button == YASL_PRIVATE_BUTTON_SECONDARY && ev.pointer.pen.pressure == 0);
    for (int button = 4; button <= 7; button++) {
        sample.detail = button;
        sample.evtype = XI_ButtonPress;
        xi_pointer_event(&surface, &sample);
        ev = next(&surface);
        assert(ev.type == YASL_PRIVATE_EVENT_POINTER_SCROLL);
        assert(ev.pointer.scroll.x == (button == 6 ? -1 : button == 7 ? 1 : 0));
        assert(ev.pointer.scroll.y == (button == 4 ? -1 : button == 5 ? 1 : 0));
        assert(ev.pointer.scroll_valid == 1u && ev.pointer.scroll_source == 1u && ev.pointer.scroll_phase == 2u);
        sample.evtype = XI_ButtonRelease;
        xi_pointer_event(&surface, &sample);
        struct yasl_private_event unused;
        assert(!input_queue_pop(&surface.input, &unused));
    }
    /* XI2 smooth-scroll valuators preserve fractional line data and source. */
    XIValuatorClassInfo scroll_valuator = {.type = XIValuatorClass, .number = 9,
        .value = 0, .mode = XIModeRelative};
    XIScrollClassInfo scroll_class = {.type = XIScrollClass, .number = 9,
        .scroll_type = XIScrollTypeVertical, .increment = 2.0};
    XIAnyClassInfo *scroll_classes[] = {(void *)&scroll_valuator, (void *)&scroll_class};
    info.name = "Test touchpad";
    info.num_classes = 2;
    info.classes = scroll_classes;
    struct x11_pointer touchpad = {0};
    pointer_info(&display, &touchpad, &info);
    display.pointers = &touchpad;
    unsigned char scroll_mask[2] = {0};
    XISetMask(scroll_mask, 9);
    double scroll_value[] = {-1.0};
    sample.evtype = XI_Motion;
    sample.valuators = (XIValuatorState){sizeof(scroll_mask), scroll_mask, scroll_value};
    xi_pointer_event(&surface, &sample);
    assert(next(&surface).type == YASL_PRIVATE_EVENT_POINTER_MOTION);
    ev = next(&surface);
    assert(ev.type == YASL_PRIVATE_EVENT_POINTER_SCROLL && ev.pointer.scroll_valid == 1u &&
           ev.pointer.scroll_source == 2u && ev.pointer.scroll.y == -0.5);
    input_queue_clear(&surface.input);
    assert(pointer_button(2) == 1 && pointer_button(8) == 3);
    pointer.pressure = (struct tablet_axis){2, 1, 1, 50, true};
    assert(pressure_value(&pointer.pressure) == 0);
    pointer.tilt_x = (struct tablet_axis){4, -90, 90, -200, true};
    assert(tilt_value(&pointer.tilt_x) == -90);
    pointer.tilt_x.value = 200;
    assert(tilt_value(&pointer.tilt_x) == 90);
    display.pointers = NULL;
    input_queue_clear(&surface.input);
    test_x11_gaps(&display, &surface);
    return 0;
}
