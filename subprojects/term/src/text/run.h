#ifndef WSAP_VTCORE_TEXT_RUN_H
#define WSAP_VTCORE_TEXT_RUN_H

#include "cell.h"
#include "unicode/unicode.h"
#include "unicode/grapheme.h"

#define UTF8_LEAD_MIN 0xc2
#define UTF8_LEAD_MAX 0xf4
#define UTF8_LEAD3_TAG 0xE0
#define UTF8_LEAD3_MASK 0xF0

#define WSAP_VTCORE_WIDTH_SLOTS 4
struct width_cache {
    uint32_t lo[WSAP_VTCORE_WIDTH_SLOTS];
    uint32_t span[WSAP_VTCORE_WIDTH_SLOTS];
    uint8_t klass;
    uint8_t next;
};

bool wsap_vtcore_tables_ready(void);
void wsap_vtcore_text_tables_init(void);

int wsap_vtcore_text_class(uint32_t cp);

struct text_run_result {
    int bytes;
    int cols;
    uint32_t last;
};

struct text_run_result wsap_vtcore_text_run(struct cell *restrict row, const char *restrict buf,
                                    int len, int max_cols, uint32_t fg, uint32_t bg,
                                    uint32_t flags, uint32_t last, struct width_cache *restrict wc);

struct text_run_bytes {
    int bytes;
    int cols;
    int out;
    int chars;
    uint32_t last;
    bool wide, narrow;
};

struct text_run_bytes wsap_vtcore_text_run_bytes(uint8_t *restrict dst, const char *restrict buf, int len,
                                          int max_cols, uint32_t last, struct width_cache *restrict wc);

static inline bool joins_cluster(uint32_t cp, int klass)
{
    if (cp < 0x0300)
        return false;
    if (klass == 0 || wsap_vtcore_grapheme_emoji_modifier(cp))
        return true;

    return klass == 1 && wsap_vtcore_unicode_spacing_mark(cp);
}

static inline bool can_extend_cluster(uint32_t last_char, uint32_t cp, int klass)
{
    if (klass == 0) return true;
    if (last_char == 0x200D || wsap_vtcore_grapheme_is_handle(last_char)) return true;
    if (cp - 0x1F1E6u <= 0x19u && wsap_vtcore_grapheme_regional_indicator(cp))
        return true;
    return joins_cluster(cp, klass);
}

static inline bool cluster_contextual(uint32_t cp)
{
    return wsap_vtcore_unicode_spacing_mark(cp) || wsap_vtcore_grapheme_emoji_modifier(cp) ||
           (cp - 0x1F1E6u <= 0x19u && wsap_vtcore_grapheme_regional_indicator(cp));
}

static inline bool cluster_joins(const uint32_t *cps, int len,
                                 uint32_t cp, int klass)
{
    if (joins_cluster(cp, klass))
        return true;
    if (cps[len - 1] == 0x200D)
        return wsap_vtcore_unicode_extended_pictographic(cp);
    return len == 1 && cp - 0x1F1E6u <= 0x19u &&
           wsap_vtcore_grapheme_regional_indicator(cp) &&
           wsap_vtcore_grapheme_regional_indicator(cps[0]);
}

#endif
