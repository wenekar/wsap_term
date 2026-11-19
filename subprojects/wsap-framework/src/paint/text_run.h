#ifndef WSAP_UI_TEXT_RUN_H
#define WSAP_UI_TEXT_RUN_H

#include "util/unicode/utf8.h"
#include "util/unicode/width.h"

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

    int n = unicode_utf8_decode_one(text, len, cp);
    if (n <= 0) {
        *cp = UI_TEXT_REPLACEMENT;
        n = 1;
    }
    int w = unicode_width(*cp);
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
        size_t span = unicode_utf8_printable_span(text + off, len - off, 0x7f);
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

#endif
