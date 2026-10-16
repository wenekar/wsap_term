#ifndef FONT_LAYOUT_H
#define FONT_LAYOUT_H

#include <hb.h>
#include <stdbool.h>
#include <stdint.h>

enum layout_role {
    LAYOUT_NEUTRAL,
    LAYOUT_CONTEXT,
    LAYOUT_INPUT,
    LAYOUT_COMPLEX,
};

#define LAYOUT_CP_MASK 0x1FFFFFu
#define LAYOUT_PAIR_CPS 256
#define LAYOUT_ROLE_SHIFT 24

struct font_layout {
    bool opaque;
    uint8_t reach_back, reach_ahead;
    uint32_t ascii[128];
    uint64_t trigger[LAYOUT_PAIR_CPS / 64];
    uint64_t pairs[LAYOUT_PAIR_CPS][LAYOUT_PAIR_CPS / 64];
    uint64_t chain_pairs[LAYOUT_PAIR_CPS][LAYOUT_PAIR_CPS / 64];
    uint32_t *keys, *values;
    uint32_t mask;
};

struct font_layout *font_layout_build(hb_face_t *face);
void font_layout_free(struct font_layout *l);

uint32_t font_layout_entry_slow(const struct font_layout *l, uint32_t cp);

static inline uint32_t font_layout_entry(const struct font_layout *l, uint32_t cp)
{
    return cp < 128 ? l->ascii[cp] : font_layout_entry_slow(l, cp);
}

static inline bool layout_pair(const uint64_t (*pairs)[LAYOUT_PAIR_CPS / 64],
                               uint32_t a, uint32_t b)
{
    return (pairs[a][b >> 6] >> (b & 63)) & 1;
}

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

bool font_layout_script_simple(hb_script_t script);

#endif
