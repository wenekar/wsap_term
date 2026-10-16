#include <assert.h>
#include "../src/snapshot.c"

int main(void)
{
    struct wsap_vtcore_options options = { .size = { 8, 24 }, .scrollback = 5 };
    struct wsap_vtcore *t = wsap_vtcore_new(&options);
    struct wsap_vtcore_snapshot *s = wsap_vtcore_snapshot_new();
    assert(t && s && wsap_vtcore_snapshot_update(s, t));

    const char line[] = "e\xcc\x81\r\n";
    for (int i = 0; i < 10000; i++) {
        wsap_vtcore_feed(t, line, sizeof(line) - 1, 0);
        assert(wsap_vtcore_snapshot_update(s, t));
    }

    assert(wsap_vtcore_history_rows(t) == 5);
    assert(s->arena_len < 8192);

    wsap_vtcore_snapshot_free(s);
    wsap_vtcore_free(t);
    return 0;
}
