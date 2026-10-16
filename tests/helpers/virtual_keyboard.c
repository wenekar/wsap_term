/* A keyboard for tests on a headless wlroots compositor, which has no
 * input device of its own. It reads one command per line on stdin:
 *
 *   press CODE     press the evdev key CODE
 *   release CODE   release it
 *
 * and answers each with "ok" once the compositor has it. The keymap is
 * the default US layout. wlroots takes modifiers only from the modifiers
 * request, so the helper tracks them and sends them after each key. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name,
                                   &zwp_virtual_keyboard_manager_v1_interface, 1);
    else if (!strcmp(interface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = global,
    .global_remove = global_remove,
};

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The keymap as a file, which the protocol hands over by fd. */
static int keymap_fd(struct xkb_keymap *keymap, uint32_t *size)
{
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    int fd = text ? memfd_create("keymap", MFD_CLOEXEC) : -1;
    size_t length = text ? strlen(text) + 1 : 0;
    if (fd >= 0 && write(fd, text, length) != (ssize_t)length) {
        close(fd);
        fd = -1;
    }
    *size = (uint32_t)length;
    free(text);
    return fd;
}

static void key(struct zwp_virtual_keyboard_v1 *keyboard, struct xkb_state *state,
                unsigned code, bool down)
{
    zwp_virtual_keyboard_v1_key(keyboard, now_ms(), code,
                                down ? WL_KEYBOARD_KEY_STATE_PRESSED
                                     : WL_KEYBOARD_KEY_STATE_RELEASED);
    /* xkb codes are evdev codes plus 8. */
    xkb_state_update_key(state, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    zwp_virtual_keyboard_v1_modifiers(
        keyboard, xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE));
}

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "virtual-keyboard: no Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager || !seat) {
        fprintf(stderr, "virtual-keyboard: compositor has no virtual keyboards\n");
        return 1;
    }
    struct zwp_virtual_keyboard_v1 *keyboard =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = context ? xkb_keymap_new_from_names(
        context, &(struct xkb_rule_names){ .layout = "us" }, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    struct xkb_state *state = keymap ? xkb_state_new(keymap) : NULL;
    uint32_t size = 0;
    int fd = state ? keymap_fd(keymap, &size) : -1;
    if (fd < 0) {
        fprintf(stderr, "virtual-keyboard: cannot build the keymap\n");
        return 1;
    }
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
    close(fd);
    if (wl_display_roundtrip(display) < 0)
        return 1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("ok");

    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        unsigned code;
        if (sscanf(line, "press %u", &code) == 1)
            key(keyboard, state, code, true);
        else if (sscanf(line, "release %u", &code) == 1)
            key(keyboard, state, code, false);
        else
            continue;
        if (wl_display_roundtrip(display) < 0)
            return 1;
        puts("ok");
    }
    zwp_virtual_keyboard_v1_destroy(keyboard);
    wl_display_disconnect(display);
    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    return 0;
}
