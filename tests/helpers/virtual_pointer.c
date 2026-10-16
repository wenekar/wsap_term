/* A pointer for tests on a headless wlroots compositor, which has no
 * input device of its own. It reads one command per line on stdin:
 *
 *   X Y        move to layout pixel X, Y
 *   press      press the left button
 *   release    release it
 *   rpress     press the right button
 *   rrelease   release it
 *
 * and answers each with "ok" once the compositor has it. Usage:
 * virtual-pointer WIDTH HEIGHT, the layout's size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *manager;

static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name))
        manager = wl_registry_bind(registry, name,
                                   &zwlr_virtual_pointer_manager_v1_interface, 1);
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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s WIDTH HEIGHT\n", argv[0]);
        return 2;
    }
    unsigned width = (unsigned)atoi(argv[1]), height = (unsigned)atoi(argv[2]);
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "virtual-pointer: no Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!manager) {
        fprintf(stderr, "virtual-pointer: compositor has no virtual pointers\n");
        return 1;
    }
    struct zwlr_virtual_pointer_v1 *pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    wl_display_roundtrip(display);
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("ok");

    char line[128];
    uint32_t time = 1;
    while (fgets(line, sizeof(line), stdin)) {
        unsigned x, y;
        if (sscanf(line, "%u %u", &x, &y) == 2)
            zwlr_virtual_pointer_v1_motion_absolute(pointer, time++, x, y,
                                                    width, height);
        else if (!strncmp(line, "press", 5))
            zwlr_virtual_pointer_v1_button(pointer, time++, BTN_LEFT, 1);
        else if (!strncmp(line, "release", 7))
            zwlr_virtual_pointer_v1_button(pointer, time++, BTN_LEFT, 0);
        else if (!strncmp(line, "rpress", 6))
            zwlr_virtual_pointer_v1_button(pointer, time++, BTN_RIGHT, 1);
        else if (!strncmp(line, "rrelease", 8))
            zwlr_virtual_pointer_v1_button(pointer, time++, BTN_RIGHT, 0);
        else
            continue;
        zwlr_virtual_pointer_v1_frame(pointer);
        if (wl_display_roundtrip(display) < 0)
            return 1;
        puts("ok");
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    wl_display_disconnect(display);
    return 0;
}
