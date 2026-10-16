#include "../../src/platform/wayland/keyboard.c"
#include <assert.h>

static struct yasl_private_event next(struct wayland_surface *w, enum yasl_private_event_type type)
{
    struct yasl_private_event ev;
    assert(input_queue_pop(&w->input, &ev));
    assert(ev.type == type && ev.surface == &w->base);
    return ev;
}

static void empty(struct wayland_surface *w)
{
    struct yasl_private_event ev;
    assert(!input_queue_pop(&w->input, &ev));
}

static void layout(struct seat *s, const char *name, const char *variant)
{
    keyboard_text_reset(s);
    if (s->xkb_state) xkb_state_unref(s->xkb_state);
    if (s->xkb_keymap) xkb_keymap_unref(s->xkb_keymap);
    struct xkb_rule_names names = {.layout = name, .variant = variant};
    s->xkb_keymap = xkb_keymap_new_from_names(s->conn->xkb_ctx, &names, 0);
    assert(s->xkb_keymap);
    s->xkb_state = xkb_state_new(s->xkb_keymap);
    assert(s->xkb_state);
    s->input_mods_ready = false;
    s->mod_alt = xkb_keymap_mod_get_index(s->xkb_keymap, XKB_MOD_NAME_ALT);
}

static void modifiers(struct seat *s, const char *name)
{
    xkb_mod_mask_t mask = name ? 1u << xkb_keymap_mod_get_index(s->xkb_keymap, name) : 0;
    xkb_state_update_mask(s->xkb_state, mask, 0, 0, 0, 0, 0);
}

static void key(struct seat *s, uint32_t code, bool down)
{
    keyboard_listener.key(s, NULL, 1, 0, code, down ?
        WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

int main(void)
{
    struct wayland_display d = {.base.ops = &yasl_private_wayland_backend};
    d.xkb_ctx = xkb_context_new(0);
    assert(d.xkb_ctx);
    struct wayland_surface w = {.base.display = &d.base, .conn = &d};
    wayland_surface_set_callbacks(&w, NULL, NULL);
    struct seat s = {.conn = &d, .keyboard_focus = &w, .repeat_fd = -1,
        .compose_attempted = true};
    const char compose[] = "<dead_acute> <e> : \"é\" U00E9\n";
    struct xkb_compose_table *table = xkb_compose_table_new_from_buffer(d.xkb_ctx,
        compose, sizeof(compose) - 1, "C.UTF-8", XKB_COMPOSE_FORMAT_TEXT_V1, 0);
    assert(table);
    s.compose = xkb_compose_state_new(table, 0);
    assert(s.compose);
    xkb_compose_table_unref(table);
    layout(&s, "us", NULL);
    key(&s, KEY_A, true);
    struct yasl_private_event ev = next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(ev.key.key == YASL_KEY_A && ev.key.native_code == KEY_A);
    assert(ev.key.codepoint == 'a' && ev.key.shifted == 'A' && !ev.key.mods);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "a") == 0);
    modifiers(&s, XKB_MOD_NAME_SHIFT);
    key(&s, KEY_A, false); /* Modifier changes cannot change physical identity. */
    ev = next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(ev.key.key == YASL_KEY_A && ev.key.action == YASL_KEY_RELEASE);
    assert(ev.key.mods == YASL_PRIVATE_MOD_SHIFT);
    empty(&w);
    key(&s, KEY_A, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "A") == 0);
    modifiers(&s, XKB_MOD_NAME_CAPS);
    key(&s, KEY_A, true);
    assert(next(&w, YASL_PRIVATE_EVENT_KEY).key.mods == YASL_PRIVATE_MOD_CAPS);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "A") == 0);
    const char *shortcuts[] = {XKB_MOD_NAME_CTRL, XKB_MOD_NAME_ALT, XKB_MOD_NAME_LOGO};
    for (size_t i = 0; i < sizeof(shortcuts)/sizeof(shortcuts[0]); i++) {
        modifiers(&s, shortcuts[i]);
        key(&s, KEY_A, true);
        next(&w, YASL_PRIVATE_EVENT_KEY);
        empty(&w); /* Shortcut characters never leak into committed text. */
    }
    layout(&s, "fr", NULL);
    key(&s, KEY_A, true);
    ev = next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(ev.key.key == YASL_KEY_A && ev.key.codepoint == 'q');
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "q") == 0);
    layout(&s, "de", NULL);
    modifiers(&s, "Mod5"); /* AltGr is text input, not an Alt shortcut. */
    key(&s, KEY_E, true);
    assert(next(&w, YASL_PRIVATE_EVENT_KEY).key.key == YASL_KEY_E);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "€") == 0);
    layout(&s, "us", "intl");
    key(&s, KEY_APOSTROPHE, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    empty(&w);
    key(&s, KEY_E, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "é") == 0);
    assert(keyboard_dispatch_key(&s, &w, KEY_E, YASL_KEY_REPEAT));
    assert(next(&w, YASL_PRIVATE_EVENT_KEY).key.action == YASL_KEY_REPEAT);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "e") == 0);
    key(&s, KEY_APOSTROPHE, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    keyboard_reset_keys(&s, &w);
    key(&s, KEY_E, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "e") == 0);
    key(&s, KEY_APOSTROPHE, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    s.ime_focus = &w;
    w.base.text_input_active = true;
    text_input_listener.commit_string(&s, NULL, "字");
    text_input_listener.done(&s, NULL, 0);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "字") == 0);
    key(&s, KEY_E, true);
    next(&w, YASL_PRIVATE_EVENT_KEY);
    assert(strcmp(next(&w, YASL_PRIVATE_EVENT_TEXT).text.data, "e") == 0);
    empty(&w);
    xkb_compose_state_unref(s.compose);
    xkb_state_unref(s.xkb_state);
    xkb_keymap_unref(s.xkb_keymap);
    xkb_context_unref(d.xkb_ctx);
    input_queue_clear(&w.input);
    return 0;
}
