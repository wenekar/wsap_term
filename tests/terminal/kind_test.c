#include <assert.h>
#include <string.h>

#include "helpers/app_fixture.h"
#include "terminal/terminal.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

/* The app asks every window for terminal data, such as the frame source.
 * A window that shows other content must answer as no terminal, and must
 * not read its content as one. */

static const int other_kind;

int main(void)
{
    struct test_app app;
    test_app_init(&app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });

    static unsigned char content[1 << 16];
    memset(content, 0xa5, sizeof(content));
    struct window *w = window_create(&app.app->host, &(struct window_spec){0});
    assert(w);
    assert(window_ui_mount(w, &(struct window_ui_spec){
        .name = "other", .kind = &other_kind, .content = content,
    }));

    assert(!terminal_is(w));
    assert(terminal_tab_count(w) == 0);
    assert(terminal_active_session(w) == NULL);
    struct terminal_frame_source source;
    assert(!terminal_frame_source(w, 0, &source));
    terminal_prepare(w, 0);

    window_close(w);
    test_app_fini(&app);
    return 0;
}
