#ifndef WSAP_UI_TEXT_RUN_H
#define WSAP_UI_TEXT_RUN_H

#include "wsap_vtcore/wsap_vtcore.h"
#include "wsap_vtcore/unicode/unicode.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <stddef.h>
#include <stdint.h>

#define UI_TEXT_REPLACEMENT 0xFFFDu

static inline int ui_text_run_next(const char *text, size_t len,
                                   uint32_t *cp, int *cells)
{
    unsigned char b0 = (unsigned char)text[0];
    if (__builtin_expect(b0 >= 0x20 && b0 <= 0x7E, 1)) {
        *cp = b0;
        *cells = 1;
        return 1;
    }

    int n = wsap_vtcore_utf8_decode_one(text, len, cp);
    if (n <= 0) {
        *cp = UI_TEXT_REPLACEMENT;
        n = 1;
    }
    int w = wsap_vtcore_unicode_width(*cp);
    if (w < 0) {
        *cp = UI_TEXT_REPLACEMENT;
        w = 1;
    }
    *cells = w;
    return n;
}

static inline int ui_text_run_cells(const char *text, size_t len)
{
    size_t off = 0;
    int cells = 0;
    while (off < len) {
        /* Fast path: scan printable ASCII span [0x20, 0x7e] using AVX-512 / AVX2 / NEON */
        size_t span = wsap_vtcore_scan_printable_span(text + off, len - off, 0x7f);
        if (span > 0) {
            cells += (int)span;
            off += span;
            if (off >= len)
                break;
        }
        uint32_t cp;
        int w;
        off += (size_t)ui_text_run_next(text + off, len - off, &cp, &w);
        cells += w;
    }
    return cells;
}

static inline size_t ui_text_run_fit(const char *text, size_t len, int cap)
{
    size_t off = 0;
    int used = 0;
    while (off < len) {
        size_t rem = (size_t)(cap - used);
        if (rem == 0)
            break;
        size_t limit = len - off < rem ? len - off : rem;
        size_t span = wsap_vtcore_scan_printable_span(text + off, limit, 0x7f);
        if (span > 0) {
            used += (int)span;
            off += span;
            if (used >= cap || off >= len)
                break;
        }
        uint32_t cp;
        int cells;
        int n = ui_text_run_next(text + off, len - off, &cp, &cells);
        if (used + cells > cap)
            break;
        used += cells;
        off += (size_t)n;
    }
    return off;
}

#endif
