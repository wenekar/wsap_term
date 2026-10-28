#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef WSAP_VTCORE_AMALGAMATION
#include "wsap_vtcore.h"
#else
#include <wsap_vtcore/wsap_vtcore.h>
#endif

static int write_codepoint(uint32_t cp)
{
    unsigned char out[4];
    size_t len;
    if (cp <= 0x7f) {
        out[0] = (unsigned char)cp;
        len = 1;
    } else if (cp <= 0x7ff) {
        out[0] = (unsigned char)(0xc0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        len = 2;
    } else if (cp <= 0xffff) {
        out[0] = (unsigned char)(0xe0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        len = 3;
    } else {
        out[0] = (unsigned char)(0xf0 | (cp >> 18));
        out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (unsigned char)(0x80 | (cp & 0x3f));
        len = 4;
    }
    return fwrite(out, 1, len, stdout) == len;
}

static int print_snapshot(const struct wsap_vtcore_snapshot *snapshot)
{
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(snapshot);
    if (!view)
        return 0;
    for (int y = 0; y < view->size.y; y++) {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snapshot, y);
        if (!row)
            return 0;
        for (int x = 0; x < view->size.x; x++) {
            const struct wsap_vtcore_cell *cell = &row[x];
            if (!cell->width) continue;
            if (cell->text & WSAP_VTCORE_TEXT_CLUSTER) {
                int count = wsap_vtcore_snapshot_cluster(snapshot, cell->text, NULL, 0);
                if (count <= 0) return 0;
                uint32_t *cps = malloc((size_t)count * sizeof(*cps));
                if (!cps) return 0;
                int copied = wsap_vtcore_snapshot_cluster(snapshot, cell->text, cps, count);
                int ok = copied == count;
                for (int i = 0; ok && i < count; i++)
                    ok = write_codepoint(cps[i]);
                free(cps);
                if (!ok) return 0;
            } else if (!write_codepoint(cell->text ? cell->text : ' ')) {
                return 0;
            }
        }
        if (fputc('\n', stdout) == EOF) return 0;
    }
    return fflush(stdout) == 0;
}

int main(void)
{
    struct wsap_vtcore_options options = {0};
    options.scrollback = 1;
    struct wsap_vtcore *term = wsap_vtcore_new(&options);
    if (!term) {
        fputs("stdin-snapshot: cannot create terminal\n", stderr);
        return EXIT_FAILURE;
    }
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    if (!snapshot) {
        fputs("stdin-snapshot: cannot create snapshot\n", stderr);
        wsap_vtcore_free(term);
        return EXIT_FAILURE;
    }

    char input[4096];
    int status = EXIT_SUCCESS;
    for (;;) {
        size_t len = fread(input, 1, sizeof(input), stdin);
        if (len && !wsap_vtcore_feed(term, input, len, 0)) {
            fputs("stdin-snapshot: terminal input error\n", stderr);
            status = EXIT_FAILURE;
            break;
        }
        if (len < sizeof(input)) {
            if (ferror(stdin)) {
                fputs("stdin-snapshot: input error\n", stderr);
                status = EXIT_FAILURE;
            }
            break;
        }
    }
    if (status == EXIT_SUCCESS &&
        (!wsap_vtcore_snapshot_update(snapshot, term) || !print_snapshot(snapshot))) {
        fputs("stdin-snapshot: snapshot or output error\n", stderr);
        status = EXIT_FAILURE;
    }
    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
    return status;
}
