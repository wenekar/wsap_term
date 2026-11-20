/* One keyboard state must give one modifier set on key and pointer events.
 * The keymaps below put several names on shared real modifiers, so Ctrl+click
 * and Ctrl+key must agree. The test drives both paths through the protocol
 * listeners and reads the mods field of each event. */
#define _GNU_SOURCE
#include "../../src/platform/wayland/keyboard.c"
#include "../../src/platform/wayland/pointer.c"
#include <assert.h>
#include <sys/mman.h>

struct fixture {
    struct wayland_display d;
    struct seat s;
    struct wayland_surface w;
};

static void load_keymap(struct fixture *f, const char *options)
{
    struct xkb_rule_names names = {.layout = "us", .options = options};
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(f->d.xkb_ctx, &names, 0);
    assert(keymap);
    char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    assert(text);
    size_t len = strlen(text);
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    assert(fd >= 0 && write(fd, text, len) == (ssize_t)len);
    keyboard_listener.keymap(&f->s, NULL, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    free(text);
    xkb_keymap_unref(keymap);
    assert(f->s.xkb_state);
}

static void setup(struct fixture *f, const char *options)
{
    *f = (struct fixture){0};
    f->d.base.ops = &yasl_private_wayland_backend;
    f->d.xkb_ctx = xkb_context_new(0);
    assert(f->d.xkb_ctx);
    f->w = (struct wayland_surface){.base.display = &f->d.base, .conn = &f->d};
    wayland_surface_set_callbacks(&f->w, NULL, NULL);
    f->s = (struct seat){.conn = &f->d, .keyboard_focus = &f->w, .pointer_focus = &f->w,
        .repeat_fd = -1, .compose_attempted = true};
    load_keymap(f, options);
}

static void teardown(struct fixture *f)
{
    input_queue_clear(&f->w.input);
    free(f->s.ptr_presses.ev);
    xkb_state_unref(f->s.xkb_state);
    xkb_keymap_unref(f->s.xkb_keymap);
    xkb_context_unref(f->d.xkb_ctx);
}

/* The next event of type. Other events in the queue are discarded. */
static struct yasl_private_event take(struct wayland_surface *w, enum yasl_private_event_type type)
{
    struct yasl_private_event ev;
    while (input_queue_pop(&w->input, &ev))
        if (ev.type == type)
            return ev;
    assert(!"no event of the requested type");
    return ev;
}

/* Returns the real modifier that `name` maps to in the keymap. */
static xkb_mod_mask_t real_mod(struct fixture *f, const char *name)
{
    return 1u << xkb_keymap_mod_get_index(f->s.xkb_keymap, name);
}

/* Depresses modifiers, then checks the mods of a key press and of a button
 * press in that state. */
static void expect_mods(struct fixture *f, xkb_mod_mask_t depressed, unsigned expected)
{
    keyboard_listener.modifiers(&f->s, NULL, 1, depressed, 0, 0, 0);
    keyboard_listener.key(&f->s, NULL, 2, 0, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    struct yasl_private_event key = take(&f->w, YASL_PRIVATE_EVENT_KEY);
    input_queue_clear(&f->w.input);
    keyboard_listener.key(&f->s, NULL, 3, 0, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    input_queue_clear(&f->w.input);

    pointer_listener.button(&f->s, NULL, 4, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    pointer_listener.frame(&f->s, NULL);
    struct yasl_private_event down = take(&f->w, YASL_PRIVATE_EVENT_POINTER_DOWN);
    input_queue_clear(&f->w.input);
    pointer_listener.button(&f->s, NULL, 5, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
    pointer_listener.frame(&f->s, NULL);
    input_queue_clear(&f->w.input);

    assert(key.key.mods == expected);
    assert(down.pointer.mods == expected);
}

static void test_keymap(const char *options)
{
    struct fixture f;
    setup(&f, options);
    expect_mods(&f, 0, 0);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_SHIFT), YASL_PRIVATE_MOD_SHIFT);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_CTRL), YASL_PRIVATE_MOD_CTRL);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_ALT), YASL_PRIVATE_MOD_ALT);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_LOGO), YASL_PRIVATE_MOD_SUPER);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_CAPS), YASL_PRIVATE_MOD_CAPS);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_CTRL) | real_mod(&f, XKB_MOD_NAME_ALT),
                YASL_PRIVATE_MOD_CTRL | YASL_PRIVATE_MOD_ALT);
    expect_mods(&f, real_mod(&f, XKB_MOD_NAME_ALT) | real_mod(&f, XKB_MOD_NAME_LOGO),
                YASL_PRIVATE_MOD_ALT | YASL_PRIVATE_MOD_SUPER);
    teardown(&f);
}

int main(void)
{
    test_keymap(NULL);
    test_keymap("altwin:meta_alt"); /* Meta and Alt on the Alt modifier. */
    test_keymap("altwin:hyper_win"); /* Hyper on the Win (Super) modifier. */
    return 0;
}
