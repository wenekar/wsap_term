#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "input/input.h"
#include "ui/yasl/drop.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/ui_input.h"

/* The window is zeroed for each case. It has no surface and no popups, so
 * native input only queues events. */
static struct window w;

static void fresh_window(void)
{
    memset(&w, 0, sizeof w);
}

static struct yasl_event yasl_pointer(unsigned action, unsigned button, unsigned buttons)
{
    struct yasl_event e = { .type = YASL_EVENT_POINTER, .action = action };
    e.pointer.button = button;
    e.pointer.buttons = buttons;
    return e;
}

static struct yasl_event yasl_pen(unsigned action)
{
    struct yasl_event e = { .type = YASL_EVENT_PEN, .action = action };
    e.pen.tool = YASL_PEN_PEN;
    return e;
}

/* Sends one native event. Returns the single mouse event it queued. */
static struct input_mouse_event feed(const struct yasl_event *e)
{
    window_native_input(&w, e);
    struct input_event out;
    bool queued = input_queue_take(&w.input, &out);
    assert(queued);
    assert(out.kind == INPUT_MOUSE);
    return out.mouse;
}

static const unsigned primary = YASL_BUTTON_BIT(YASL_BUTTON_PRIMARY);
static const unsigned secondary = YASL_BUTTON_BIT(YASL_BUTTON_SECONDARY);

/* A left drag keeps the left button held after a right press and release. */
static void drag_survives_second_button_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_PRIMARY, primary);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_SECONDARY, primary | secondary);
    m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_SECONDARY, primary);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, primary);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_PRIMARY, 0);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, 0);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_NONE);
}

/* When the drag button goes up, the other button still held takes the drag. */
static void drag_moves_to_other_held_button_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_PRIMARY, primary);
    feed(&e);
    e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_SECONDARY, primary | secondary);
    feed(&e);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_PRIMARY, secondary);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, secondary);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_UP, YASL_BUTTON_SECONDARY, 0);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, 0);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_NONE);
}

/* A drag keeps the button that started it while another button is held. */
static void drag_keeps_starting_button_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_SECONDARY, secondary);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_SECONDARY);

    e = yasl_pointer(YASL_POINTER_DOWN, YASL_BUTTON_PRIMARY, primary | secondary);
    m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pointer(YASL_POINTER_MOVE, 0, primary | secondary);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_SECONDARY);
}

/* Pen contact reports the primary button until the pen lifts. */
static void pen_contact_test(void)
{
    fresh_window();
    struct yasl_event e = yasl_pen(YASL_PEN_DOWN);
    struct input_mouse_event m = feed(&e);
    assert(m.kind == PTR_PRESS && m.button == POINTER_BUTTON_PRIMARY);
    assert(m.device == POINTER_DEVICE_PEN);

    e = yasl_pen(YASL_PEN_MOVE);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pen(YASL_PEN_UP);
    m = feed(&e);
    assert(m.kind == PTR_RELEASE && m.button == POINTER_BUTTON_PRIMARY);

    e = yasl_pen(YASL_PEN_MOVE);
    m = feed(&e);
    assert(m.kind == PTR_MOTION && m.button == POINTER_BUTTON_NONE);
}

/* Sends one key press that carries no native text. Returns the queued key. */
static struct input_key_event feed_key(enum yasl_key key, uint32_t codepoint,
                                       uint32_t shifted, unsigned mods)
{
    struct yasl_event e = { .type = YASL_EVENT_KEY, .action = YASL_KEY_PRESS, .mods = mods };
    e.key.key = key;
    e.key.codepoint = codepoint;
    e.key.shifted = shifted;
    e.key.text_input = YASL_KEY_TEXT_UNSPECIFIED;
    window_native_input(&w, &e);
    struct input_event out;
    bool queued = input_queue_take(&w.input, &out);
    assert(queued);
    assert(out.kind == INPUT_KEY);
    return out.key;
}

/* Caps Lock changes letter keys only. Shift cancels it on letters. */
static void caps_lock_key_text_test(void)
{
    fresh_window();
    struct input_key_event k = feed_key(YASL_KEY_1, '1', '!', YASL_MOD_CAPS);
    assert(k.text == '1');
    assert(ui_key_from_input(&k).text == '1');

    k = feed_key(YASL_KEY_A, 'a', 'A', YASL_MOD_CAPS);
    assert(k.text == 'A');
    assert(ui_key_from_input(&k).text == 'A');

    k = feed_key(YASL_KEY_A, 'a', 'A', YASL_MOD_CAPS | YASL_MOD_SHIFT);
    assert(k.text == 'a');
    assert(ui_key_from_input(&k).text == 'a');

    /* s-cedilla on a Turkish layout sits on the US ';' key. */
    k = feed_key(YASL_KEY_SEMICOLON, 0x15F, 0x15E, YASL_MOD_CAPS);
    assert(k.text == 0x15E);
    assert(ui_key_from_input(&k).text == 0x15E);

    k = feed_key(YASL_KEY_SEMICOLON, 0x15F, 0x15E, YASL_MOD_CAPS | YASL_MOD_SHIFT);
    assert(k.text == 0x15F);
    assert(ui_key_from_input(&k).text == 0x15F);

    /* e-acute on an AZERTY layout sits on the '2' key. Its shifted label is ASCII. */
    k = feed_key(YASL_KEY_2, 0xE9, '2', YASL_MOD_CAPS);
    assert(k.text == 0xE9);
    assert(ui_key_from_input(&k).text == 0xE9);
}

/* Builds a DROP RELEASE event with `length` bytes of `text` and type `mime`. */
static struct yasl_event drop_release(const char *mime, const char *text, size_t length)
{
    struct yasl_event e = {
        .type = YASL_EVENT_DROP,
        .action = YASL_DROP_RELEASE,
        .text = text,
        .length = length,
    };
    snprintf(e.drop.mime, sizeof e.drop.mime, "%s", mime);
    return e;
}

/* Takes the next queued event. It must be a paste of `text`. */
static void expect_paste(const char *text)
{
    struct input_event out;
    assert(input_queue_take(&w.input, &out));
    assert(out.kind == INPUT_PASTE);
    assert(out.paste.len == strlen(text));
    assert(memcmp(out.paste.data, text, out.paste.len) == 0);
    input_event_release(&out);
}

/* Makes a new empty directory in /tmp. The path is written to `path`. */
static void fresh_dir(char path[PATH_MAX])
{
    snprintf(path, PATH_MAX, "/tmp/wsap-drop-XXXXXX");
    assert(mkdtemp(path));
}

/* Points XDG_RUNTIME_DIR at `dir` for the next drop. */
static void use_runtime(const char *dir)
{
    assert(setenv("XDG_RUNTIME_DIR", dir, 1) == 0);
}

/* Removes the drop directories of a case, when the case made them. */
static void remove_drop_dirs(const char *runtime)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/wsap_term/drops", runtime);
    rmdir(path);
    snprintf(path, sizeof path, "%s/wsap_term", runtime);
    rmdir(path);
    rmdir(runtime);
}

/* The image payload holds NUL bytes, so every byte must survive the save. */
static const char image_bytes[] = { 'P', 'N', 'G', 0, 1, 0, 2, 'x' };

/* A PNG drop is saved to a private file. The input queue gets the file as a
 * file drop. Drop cleanup then removes the file. */
static void image_drop_saves_file_test(void)
{
    char runtime[PATH_MAX];
    fresh_dir(runtime);
    use_runtime(runtime);
    fresh_window();

    struct yasl_event e = drop_release("image/png", image_bytes, sizeof image_bytes);
    window_native_input(&w, &e);
    struct input_event out;
    assert(input_queue_take(&w.input, &out));
    assert(out.kind == INPUT_DROP_FILES);
    char *uri = malloc(out.drop.len + 1);
    assert(uri);
    memcpy(uri, out.drop.data, out.drop.len);
    uri[out.drop.len] = '\0';
    input_event_release(&out);
    assert(!input_queue_take(&w.input, &out));

    char prefix[PATH_MAX + 32];
    snprintf(prefix, sizeof prefix, "file://%s/wsap_term/drops/", runtime);
    assert(strncmp(uri, prefix, strlen(prefix)) == 0);
    size_t n = strlen(uri);
    assert(n > 4 && strcmp(uri + n - 4, ".png") == 0);

    const char *path = uri + strlen("file://");
    struct stat st;
    assert(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);
    char got[sizeof image_bytes + 1];
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t got_len = fread(got, 1, sizeof got, f);
    fclose(f);
    assert(got_len == sizeof image_bytes);
    assert(memcmp(got, image_bytes, sizeof image_bytes) == 0);

    drop_cleanup();
    assert(stat(path, &st) != 0 && errno == ENOENT);
    free(uri);
    remove_drop_dirs(runtime);
}

/* Text drops paste like clipboard text, for each text type. */
static void text_drop_pastes_test(void)
{
    static const char text[] = "caf\xc3\xa9 \xe2\x9c\x93\n";
    static const char *const types[] = {
        "text/plain;charset=utf-8", "UTF8_STRING", "text/plain",
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        fresh_window();
        struct yasl_event e = drop_release(types[i], text, sizeof text - 1);
        window_native_input(&w, &e);
        expect_paste(text);
    }
}

/* A uri list with no local file pastes its URLs. Comment lines and blank
 * lines are skipped, and the URLs are joined with a space. */
static void web_uri_list_pastes_test(void)
{
    fresh_window();
    static const char link[] = "https://example.com/a.png\r\n";
    struct yasl_event e = drop_release("text/uri-list", link, sizeof link - 1);
    window_native_input(&w, &e);
    expect_paste("https://example.com/a.png");

    fresh_window();
    static const char list[] = "# copied from a browser\r\n"
                               "https://example.com/a.png\r\n"
                               "\r\n"
                               "https://example.com/b.png\r\n";
    e = drop_release("text/uri-list", list, sizeof list - 1);
    window_native_input(&w, &e);
    expect_paste("https://example.com/a.png https://example.com/b.png");
}

/* A uri list with a local file queues the list as a file drop, as before. */
static void local_uri_list_drops_files_test(void)
{
    fresh_window();
    static const char list[] = "file:///tmp/x\r\n";
    struct yasl_event e = drop_release("text/uri-list", list, sizeof list - 1);
    window_native_input(&w, &e);
    struct input_event out;
    assert(input_queue_take(&w.input, &out));
    assert(out.kind == INPUT_DROP_FILES);
    assert(out.drop.len == sizeof list - 1);
    assert(memcmp(out.drop.data, list, out.drop.len) == 0);
    input_event_release(&out);
}

/* A drop of another data type queues nothing. An empty image queues nothing. */
static void other_drops_queue_nothing_test(void)
{
    char runtime[PATH_MAX];
    fresh_dir(runtime);
    use_runtime(runtime);
    static const char json[] = "{}";

    fresh_window();
    struct yasl_event e = drop_release("application/json", json, sizeof json - 1);
    window_native_input(&w, &e);
    e = drop_release("image/png", json, 0);
    window_native_input(&w, &e);
    struct input_event out;
    assert(!input_queue_take(&w.input, &out));
    remove_drop_dirs(runtime);
}

/* An image over 64 MiB is refused. The save never reads the bytes, so the
 * buffer stays untouched. Nothing is queued. */
static void oversize_image_refused_test(void)
{
    char runtime[PATH_MAX];
    fresh_dir(runtime);
    use_runtime(runtime);
    const size_t limit = (size_t)64 * 1024 * 1024;
    char *big = calloc(1, limit + 1);
    assert(big);

    fresh_window();
    struct yasl_event e = drop_release("image/png", big, limit + 1);
    window_native_input(&w, &e);
    struct input_event out;
    assert(!input_queue_take(&w.input, &out));
    free(big);
    remove_drop_dirs(runtime);
}

/* A drops directory that is a symlink refuses the image. Nothing is queued,
 * and the link target stays empty. */
static void drops_symlink_refused_test(void)
{
    char runtime[PATH_MAX], target[PATH_MAX];
    char base[2 * PATH_MAX], drops[3 * PATH_MAX];
    fresh_dir(runtime);
    fresh_dir(target);
    snprintf(base, sizeof base, "%s/wsap_term", runtime);
    snprintf(drops, sizeof drops, "%s/drops", base);
    assert(mkdir(base, 0700) == 0);
    assert(symlink(target, drops) == 0);
    use_runtime(runtime);

    fresh_window();
    struct yasl_event e = drop_release("image/png", image_bytes, sizeof image_bytes);
    window_native_input(&w, &e);
    struct input_event out;
    assert(!input_queue_take(&w.input, &out));
    assert(rmdir(target) == 0);

    unlink(drops);
    rmdir(base);
    rmdir(runtime);
}

int main(void)
{
    drag_survives_second_button_test();
    drag_moves_to_other_held_button_test();
    drag_keeps_starting_button_test();
    pen_contact_test();
    caps_lock_key_text_test();
    image_drop_saves_file_test();
    text_drop_pastes_test();
    web_uri_list_pastes_test();
    local_uri_list_drops_files_test();
    other_drops_queue_nothing_test();
    oversize_image_refused_test();
    drops_symlink_refused_test();
    return 0;
}
