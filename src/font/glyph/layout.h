#ifndef FONT_LAYOUT_H
#define FONT_LAYOUT_H

#include <hb.h>
#include <stdbool.h>
#include <stdint.h>

/* What a code point is to the OpenType lookups that a face applies by
 * default. The roles decide which cells of a terminal row need shaping. */
enum layout_role {
    /* No default lookup tests or changes its glyph. */
    LAYOUT_NEUTRAL,
    /* Lookups test its glyph as context. No lookup changes it. */
    LAYOUT_CONTEXT,
    /* A lookup can change or move its glyph. */
    LAYOUT_INPUT,
    /* The analysis does not model it: a mark, a format character, a
     * complex script, or a code point the face does not map. */
    LAYOUT_COMPLEX,
};

#define LAYOUT_CP_MASK 0x1FFFFFu
/* The pair filter covers code points below this. */
#define LAYOUT_PAIR_CPS 256
#define LAYOUT_ROLE_SHIFT 24

/* The default layout of one face, reduced to what a terminal needs to
 * know before it shapes. A layout is immutable once built, so any thread
 * may read it. */
struct font_layout {
    /* The face uses tables or lookups that the analysis does not model.
     * Every run then needs shaping in full. */
    bool opaque;
    /* How many glyphs a lookup tests before and after the glyphs that it
     * changes. */
    uint8_t reach_back, reach_ahead;
    /* The entry of each ASCII code point. See font_layout_entry(). */
    uint32_t ascii[128];
    /* Code points that a rule changes with no context. */
    uint64_t trigger[LAYOUT_PAIR_CPS / 64];
    /* Bit b of pairs[a] is set when some rule of two glyphs that changes
     * glyphs is glyph a then glyph b. chain_pairs holds the adjacent pairs
     * of the longer rules. */
    uint64_t pairs[LAYOUT_PAIR_CPS][LAYOUT_PAIR_CPS / 64];
    uint64_t chain_pairs[LAYOUT_PAIR_CPS][LAYOUT_PAIR_CPS / 64];
    /* Entries of the other mapped code points: an open-addressing table. */
    uint32_t *keys, *values;
    uint32_t mask;
};

/* Builds the layout of face. Never NULL unless memory runs out; a face
 * that the analysis cannot read gives an opaque layout. */
struct font_layout *font_layout_build(hb_face_t *face);
void font_layout_free(struct font_layout *l);

uint32_t font_layout_entry_slow(const struct font_layout *l, uint32_t cp);

/* The role of cp in its top bits, and a stand-in code point in its low
 * bits. Shaping gives the same glyphs for neighboring input code points
 * when a neutral or context code point is replaced by its stand-in. An
 * input or complex code point is its own stand-in. */
static inline uint32_t font_layout_entry(const struct font_layout *l, uint32_t cp)
{
    return cp < 128 ? l->ascii[cp] : font_layout_entry_slow(l, cp);
}

static inline bool layout_pair(const uint64_t (*pairs)[LAYOUT_PAIR_CPS / 64],
                               uint32_t a, uint32_t b)
{
    return (pairs[a][b >> 6] >> (b & 63)) & 1;
}

/* False when no rule can change any glyph of cps: no trigger, no pair of a
 * two-glyph rule, and no two pairs in a row of a longer rule. Every code
 * point must be below LAYOUT_PAIR_CPS. */
static inline bool font_layout_may_change(const struct font_layout *l,
                                          const uint32_t *cps, int len)
{
    bool chain = false;
    for (int i = 0; i < len; i++) {
        uint32_t a = cps[i];
        if ((l->trigger[a >> 6] >> (a & 63)) & 1)
            return true;
        if (i + 1 >= len)
            break;
        uint32_t b = cps[i + 1];
        if (layout_pair(l->pairs, a, b))
            return true;
        bool link = layout_pair(l->chain_pairs, a, b);
        if (link && chain)
            return true;
        chain = link;
    }
    return false;
}

static inline enum layout_role font_layout_entry_role(uint32_t entry)
{
    return (enum layout_role)(entry >> LAYOUT_ROLE_SHIFT);
}

static inline uint32_t font_layout_entry_cp(uint32_t entry)
{
    return entry & LAYOUT_CP_MASK;
}

/* Scripts that HarfBuzz shapes with its default shaper. */
bool font_layout_script_simple(hb_script_t script);

#endif
