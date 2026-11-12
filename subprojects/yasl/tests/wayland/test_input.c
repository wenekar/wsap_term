#include "platform/wayland/internal.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

static struct yasl_private_event next_event(struct yasl_private_display *display,
                                          enum yasl_private_event_type type)
{
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(display, &event))
        if (event.type == type)
            return event;
    assert(!"no matching event");
    return (struct yasl_private_event){0};
}
#include <unistd.h>

static void test_ime_capability(void)
{
    struct wayland_display display = {0};
    struct wayland_surface w = {.conn = &display};
    assert(!(wayland_display_caps(&display) & YASL_PRIVATE_CAP_TEXT_INPUT));
    errno = 0;
    assert(!wayland_surface_update_text_input(&w) && errno == ENOTSUP);
    display.text_input_mgr = (struct zwp_text_input_manager_v3 *)&display;
    assert(wayland_display_caps(&display) & YASL_PRIVATE_CAP_TEXT_INPUT);
    w.base.text_input_active = true;
    w.base.text_purpose = 1; /* Public YASL_TEXT_PASSWORD. */
    /* A supported compositor can accept the requested state before a seat
     * or keyboard focus has arrived. The enter callback applies it later. */
    assert(wayland_surface_update_text_input(&w));
}

static void test_popup_bounds(void)
{
    struct seat seat = {.input_serial = 1};
    struct wayland_display d = {.base.ops = &yasl_private_wayland_backend, .seats = &seat};
    struct wayland_surface w = {.base.display = &d.base, .conn = &d};
    w.parent = &w;
    w.content.x = w.content.y = 1;
    /* Invalid bounds must be rejected before accessing any protocol object. */
    w.xdg_surface = (struct xdg_surface *)&w;
    w.popup = (struct xdg_popup *)&w;
    struct yasl_private_popup_spec specs[] = {
        {.size = {100, 100}, .content_offset = {INT_MAX, 0}, .content_size = {1, 0}},
        {.size = {100, 100}, .content_offset = {0, INT_MAX}, .content_size = {0, 1}},
        {.size = {100, 100}, .content_offset = {1, 0}, .content_size = {INT_MAX, 0}},
        {.size = {100, 100}, .content_offset = {0, 1}, .content_size = {0, INT_MAX}},
        {.size = {100, 100}, .anchor = {INT_MIN, 0, 1, 1}},
        {.size = {100, 100}, .anchor = {0, INT_MIN, 1, 1}},
    };
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
        assert(!yasl_private_surface_create_popup(&w.base, &specs[i]));
        assert(!yasl_private_surface_popup_reposition(&w.base, &specs[i]));
    }
}

int main(void)
{
    test_ime_capability();
    test_popup_bounds();
    struct wayland_surface w = {0};
    struct seat seat = {.ime_focus = &w};
    w.base.text_input_active = true;
    wayland_surface_set_callbacks(&w, NULL, NULL);
    for (int i = 0; i < 600; i++) {
        char text[32];
        snprintf(text, sizeof(text), "commit-%d", i);
        text_input_listener.commit_string(&seat, NULL, text);
        text_input_listener.done(&seat, NULL, (uint32_t)i);
        assert(!w.ime_commit);
    }
    for (int i = 0; i < 600; i++) {
        char text[32];
        snprintf(text, sizeof(text), "commit-%d", i);
        struct yasl_private_event event;
        assert(input_queue_pop(&w.input, &event));
        assert(event.type == YASL_PRIVATE_EVENT_TEXT && event.surface == &w.base);
        assert(strcmp(event.text.data, text) == 0);
    }
    text_input_listener.preedit_string(&seat, NULL, "abcd", 3, 1);
    text_input_listener.done(&seat, NULL, 600);
    int begin, end;
    assert(wayland_surface_ime_preedit_cursor(&w, &begin, &end));
    assert(begin == 1 && end == 3);
    ime_reset(&w);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct wl_display *wl = wl_display_connect_to_fd(sockets[0]);
    assert(wl);
    struct wayland_display display = {.wl = wl};
    display.text_input_mgr = (struct zwp_text_input_manager_v3 *)&display;
    seat.conn = &display;
    seat.text_input = (struct zwp_text_input_v3 *)wl_proxy_create(
            (struct wl_proxy *)wl, &zwp_text_input_v3_interface);
    assert(seat.text_input);
    seat.ime_focus = &w;
    w.conn = &display;
    w.ime_seat = &seat;
    w.base.text_input_active = true;
    w.base.text_surrounding = strdup("ab\xf0\x9f\x98\x80" "cd");
    w.base.text_length = 8;
    w.base.text_begin = 2;
    w.base.text_end = 6;
    w.base.text_revision = 41;
    w.base.text_generation = 9;
    assert(wayland_surface_update_text_input(&w));
    text_input_listener.delete_surrounding_text(&seat, NULL, 2, 2);
    text_input_listener.commit_string(&seat, NULL, "X");
    text_input_listener.done(&seat, seat.text_input, 1);
    struct yasl_private_event edit;
    assert(input_queue_pop(&w.input, &edit));
    assert(edit.type == YASL_PRIVATE_EVENT_TEXT_EDIT && edit.surface == &w.base);
    assert(strcmp(edit.text_edit.data, "X") == 0 && edit.text_edit.len == 1);
    assert(edit.text_edit.begin == 2 && edit.text_edit.end == 8);
    assert(edit.text_edit.revision == 41 && edit.text_edit.generation == 9);
    struct yasl_private_event committed;
    assert(!input_queue_pop(&w.input, &committed));

    /* Deletion without a commit replaces the range with the empty string. */
    text_input_listener.delete_surrounding_text(&seat, NULL, 2, 2);
    text_input_listener.done(&seat, seat.text_input, 1);
    assert(input_queue_pop(&w.input, &edit));
    assert(edit.type == YASL_PRIVATE_EVENT_TEXT_EDIT && edit.text_edit.len == 0);
    assert(edit.text_edit.data[0] == '\0' && edit.text_edit.begin == 2 && edit.text_edit.end == 8);
    assert(!input_queue_pop(&w.input, &committed));
    /* Update the document while an older compositor edit is in flight. */
    w.base.text_revision = 42;
    w.base.text_begin = w.base.text_end = 0;
    assert(wayland_surface_update_text_input(&w));
    text_input_listener.delete_surrounding_text(&seat, seat.text_input, 2, 2);
    text_input_listener.commit_string(&seat, seat.text_input, "Y");
    text_input_listener.done(&seat, seat.text_input, 1);
    assert(input_queue_pop(&w.input, &edit));
    assert(edit.type == YASL_PRIVATE_EVENT_TEXT_EDIT);
    assert(edit.text_edit.begin == 2 && edit.text_edit.end == 8);
    assert(edit.text_edit.revision == 41 && edit.text_edit.generation == 9);
    assert(strcmp(edit.text_edit.data, "Y") == 0);
    display.base.ops = &yasl_private_wayland_backend;
    w.base.display = &display.base;
    assert(yasl_private_events_open(&display.base));
    assert(yasl_private_events_register(&w.base));
    text_input_listener.preedit_string(&seat, seat.text_input, "compose", 2, 2);
    text_input_listener.done(&seat, seat.text_input, 1);
    struct yasl_private_event composition = next_event(&display.base, YASL_PRIVATE_EVENT_COMPOSITION);
    assert(composition.composition.revision == 41 && composition.composition.generation == 9);
    text_input_listener.preedit_string(&seat, seat.text_input, "compose", 2, 2);
    text_input_listener.done(&seat, seat.text_input, 2);
    composition = next_event(&display.base, YASL_PRIVATE_EVENT_COMPOSITION);
    assert(composition.composition.revision == 42 && composition.composition.generation == 9);
    for (int i = 0; i < 80; i++) {
        w.base.text_revision = 100 + i;
        assert(wayland_surface_update_text_input(&w));
    }
    assert(seat.ime_barrier);
    uint32_t message[3] = {
        wl_proxy_get_id((struct wl_proxy *)seat.ime_barrier),
        (uint32_t)(sizeof(message) << 16), 0,
    };
    assert(write(sockets[1], message, sizeof(message)) == sizeof(message));
    assert(wl_display_dispatch(wl) >= 0);
    text_input_listener.commit_string(&seat, seat.text_input, "Z");
    text_input_listener.done(&seat, seat.text_input, seat.ime_serial);
    edit = next_event(&display.base, YASL_PRIVATE_EVENT_TEXT_EDIT);
    assert(edit.type == YASL_PRIVATE_EVENT_TEXT_EDIT);
    assert(edit.text_edit.revision == 179);
    assert(strcmp(edit.text_edit.data, "Z") == 0);
    yasl_private_events_unregister(&w.base);
    yasl_private_events_close(&display.base);
    if (seat.ime_barrier) wl_callback_destroy(seat.ime_barrier);
    free(seat.ime_snapshots);
    wl_proxy_destroy((struct wl_proxy *)seat.text_input);
    wl_display_disconnect(wl);
    close(sockets[1]);
    input_queue_clear(&w.input);
    free(w.base.text_surrounding);
    return 0;
}
