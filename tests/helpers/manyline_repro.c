#include <wsap_vtcore/wsap_vtcore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int cols = argc > 1 ? atoi(argv[1]) : 80;
    int rows = argc > 2 ? atoi(argv[2]) : 24;
    size_t chunk = argc > 3 ? (size_t)atol(argv[3]) : 1u << 20;
    int iters = argc > 4 ? atoi(argv[4]) : 64;
    unsigned seed = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : 12345;

    char *buf = malloc(chunk);
    if (!buf) return 1;
    unsigned state = seed;
    for (size_t i = 0; i < chunk; i++) {
        state = state * 1103515245u + 12345u;
        unsigned pick = (state >> 16) % 27;
        buf[i] = pick == 26 ? '\n' : (char)('a' + pick);
    }

    struct wsap_vtcore *t = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = cols, .y = rows },
        .scrollback = 4096,
    });
    if (!t) return 1;

    /* Bare LFs also return to column 0, as a tty with ONLCR does. */
    for (int it = 0; it < iters; it++) {
        wsap_vtcore_feed(t, buf, chunk, WSAP_VTCORE_FEED_ONLCR);
        if ((it & 7) == 0)
            fprintf(stderr, "iter %d ok (sb=%d)\n", it, wsap_vtcore_scrollback(t).total);
    }

    /* Read every row of the viewport, as a renderer does. */
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    if (!snap || !wsap_vtcore_snapshot_update(snap, t)) return 1;
    for (int r = 0; r < wsap_vtcore_size(t).y; r++)
        (void)wsap_vtcore_snapshot_row(snap, r);
    wsap_vtcore_snapshot_free(snap);

    fprintf(stderr, "done sb=%d\n", wsap_vtcore_scrollback(t).total);
    wsap_vtcore_free(t);
    free(buf);
    return 0;
}
