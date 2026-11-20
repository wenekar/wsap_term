#include <assert.h>
#include <stdlib.h>

#include "ui/yasl/internal.h"
#include "ui/yasl/window.h"

/* The debugger reads the host registry to find a window. The lookup must give
 * the host that owns each window, and one host must leave without a change to
 * the other. */
int main(void)
{
    struct window_config config = { .size = { .x = 320, .y = 200 } };
    struct window_host a = {0};
    struct window_host b = {0};
    window_host_init(&a, NULL, &config, NULL);
    window_host_init(&b, NULL, &config, NULL);
    assert(window_host_registry_count == 2);
    assert(window_host_registry_first() == &a);
    assert(window_host_registry[1] == &b);

    struct window *wa = window_create(&a, &(struct window_spec){0});
    struct window *wb = window_create(&b, &(struct window_spec){0});
    assert(wa && wb);
    assert(window_host(wa) == window_host_registry[0]);
    assert(window_host(wb) == window_host_registry[1]);

    window_host_registry_remove(&b);
    assert(window_host_registry_count == 1);
    assert(window_host_registry_first() == &a);
    assert(window_host(wa) == &a);
    window_destroy(wb);

    window_host_registry_remove(&a);
    assert(window_host_registry_count == 0);
    assert(window_host_registry_first() == NULL);
    window_destroy(wa);
    free(a.windows);
    free(b.windows);
    input_keymap_free(&a.keymap);
    input_keymap_free(&b.keymap);
    return 0;
}
