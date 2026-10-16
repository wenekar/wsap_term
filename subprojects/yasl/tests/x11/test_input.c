/* An isolated real X server supplies the keymap; disabling this surface's XIC
 * deliberately exercises text insertion when no input method is available. */
#define _POSIX_C_SOURCE 200809L
#include <locale.h>
#include "../../src/platform/x11/window.c"

static void check_selection_encoding(struct x11_display *d, struct x11_surface *s)
{
    struct yasl_private_event queued;
    while (yasl_private_display_poll_event(&d->base, &queued)) {}
    Window requestor = XCreateSimpleWindow(d->native, DefaultRootWindow(d->native),
        0, 0, 1, 1, 0, 0, 0);
    Atom property = XInternAtom(d->native, "YASL_PRIVATE_TEST_STRING", False);
    XSelectionRequestEvent request = {.display = d->native, .requestor = requestor,
        .selection = d->clipboard, .target = XA_STRING, .property = property};
    yasl_private_clipboard_set(&d->base, YASL_PRIVATE_CLIPBOARD, "café £");
    selection_request(d, &request);
    Atom actual;
    int format;
    unsigned long count, left;
    unsigned char *data = NULL;
    assert(XGetWindowProperty(d->native, requestor, property, 0, 100, True,
        AnyPropertyType, &actual, &format, &count, &left, &data) == Success);
    assert(actual == XA_STRING && format == 8 && count == 6);
    assert(memcmp(data, "caf\xe9 \xa3", count) == 0);
    XFree(data);
    yasl_private_clipboard_set(&d->base, YASL_PRIVATE_CLIPBOARD, "字");
    selection_request(d, &request);
    assert(XGetWindowProperty(d->native, requestor, property, 0, 100, True,
        AnyPropertyType, &actual, &format, &count, &left, &data) == Success);
    assert(actual == None); /* Never mislabel UTF-8 as Latin-1. */
    if (data) XFree(data);
    request.target = d->targets;
    selection_request(d, &request);
    assert(XGetWindowProperty(d->native, requestor, property, 0, 100, True,
        AnyPropertyType, &actual, &format, &count, &left, &data) == Success);
    /* Text outside Latin-1 offers no STRING target. */
    assert(actual == XA_ATOM && format == 32 && count == 4);
    assert(((Atom *)data)[0] == d->targets && ((Atom *)data)[1] == d->utf8 &&
           ((Atom *)data)[2] == d->timestamp && ((Atom *)data)[3] == d->multiple);
    XFree(data);
    XDestroyWindow(d->native, requestor);

    /* Simulate legacy STRING, empty UTF-8 and malformed UTF-8. */
    const struct {
        Atom type;
        const void *bytes;
        size_t len;
        int format;
        const char *expected;
        enum yasl_private_transfer_status status;
    } cases[] = {
        {XA_STRING, "caf\xe9 \xa3", 6, 8, "café £", YASL_PRIVATE_TRANSFER_OK},
        {d->utf8, "", 0, 8, "", YASL_PRIVATE_TRANSFER_OK},
        {d->utf8, "\xff", 1, 8, NULL, YASL_PRIVATE_TRANSFER_FAILED},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct paste_request *pending = calloc(1, sizeof(*pending));
        assert(pending);
        pending->display = d;
        pending->window = XCreateSimpleWindow(d->native, DefaultRootWindow(d->native),
            0, 0, 1, 1, 0, 0, 0);
        pending->target = s;
        pending->id = (uint64_t)i + 1;
        pending->timeout = yasl_loop_add_timer(d->loop, PASTE_TIMEOUT_NS,
                paste_timeout, pending);
        assert(pending->timeout);
        pending->next = d->pastes;
        d->pastes = pending;
        XChangeProperty(d->native, pending->window, property, cases[i].type,
            cases[i].format, PropModeReplace, cases[i].bytes, (int)cases[i].len);
        XSelectionEvent reply = {.requestor = pending->window, .property = property};
        selection_notify(d, &reply);
        struct yasl_private_event event;
        assert(yasl_private_display_poll_event(&d->base, &event));
        assert(event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == i + 1);
        assert(event.paste.status == cases[i].status);
        if (cases[i].status == YASL_PRIVATE_TRANSFER_OK) {
            assert(event.text.len == strlen(cases[i].expected));
            assert(strcmp(event.text.data, cases[i].expected) == 0);
        }
        assert(!yasl_private_display_poll_event(&d->base, &event));
    }

    /* INCR: the owner announces a size, then writes chunks until an empty one. */
    struct paste_request *incr = calloc(1, sizeof(*incr));
    assert(incr);
    incr->display = d;
    incr->window = XCreateSimpleWindow(d->native, DefaultRootWindow(d->native), 0, 0, 1, 1, 0, 0, 0);
    incr->target = s;
    incr->id = 9;
    incr->timeout = yasl_loop_add_timer(d->loop, PASTE_TIMEOUT_NS, paste_timeout, incr);
    assert(incr->timeout);
    incr->next = d->pastes;
    d->pastes = incr;
    unsigned long incr_bytes = 5;
    XChangeProperty(d->native, incr->window, d->paste_property, d->incr, 32, PropModeReplace,
        (const unsigned char *)&incr_bytes, 1);
    selection_notify(d, &(XSelectionEvent){.requestor = incr->window, .property = d->paste_property});
    struct yasl_private_event event;
    assert(incr->incr && !yasl_private_display_poll_event(&d->base, &event));
    const char *chunks[] = {"inc", "r\xc3\xa9", ""};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        XChangeProperty(d->native, incr->window, d->paste_property, d->utf8, 8, PropModeReplace,
            (const unsigned char *)chunks[i], (int)strlen(chunks[i]));
        assert(paste_incr_property(d, &(XPropertyEvent){.window = incr->window,
            .atom = d->paste_property, .state = PropertyNewValue}));
    }
    assert(yasl_private_display_poll_event(&d->base, &event));
    assert(event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == 9 &&
           event.paste.status == YASL_PRIVATE_TRANSFER_OK && !strcmp(event.paste.data, "incr\xc3\xa9"));

    struct paste_request *rejected = calloc(1, sizeof(*rejected));
    assert(rejected);
    rejected->window = XCreateSimpleWindow(d->native, DefaultRootWindow(d->native),
        0, 0, 1, 1, 0, 0, 0);
    rejected->target = s;
    rejected->selection = d->clipboard;
    rejected->id = 99;
    rejected->next = d->pastes;
    d->pastes = rejected;
    XSelectionEvent no_property = {.requestor = rejected->window, .property = None};
    selection_notify(d, &no_property);
    assert(yasl_private_display_poll_event(&d->base, &event));
    assert(event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == 99);
    assert(event.paste.status == YASL_PRIVATE_TRANSFER_FAILED);
}

/* Own the XSETTINGS selection with one Net/DoubleClickTime value. */
static void check_double_click(struct x11_display *d)
{
    /* No settings owner and no portal: the default applies. */
    unsetenv("DBUS_SESSION_BUS_ADDRESS");
    unsetenv("XDG_RUNTIME_DIR");
    assert(yasl_private_display_double_click_ms(&d->base) == YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT);

    char name[32];
    snprintf(name, sizeof(name), "_XSETTINGS_S%d", d->screen);
    Atom selection = XInternAtom(d->native, name, False);
    Atom property = XInternAtom(d->native, "_XSETTINGS_SETTINGS", False);
    Window owner = XCreateSimpleWindow(d->native, DefaultRootWindow(d->native),
        0, 0, 1, 1, 0, 0, 0);
    XSetSelectionOwner(d->native, selection, owner, CurrentTime);
    static const unsigned char blob[] = {
        LSBFirst, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
        0, 0, 19, 0, 'N', 'e', 't', '/', 'D', 'o', 'u', 'b', 'l', 'e',
        'C', 'l', 'i', 'c', 'k', 'T', 'i', 'm', 'e', 0, 0, 0, 0, 0,
        250, 0, 0, 0,
    };
    XChangeProperty(d->native, owner, property, property, 8, PropModeReplace,
        blob, sizeof(blob));
    XSync(d->native, False);
    assert(yasl_private_display_double_click_ms(&d->base) == 250);

    XDestroyWindow(d->native, owner);
    XSync(d->native, False);
    assert(yasl_private_display_double_click_ms(&d->base) == YASL_PRIVATE_DOUBLE_CLICK_MS_DEFAULT);
}

static struct yasl_private_event wait_paste(struct yasl_private_display *display, uint64_t id)
{
    struct yasl_private_event event;
    for (int i = 0; i < 256 && yasl_private_display_poll_event(display, &event); i++)
        if (event.type == YASL_PRIVATE_EVENT_PASTE && event.paste.request_id == id)
            return event;
    assert(!"expected paste completion was not queued");
    return (struct yasl_private_event){0};
}

static void check_request_completion(struct x11_display *d, struct x11_surface *s)
{
    assert(surface_input_queue(&s->base) == &s->input);
    assert(copy_text(&d->base, YASL_PRIVATE_CLIPBOARD, ""));
    XSync(d->native, False);
    assert(XGetSelectionOwner(d->native, d->clipboard) == d->selection_window);
    assert(request_paste(&d->base, YASL_PRIVATE_CLIPBOARD, &s->base, 101));
    struct yasl_private_event event = wait_paste(&d->base, 101);
    assert(event.paste.status == YASL_PRIVATE_TRANSFER_OK && event.paste.len == 0);
    assert(event.paste.data && !event.paste.data[0]);

    XSetSelectionOwner(d->native, d->clipboard, None, CurrentTime);
    XSync(d->native, False);
    assert(request_paste(&d->base, YASL_PRIVATE_CLIPBOARD, &s->base, 102));
    XSync(d->native, False);
    dispatch(&d->base);
    event = wait_paste(&d->base, 102);
    assert(event.paste.status == YASL_PRIVATE_TRANSFER_UNAVAILABLE);

    Window silent_owner = XCreateSimpleWindow(d->native, DefaultRootWindow(d->native),
        0, 0, 1, 1, 0, 0, 0);
    XSetSelectionOwner(d->native, d->clipboard, silent_owner, CurrentTime);
    XSync(d->native, False);
    assert(request_paste(&d->base, YASL_PRIVATE_CLIPBOARD, &s->base, 103));
    struct paste_request *timed = d->pastes;
    assert(timed && timed->id == 103 && timed->timeout);
    assert(yasl_loop_del_timer(d->loop, timed->timeout));
    timed->timeout = 0;
    paste_timeout(timed, 0, PASTE_TIMEOUT_NS);
    event = wait_paste(&d->base, 103);
    assert(event.paste.status == YASL_PRIVATE_TRANSFER_FAILED);

    struct yasl_private_window_spec spec = {.title = "Cancelled paste", .size = {80, 60}};
    struct yasl_surface *cancel_target = yasl_private_surface_create_toplevel(&d->base, &spec);
    assert(cancel_target);
    assert(request_paste(&d->base, YASL_PRIVATE_CLIPBOARD, cancel_target, 104));
    assert(d->pastes && d->pastes->target == (struct x11_surface *)cancel_target);
    yasl_private_timer_id timeout = d->pastes->timeout;
    yasl_private_surface_destroy(cancel_target);
    assert(d->pastes && !d->pastes->target && d->pastes->id == 0);
    assert(yasl_loop_del_timer(d->loop, timeout));
    d->pastes->timeout = 0;
    XSelectionEvent late_reply = {
        .requestor = d->pastes->window,
        .selection = d->pastes->selection,
        .property = None,
    };
    selection_notify(d, &late_reply);
    assert(!d->pastes);
    XDestroyWindow(d->native, silent_owner);
    XSetSelectionOwner(d->native, d->clipboard, None, CurrentTime);
    XSync(d->native, False);
}

static struct yasl_private_event next_composition(struct yasl_private_display *display)
{
    struct yasl_private_event event;
    for (int i = 0; i < 256 && yasl_private_display_poll_event(display, &event); i++)
        if (event.type == YASL_PRIVATE_EVENT_COMPOSITION)
            return event;
    assert(!"expected composition snapshot was not queued");
    return (struct yasl_private_event){0};
}

static void drain_display_events(struct yasl_private_display *display)
{
    struct yasl_private_event event;
    while (yasl_private_display_poll_event(display, &event)) {}
}

static void check_xim_callbacks(struct yasl_private_display *display, struct x11_surface *s)
{
    const char *old_locale = setlocale(LC_CTYPE, NULL);
    char *saved_locale = old_locale ? strdup(old_locale) : NULL;
    const char *utf8_locale = setlocale(LC_CTYPE, "C.UTF-8");
    if (!utf8_locale) utf8_locale = setlocale(LC_CTYPE, "C.utf8");
    assert(utf8_locale);

    drain_display_events(display);
    assert(xim_preedit_start(NULL, (XPointer)s, NULL) == -1);

    wchar_t initial[] = {L'A', (wchar_t)0x1f600, L'B'};
    XIMText wide = {.length = 3, .encoding_is_wchar = True,
        .string.wide_char = initial};
    XIMPreeditDrawCallbackStruct draw = {
        .caret = 2, .chg_first = 0, .chg_length = 0, .text = &wide,
    };
    xim_preedit_draw(NULL, (XPointer)s, (XPointer)&draw);
    assert(s->ime_preedit && strcmp(s->ime_preedit, "A😀B") == 0);
    struct yasl_private_event event = next_composition(display);
    assert(event.composition.active && event.composition.len == 6);
    assert(event.composition.begin == 5 && event.composition.end == 5);

    /* XIMText.length counts characters. The data need not have a terminator. */
    char multibyte[] = {(char)0xc3, (char)0xa9, 'Z', 'x'};
    XIMText mb = {.length = 2, .encoding_is_wchar = False,
        .string.multi_byte = multibyte};
    draw = (XIMPreeditDrawCallbackStruct){
        .caret = 3, .chg_first = 1, .chg_length = 1, .text = &mb,
    };
    xim_preedit_draw(NULL, (XPointer)s, (XPointer)&draw);
    assert(s->ime_preedit && strcmp(s->ime_preedit, "AéZB") == 0);
    event = next_composition(display);
    assert(event.composition.active && event.composition.begin == 4);
    assert(event.composition.end == 4);

    /* A NULL text string updates feedback/caret while preserving the snapshot. */
    XIMText feedback = {.length = 4, .encoding_is_wchar = False,
        .string.multi_byte = NULL};
    draw = (XIMPreeditDrawCallbackStruct){
        .caret = 1, .chg_first = 0, .chg_length = 4, .text = &feedback,
    };
    xim_preedit_draw(NULL, (XPointer)s, (XPointer)&draw);
    assert(strcmp(s->ime_preedit, "AéZB") == 0);
    event = next_composition(display);
    assert(event.composition.active && event.composition.begin == 1);

    char malformed[] = {(char)0xff, 'x'};
    XIMText invalid = {.length = 1, .encoding_is_wchar = False,
        .string.multi_byte = malformed};
    draw.text = &invalid;
    draw.chg_first = 0;
    draw.chg_length = 0;
    xim_preedit_draw(NULL, (XPointer)s, (XPointer)&draw);
    assert(strcmp(s->ime_preedit, "AéZB") == 0);
    struct yasl_private_event unused;
    assert(!yasl_private_display_poll_event(display, &unused));

    wchar_t empty[] = {0};
    XIMText deletion = {.length = 0, .encoding_is_wchar = True,
        .string.wide_char = empty};
    draw = (XIMPreeditDrawCallbackStruct){
        .caret = 0, .chg_first = 0, .chg_length = 4, .text = &deletion,
    };
    xim_preedit_draw(NULL, (XPointer)s, (XPointer)&draw);
    event = next_composition(display);
    assert(!event.composition.active && event.composition.len == 0);
    assert(!s->ime_preedit);

    /* Done also publishes an explicit end snapshot. */
    wide = (XIMText){.length = 3, .encoding_is_wchar = True,
        .string.wide_char = initial};
    draw = (XIMPreeditDrawCallbackStruct){
        .caret = 1, .chg_first = 0, .chg_length = 0, .text = &wide,
    };
    xim_preedit_draw(NULL, (XPointer)s, (XPointer)&draw);
    (void)next_composition(display);
    xim_preedit_done(NULL, (XPointer)s, NULL);
    event = next_composition(display);
    assert(!event.composition.active && !s->ime_preedit);

    if (saved_locale) {
        setlocale(LC_CTYPE, saved_locale);
        free(saved_locale);
    }
}

int main(void)
{
    struct yasl_private_display_spec ds = {.backend = YASL_PRIVATE_BACKEND_X11};
    struct yasl_private_display *base = yasl_private_display_open(NULL, &ds);
    assert(base);
    struct x11_display *d = (struct x11_display *)base;
    struct yasl_private_window_spec spec = {.title = "Keyboard fallback", .size = {100, 100}};
    struct yasl_surface *surface = yasl_private_surface_create_toplevel(base, &spec);
    assert(surface);
    struct x11_surface *s = (struct x11_surface *)surface;
    XIC saved_ic = s->ic;
    s->ic = NULL;
    unsigned code = XKeysymToKeycode(d->native, XK_a);
    int width = 0;
    KeySym *saved = XGetKeyboardMapping(d->native, code, 1, &width);
    assert(saved && width >= 2);
    KeySym *symbols = calloc((size_t)width, sizeof(*symbols));
    assert(symbols);
    const struct { KeySym symbol, shifted; const char *text; uint32_t label; } cases[] = {
        {XK_eacute, XK_Eacute, "é", 0xe9},
        {0x01000416, 0x01000416, "Ж", 0x416},
        {0x0101f600, 0x0101f600, "😀", 0x1f600},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        symbols[0] = cases[i].symbol;
        symbols[1] = cases[i].shifted;
        XChangeKeyboardMapping(d->native, code, width, symbols, 1);
        XSync(d->native, False);
        dispatch(base);
        input_queue_clear(&s->input);
        /* Native creation and keymap updates may already have queued focus or
         * reset input on the shared display stream. Isolate this synthetic key. */
        struct yasl_private_event event;
        while (yasl_private_display_poll_event(base, &event)) {}
        XKeyEvent native = {.type = KeyPress, .display = d->native,
            .window = s->window, .keycode = code};
        key_event(s, &native, false);
        assert(yasl_private_display_poll_event(base, &event));
        assert(event.type == YASL_PRIVATE_EVENT_KEY && event.key.key == YASL_KEY_A);
        assert(event.key.codepoint == cases[i].label);
        assert(yasl_private_display_poll_event(base, &event));
        assert(event.type == YASL_PRIVATE_EVENT_TEXT && event.text.len == strlen(cases[i].text));
        assert(strcmp(event.text.data, cases[i].text) == 0);
        assert(!yasl_private_display_poll_event(base, &event));
        native.type = KeyRelease;
        key_event(s, &native, false);
        assert(yasl_private_display_poll_event(base, &event) && event.type == YASL_PRIVATE_EVENT_KEY);
        assert(!yasl_private_display_poll_event(base, &event));
    }
    XChangeKeyboardMapping(d->native, code, width, saved, 1);
    XSync(d->native, False);
    free(symbols);
    XFree(saved);
    s->ic = saved_ic;
    check_selection_encoding(d, s);
    check_double_click(d);
    check_request_completion(d, s);
    check_xim_callbacks(base, s);

    /* Test CSD interactive actions, window menu, and IME caret */
    assert(yasl_private_display_caps(base) & YASL_PRIVATE_CAP_CSD);
    yasl_private_surface_set_ime_caret(surface, (struct yasl_private_rect2i){10, 20, 2, 16});
    assert(yasl_private_surface_ime_preedit(surface) == NULL);
    int begin = -1, end = -1;
    assert(!yasl_private_surface_ime_preedit_cursor(surface, &begin, &end));
    /* The window menu, move and resize need a button held; nothing presses one here. */
    errno = 0;
    assert(!yasl_private_surface_show_window_menu(surface, (struct yasl_private_vector2i){50, 50}) &&
           errno == ESTALE);
    errno = 0;
    assert(!yasl_private_surface_start_interactive_move(surface) && errno == ESTALE);

    yasl_private_surface_destroy(surface);
    yasl_private_display_close(base);
    return 0;
}
