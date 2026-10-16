#include "font/glyph/layout.h"

#include <hb-aat.h>
#include <hb-ot.h>
#include <stdlib.h>
#include <string.h>

#define REACH_MAX 8

struct table {
    const uint8_t *data;
    unsigned len;
    hb_tag_t tag;
};

typedef uint64_t ascii_mask[LAYOUT_PAIR_CPS / 64];

struct analysis {
    struct table t;
    struct font_layout *out;
    uint64_t *sig;
    uint64_t *input;
    hb_set_t *seen_sets;
    hb_set_t *lookups;
    hb_set_t *pending;
    uint64_t next_id;
    unsigned ascii_gid[LAYOUT_PAIR_CPS];
    ascii_mask ascii_all;
    unsigned glyphs;
    unsigned back, ahead;
    bool direct;
    bool bad;
};

static unsigned u16(struct analysis *a, unsigned off)
{
    if (off + 2 > a->t.len) {
        a->bad = true;
        return 0;
    }
    return (unsigned)a->t.data[off] << 8 | a->t.data[off + 1];
}

static unsigned u32(struct analysis *a, unsigned off)
{
    return u16(a, off) << 16 | u16(a, off + 2);
}

static uint64_t mix64(uint64_t x)
{
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    return x ^ (x >> 31);
}

static void add_glyph(struct analysis *a, unsigned g, uint64_t id, bool input)
{
    if (g >= a->glyphs)
        return;
    a->sig[g] += id;
    if (input)
        a->input[g >> 6] |= UINT64_C(1) << (g & 63);
}

static void note_reach(struct analysis *a, unsigned back, unsigned ahead)
{
    if (back > a->back) a->back = back;
    if (ahead > a->ahead) a->ahead = ahead;
}

static bool first_visit(struct analysis *a, unsigned off, unsigned kind)
{
    hb_codepoint_t key = (hb_codepoint_t)off << 2 | kind;
    if (hb_set_has(a->seen_sets, key))
        return false;
    hb_set_add(a->seen_sets, key);
    return true;
}

static void visit_coverage(struct analysis *a, unsigned off, bool input)
{
    if (!input && !first_visit(a, off, 0))
        return;
    uint64_t id = mix64(++a->next_id);
    unsigned format = u16(a, off);
    if (format == 1) {
        unsigned n = u16(a, off + 2);
        for (unsigned i = 0; i < n && !a->bad; i++)
            add_glyph(a, u16(a, off + 4 + 2 * i), id, input);
    } else if (format == 2) {
        unsigned n = u16(a, off + 2);
        for (unsigned i = 0; i < n && !a->bad; i++) {
            unsigned r = off + 4 + 6 * i;
            unsigned first = u16(a, r), last = u16(a, r + 2);
            for (unsigned g = first; g <= last && g < a->glyphs; g++)
                add_glyph(a, g, id, input);
        }
    } else {
        a->bad = true;
    }
}

static void visit_classdef(struct analysis *a, unsigned off)
{
    if (!off || !first_visit(a, off, 1))
        return;
    uint64_t base = mix64(++a->next_id);
    unsigned format = u16(a, off);
    if (format == 1) {
        unsigned first = u16(a, off + 2), n = u16(a, off + 4);
        for (unsigned i = 0; i < n && !a->bad; i++) {
            unsigned c = u16(a, off + 6 + 2 * i);
            if (c)
                add_glyph(a, first + i, mix64(base ^ c), false);
        }
    } else if (format == 2) {
        unsigned n = u16(a, off + 2);
        for (unsigned i = 0; i < n && !a->bad; i++) {
            unsigned r = off + 4 + 6 * i;
            unsigned first = u16(a, r), last = u16(a, r + 2), c = u16(a, r + 4);
            for (unsigned g = first; c && g <= last && g < a->glyphs; g++)
                add_glyph(a, g, mix64(base ^ c), false);
        }
    } else {
        a->bad = true;
    }
}

static void visit_glyph(struct analysis *a, unsigned g)
{
    add_glyph(a, g, mix64(UINT64_C(0xA5A5) << 32 | g), false);
}

static int coverage_index(struct analysis *a, unsigned off, unsigned g)
{
    unsigned format = u16(a, off), n = u16(a, off + 2);
    if (!n || a->bad)
        return -1;
    if (format == 1) {
        unsigned first = u16(a, off + 4);
        unsigned last = u16(a, off + 4 + 2 * (n - 1));
        if (g < first || g > last || a->bad)
            return -1;
        unsigned lo = 0, hi = n;
        while (lo < hi && !a->bad) {
            unsigned mid = (lo + hi) / 2;
            unsigned v = u16(a, off + 4 + 2 * mid);
            if (v == g) return (int)mid;
            if (v < g) lo = mid + 1; else hi = mid;
        }
        return -1;
    }
    if (format == 2) {
        unsigned first = u16(a, off + 4);
        unsigned last = u16(a, off + 4 + 6 * (n - 1) + 2);
        if (g < first || g > last || a->bad)
            return -1;
        unsigned lo = 0, hi = n;
        while (lo < hi && !a->bad) {
            unsigned mid = (lo + hi) / 2;
            unsigned r = off + 4 + 6 * mid;
            unsigned rfirst = u16(a, r), rlast = u16(a, r + 2);
            if (g >= rfirst && g <= rlast) return (int)(u16(a, r + 4) + g - rfirst);
            if (rlast < g) lo = mid + 1; else hi = mid;
        }
        return -1;
    }
    a->bad = true;
    return -1;
}

static unsigned classdef_get(struct analysis *a, unsigned off, unsigned g)
{
    if (!off)
        return 0;
    unsigned format = u16(a, off);
    if (format == 1) {
        unsigned first = u16(a, off + 2), n = u16(a, off + 4);
        return g >= first && g - first < n ? u16(a, off + 6 + 2 * (g - first)) : 0;
    }
    if (format != 2) {
        a->bad = true;
        return 0;
    }
    unsigned hi = u16(a, off + 2);
    if (!hi || a->bad)
        return 0;
    unsigned first = u16(a, off + 4);
    unsigned last = u16(a, off + 4 + 6 * (hi - 1) + 2);
    if (g < first || g > last || a->bad)
        return 0;
    unsigned lo = 0;
    while (lo < hi && !a->bad) {
        unsigned mid = (lo + hi) / 2, r = off + 4 + 6 * mid;
        unsigned rfirst = u16(a, r), rlast = u16(a, r + 2);
        if (g >= rfirst && g <= rlast) return u16(a, r + 4);
        if (rlast < g) lo = mid + 1; else hi = mid;
    }
    return 0;
}

enum elem_kind { ELEM_COVERAGE, ELEM_CLASS, ELEM_GLYPH, ELEM_ALL };

static void elem_mask(struct analysis *a, enum elem_kind kind, unsigned off,
                      unsigned value, ascii_mask m)
{
    if (kind == ELEM_ALL) {
        memcpy(m, a->ascii_all, sizeof(ascii_mask));
        return;
    }
    memset(m, 0, sizeof(ascii_mask));
    switch (kind) {
    case ELEM_COVERAGE:
        for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++) {
            uint64_t bits = a->ascii_all[w];
            while (bits) {
                int bit = __builtin_ctzll(bits);
                unsigned cp = (unsigned)(w * 64 + bit);
                bits &= bits - 1;
                if (coverage_index(a, off, a->ascii_gid[cp]) >= 0)
                    m[w] |= UINT64_C(1) << bit;
            }
        }
        break;
    case ELEM_CLASS:
        for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++) {
            uint64_t bits = a->ascii_all[w];
            while (bits) {
                int bit = __builtin_ctzll(bits);
                unsigned cp = (unsigned)(w * 64 + bit);
                bits &= bits - 1;
                if (classdef_get(a, off, a->ascii_gid[cp]) == value)
                    m[w] |= UINT64_C(1) << bit;
            }
        }
        break;
    case ELEM_GLYPH:
        for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++) {
            uint64_t bits = a->ascii_all[w];
            while (bits) {
                int bit = __builtin_ctzll(bits);
                unsigned cp = (unsigned)(w * 64 + bit);
                bits &= bits - 1;
                if (a->ascii_gid[cp] == value)
                    m[w] |= UINT64_C(1) << bit;
            }
        }
        break;
    case ELEM_ALL:
        break;
    }
}

static void note_sequence(struct analysis *a, const ascii_mask *seq, int n)
{
    if (!a->direct || n <= 0)
        return;
    struct font_layout *l = a->out;
    if (n == 1) {
        for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++)
            l->trigger[w] |= seq[0][w];
        return;
    }
    uint64_t (*pairs)[LAYOUT_PAIR_CPS / 64] = n == 2 ? l->pairs : l->chain_pairs;
    for (int k = 0; k + 1 < n; k++) {
        const uint64_t *next = seq[k + 1];
        if (!next[0] && !next[1] && !next[2] && !next[3])
            continue;
        for (int w0 = 0; w0 < LAYOUT_PAIR_CPS / 64; w0++) {
            uint64_t bits = seq[k][w0];
            while (bits) {
                int bit = __builtin_ctzll(bits);
                unsigned cp = (unsigned)(w0 * 64 + bit);
                bits &= bits - 1;
                for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++)
                    pairs[cp][w] |= next[w];
            }
        }
    }
}

#define SEQ_MAX 64

static void visit_records(struct analysis *a, unsigned off, unsigned count)
{
    for (unsigned i = 0; i < count && !a->bad; i++) {
        unsigned lookup = u16(a, off + 4 * i + 2);
        if (!hb_set_has(a->lookups, lookup)) {
            hb_set_add(a->lookups, lookup);
            hb_set_add(a->pending, lookup);
        }
    }
}

static void visit_context(struct analysis *a, unsigned st)
{
    unsigned format = u16(a, st);
    if (format == 1 || format == 2) {
        visit_coverage(a, st + u16(a, st + 2), true);
        unsigned head = format == 1 ? 4 : 6;
        if (format == 2)
            visit_classdef(a, st + u16(a, st + 4));
        unsigned sets = u16(a, st + head);
        for (unsigned s = 0; s < sets && !a->bad; s++) {
            unsigned so = u16(a, st + head + 2 + 2 * s);
            if (!so) continue;
            unsigned set = st + so, rules = u16(a, set);
            unsigned cov = st + u16(a, st + 2);
            unsigned cd = format == 2 ? st + u16(a, st + 4) : 0;
            for (unsigned r = 0; r < rules && !a->bad; r++) {
                unsigned rule = set + u16(a, set + 2 + 2 * r);
                unsigned glyphs = u16(a, rule), records = u16(a, rule + 2);
                note_reach(a, 0, glyphs ? glyphs - 1 : 0);
                for (unsigned i = 1; i < glyphs && format == 1; i++)
                    visit_glyph(a, u16(a, rule + 4 + 2 * (i - 1)));
                visit_records(a, rule + 4 + 2 * (glyphs ? glyphs - 1 : 0), records);
                if (!records || !a->direct || glyphs > SEQ_MAX)
                    continue;
                ascii_mask seq[SEQ_MAX];
                elem_mask(a, ELEM_COVERAGE, cov, 0, seq[0]);
                if (format == 2) {
                    ascii_mask c;
                    elem_mask(a, ELEM_CLASS, cd, s, c);
                    for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++)
                        seq[0][w] &= c[w];
                }
                for (unsigned i = 1; i < glyphs; i++) {
                    unsigned v = u16(a, rule + 4 + 2 * (i - 1));
                    elem_mask(a, format == 1 ? ELEM_GLYPH : ELEM_CLASS, cd, v, seq[i]);
                }
                note_sequence(a, seq, (int)glyphs);
            }
        }
    } else if (format == 3) {
        unsigned glyphs = u16(a, st + 2), records = u16(a, st + 4);
        note_reach(a, 0, glyphs ? glyphs - 1 : 0);
        for (unsigned i = 0; i < glyphs && !a->bad; i++)
            visit_coverage(a, st + u16(a, st + 6 + 2 * i), i == 0);
        visit_records(a, st + 6 + 2 * glyphs, records);
        if (records && a->direct && glyphs <= SEQ_MAX) {
            ascii_mask seq[SEQ_MAX];
            for (unsigned i = 0; i < glyphs; i++)
                elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 6 + 2 * i), 0, seq[i]);
            note_sequence(a, seq, (int)glyphs);
        }
    } else {
        a->bad = true;
    }
}

static void visit_chain(struct analysis *a, unsigned st)
{
    unsigned format = u16(a, st);
    if (format == 1 || format == 2) {
        visit_coverage(a, st + u16(a, st + 2), true);
        unsigned head = 4;
        if (format == 2) {
            for (int k = 0; k < 3; k++) {
                unsigned cd = u16(a, st + 4 + 2 * k);
                if (cd)
                    visit_classdef(a, st + cd);
            }
            head = 10;
        }
        unsigned sets = u16(a, st + head);
        for (unsigned s = 0; s < sets && !a->bad; s++) {
            unsigned so = u16(a, st + head + 2 + 2 * s);
            if (!so) continue;
            unsigned set = st + so, rules = u16(a, set);
            unsigned cov = st + u16(a, st + 2);
            unsigned cds[3] = {0};
            for (int k = 0; format == 2 && k < 3; k++) {
                unsigned cd = u16(a, st + 4 + 2 * k);
                cds[k] = cd ? st + cd : 0;
            }
            for (unsigned r = 0; r < rules && !a->bad; r++) {
                unsigned p = set + u16(a, set + 2 + 2 * r);
                unsigned rule = p;
                unsigned back = u16(a, p);
                for (unsigned i = 0; i < back && format == 1; i++)
                    visit_glyph(a, u16(a, p + 2 + 2 * i));
                p += 2 + 2 * back;
                unsigned input = u16(a, p);
                for (unsigned i = 1; i < input && format == 1; i++)
                    visit_glyph(a, u16(a, p + 2 + 2 * (i - 1)));
                p += 2 + 2 * (input ? input - 1 : 0);
                unsigned ahead = u16(a, p);
                for (unsigned i = 0; i < ahead && format == 1; i++)
                    visit_glyph(a, u16(a, p + 2 + 2 * i));
                p += 2 + 2 * ahead;
                note_reach(a, back, (input ? input - 1 : 0) + ahead);
                unsigned records = u16(a, p);
                visit_records(a, p + 2, records);
                if (!records || !a->direct || back + input + ahead > SEQ_MAX || !input)
                    continue;
                ascii_mask seq[SEQ_MAX];
                enum elem_kind kind = format == 1 ? ELEM_GLYPH : ELEM_CLASS;
                unsigned q = rule;
                for (unsigned i = 0; i < back; i++)
                    elem_mask(a, kind, cds[0], u16(a, q + 2 + 2 * i), seq[back - 1 - i]);
                q += 2 + 2 * back;
                elem_mask(a, ELEM_COVERAGE, cov, 0, seq[back]);
                if (format == 2) {
                    ascii_mask c;
                    elem_mask(a, ELEM_CLASS, cds[1], s, c);
                    for (int w = 0; w < LAYOUT_PAIR_CPS / 64; w++)
                        seq[back][w] &= c[w];
                }
                for (unsigned i = 1; i < input; i++)
                    elem_mask(a, kind, cds[1], u16(a, q + 2 + 2 * (i - 1)), seq[back + i]);
                q += 2 + 2 * (input - 1);
                for (unsigned i = 0; i < ahead; i++)
                    elem_mask(a, kind, cds[2], u16(a, q + 2 + 2 * i), seq[back + input + i]);
                note_sequence(a, seq, (int)(back + input + ahead));
            }
        }
    } else if (format == 3) {
        unsigned p = st + 2;
        unsigned back = u16(a, p);
        for (unsigned i = 0; i < back && !a->bad; i++)
            visit_coverage(a, st + u16(a, p + 2 + 2 * i), false);
        p += 2 + 2 * back;
        unsigned input = u16(a, p);
        for (unsigned i = 0; i < input && !a->bad; i++)
            visit_coverage(a, st + u16(a, p + 2 + 2 * i), i == 0);
        p += 2 + 2 * input;
        unsigned ahead = u16(a, p);
        for (unsigned i = 0; i < ahead && !a->bad; i++)
            visit_coverage(a, st + u16(a, p + 2 + 2 * i), false);
        p += 2 + 2 * ahead;
        note_reach(a, back, (input ? input - 1 : 0) + ahead);
        unsigned records = u16(a, p);
        visit_records(a, p + 2, records);
        if (records && a->direct && input && back + input + ahead <= SEQ_MAX) {
            ascii_mask seq[SEQ_MAX];
            unsigned q = st + 2;
            for (unsigned i = 0; i < back; i++)
                elem_mask(a, ELEM_COVERAGE, st + u16(a, q + 2 + 2 * i), 0, seq[back - 1 - i]);
            q += 2 + 2 * back;
            for (unsigned i = 0; i < input; i++)
                elem_mask(a, ELEM_COVERAGE, st + u16(a, q + 2 + 2 * i), 0, seq[back + i]);
            q += 2 + 2 * input;
            for (unsigned i = 0; i < ahead; i++)
                elem_mask(a, ELEM_COVERAGE, st + u16(a, q + 2 + 2 * i), 0,
                          seq[back + input + i]);
            note_sequence(a, seq, (int)(back + input + ahead));
        }
    } else {
        a->bad = true;
    }
}

static unsigned value_size(unsigned format)
{
    return 2 * (unsigned)__builtin_popcount(format & 0xFF);
}

static void visit_gsub(struct analysis *a, unsigned type, unsigned st)
{
    switch (type) {
    case 1: case 2: case 3: {
        visit_coverage(a, st + u16(a, st + 2), true);
        ascii_mask seq[1];
        elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 2), 0, seq[0]);
        note_sequence(a, seq, 1);
        break;
    }
    case 4: {
        visit_coverage(a, st + u16(a, st + 2), true);
        unsigned sets = u16(a, st + 4);
        ascii_mask first;
        elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 2), 0, first);
        for (unsigned s = 0; s < sets && !a->bad; s++) {
            unsigned set = st + u16(a, st + 6 + 2 * s), ligs = u16(a, set);
            for (unsigned l = 0; l < ligs && !a->bad; l++) {
                unsigned lig = set + u16(a, set + 2 + 2 * l);
                unsigned comps = u16(a, lig + 2);
                note_reach(a, 0, comps ? comps - 1 : 0);
                for (unsigned c = 1; c < comps && !a->bad; c++)
                    add_glyph(a, u16(a, lig + 4 + 2 * (c - 1)), 0, true);
                if (!a->direct || comps > SEQ_MAX || !comps)
                    continue;
                ascii_mask seq[SEQ_MAX];
                memcpy(seq[0], first, sizeof(ascii_mask));
                for (unsigned c = 1; c < comps; c++)
                    elem_mask(a, ELEM_GLYPH, 0, u16(a, lig + 4 + 2 * (c - 1)), seq[c]);
                note_sequence(a, seq, (int)comps);
            }
        }
        break;
    }
    case 5:
        visit_context(a, st);
        break;
    case 6:
        visit_chain(a, st);
        break;
    case 8: {
        visit_coverage(a, st + u16(a, st + 2), true);
        unsigned p = st + 4, back = u16(a, p);
        for (unsigned i = 0; i < back && !a->bad; i++)
            visit_coverage(a, st + u16(a, p + 2 + 2 * i), false);
        p += 2 + 2 * back;
        unsigned ahead = u16(a, p);
        for (unsigned i = 0; i < ahead && !a->bad; i++)
            visit_coverage(a, st + u16(a, p + 2 + 2 * i), false);
        note_reach(a, back, ahead);
        if (a->direct && back + 1 + ahead <= SEQ_MAX) {
            ascii_mask seq[SEQ_MAX];
            unsigned q = st + 4;
            for (unsigned i = 0; i < back; i++)
                elem_mask(a, ELEM_COVERAGE, st + u16(a, q + 2 + 2 * i), 0, seq[back - 1 - i]);
            elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 2), 0, seq[back]);
            q += 2 + 2 * back;
            for (unsigned i = 0; i < ahead; i++)
                elem_mask(a, ELEM_COVERAGE, st + u16(a, q + 2 + 2 * i), 0, seq[back + 1 + i]);
            note_sequence(a, seq, (int)(back + 1 + ahead));
        }
        break;
    }
    default:
        a->bad = true;
    }
}

static void visit_gpos(struct analysis *a, unsigned type, unsigned st)
{
    ascii_mask seq[2];
    switch (type) {
    case 1:
        visit_coverage(a, st + u16(a, st + 2), true);
        elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 2), 0, seq[0]);
        note_sequence(a, seq, 1);
        break;
    case 2: {
        unsigned format = u16(a, st);
        visit_coverage(a, st + u16(a, st + 2), true);
        elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 2), 0, seq[0]);
        elem_mask(a, ELEM_ALL, 0, 0, seq[1]);
        note_sequence(a, seq, 2);
        unsigned vf1 = u16(a, st + 4), vf2 = u16(a, st + 6);
        if (vf2) {
            a->bad = true;
            break;
        }
        note_reach(a, 0, 1);
        if (format == 1) {
            unsigned sets = u16(a, st + 8);
            unsigned record = 2 + value_size(vf1);
            for (unsigned s = 0; s < sets && !a->bad; s++) {
                unsigned set = st + u16(a, st + 10 + 2 * s), n = u16(a, set);
                for (unsigned i = 0; i < n && !a->bad; i++)
                    visit_glyph(a, u16(a, set + 2 + record * i));
            }
        } else if (format == 2) {
            visit_classdef(a, st + u16(a, st + 10));
        } else {
            a->bad = true;
        }
        break;
    }
    case 3:
        visit_coverage(a, st + u16(a, st + 2), true);
        note_reach(a, 1, 1);
        elem_mask(a, ELEM_COVERAGE, st + u16(a, st + 2), 0, seq[0]);
        memcpy(seq[1], seq[0], sizeof(ascii_mask));
        note_sequence(a, seq, 2);
        break;
    case 4: case 5: case 6:
        break;
    case 7:
        visit_context(a, st);
        break;
    case 8:
        visit_chain(a, st);
        break;
    default:
        a->bad = true;
    }
}

static void visit_lookup(struct analysis *a, unsigned index)
{
    unsigned list = u16(a, 8);
    if (index >= u16(a, list)) {
        a->bad = true;
        return;
    }
    unsigned lookup = list + u16(a, list + 2 + 2 * index);
    unsigned type = u16(a, lookup), flags = u16(a, lookup + 2);
    unsigned subtables = u16(a, lookup + 4);
    if (flags & 0x0002) {
        a->bad = true;
        return;
    }
    bool gsub = a->t.tag == HB_OT_TAG_GSUB;
    for (unsigned s = 0; s < subtables && !a->bad; s++) {
        unsigned st = lookup + u16(a, lookup + 6 + 2 * s);
        unsigned t = type;
        if ((gsub && type == 7) || (!gsub && type == 9)) {
            t = u16(a, st + 2);
            st += u32(a, st + 4);
        }
        if (gsub)
            visit_gsub(a, t, st);
        else
            visit_gpos(a, t, st);
    }
}

static const hb_script_t simple_scripts[] = {
    HB_SCRIPT_COMMON, HB_SCRIPT_LATIN, HB_SCRIPT_GREEK, HB_SCRIPT_CYRILLIC,
    HB_SCRIPT_ARMENIAN, HB_SCRIPT_GEORGIAN, HB_SCRIPT_HAN, HB_SCRIPT_HIRAGANA,
    HB_SCRIPT_KATAKANA, HB_SCRIPT_BOPOMOFO, HB_SCRIPT_RUNIC, HB_SCRIPT_OGHAM,
    HB_SCRIPT_BRAILLE, HB_SCRIPT_YI,
};

bool font_layout_script_simple(hb_script_t script)
{
    switch (script) {
    case HB_SCRIPT_INHERITED:
    case HB_SCRIPT_UNKNOWN:
    case HB_SCRIPT_COMMON:
    case HB_SCRIPT_LATIN:
    case HB_SCRIPT_GREEK:
    case HB_SCRIPT_CYRILLIC:
    case HB_SCRIPT_ARMENIAN:
    case HB_SCRIPT_GEORGIAN:
    case HB_SCRIPT_HAN:
    case HB_SCRIPT_HIRAGANA:
    case HB_SCRIPT_KATAKANA:
    case HB_SCRIPT_BOPOMOFO:
    case HB_SCRIPT_RUNIC:
    case HB_SCRIPT_OGHAM:
    case HB_SCRIPT_BRAILLE:
    case HB_SCRIPT_YI:
        return true;
    default:
        return false;
    }
}

static bool cp_complex(hb_unicode_funcs_t *uf, uint32_t cp)
{
    switch (hb_unicode_general_category(uf, cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
    case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
    case HB_UNICODE_GENERAL_CATEGORY_SURROGATE:
    case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
    case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
        return true;
    default:
        break;
    }
    return cp == 0x2044 || hb_unicode_combining_class(uf, cp) != 0 ||
           !font_layout_script_simple(hb_unicode_script(uf, cp));
}

static const hb_feature_t fraction_off[] = {
    { HB_TAG('f', 'r', 'a', 'c'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END },
    { HB_TAG('n', 'u', 'm', 'r'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END },
    { HB_TAG('d', 'n', 'o', 'm'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END },
};

static void collect_lookups(hb_face_t *face, hb_tag_t table, hb_set_t *out)
{
    for (size_t i = 0; i < sizeof(simple_scripts) / sizeof(simple_scripts[0]); i++) {
        hb_segment_properties_t props = {
            .direction = HB_DIRECTION_LTR,
            .script = simple_scripts[i],
            .language = hb_language_get_default(),
        };
        hb_shape_plan_t *plan = hb_shape_plan_create_cached(face, &props, fraction_off,
            sizeof(fraction_off) / sizeof(fraction_off[0]), NULL);
        hb_ot_shape_plan_collect_lookups(plan, table, out);
        hb_shape_plan_destroy(plan);
    }
}

static bool analyze_table(struct analysis *a, hb_face_t *face, hb_tag_t tag)
{
    hb_blob_t *blob = hb_face_reference_table(face, tag);
    a->t = (struct table){ .tag = tag };
    a->t.data = (const uint8_t *)hb_blob_get_data(blob, &a->t.len);
    hb_set_clear(a->lookups);
    hb_set_clear(a->pending);
    hb_set_clear(a->seen_sets);
    if (a->t.len) {
        collect_lookups(face, tag, a->lookups);
        hb_set_t *direct = hb_set_copy(a->lookups);
        hb_set_union(a->pending, a->lookups);
        hb_codepoint_t l;
        while (!a->bad && (l = hb_set_get_min(a->pending)) != HB_SET_VALUE_INVALID) {
            hb_set_del(a->pending, l);
            a->direct = hb_set_has(direct, l);
            visit_lookup(a, l);
        }
        hb_set_destroy(direct);
    }
    hb_blob_destroy(blob);
    return !a->bad;
}

static bool face_has_table(hb_face_t *face, hb_tag_t tag)
{
    hb_blob_t *blob = hb_face_reference_table(face, tag);
    bool has = hb_blob_get_length(blob) > 0;
    hb_blob_destroy(blob);
    return has;
}

struct rep_slot {
    uint64_t key;
    uint32_t cp;
};

static uint64_t rep_key(uint64_t sig, unsigned gdef, unsigned gc)
{
    return mix64(sig ^ ((uint64_t)gdef << 40 | (uint64_t)gc << 48)) | 1;
}

static struct rep_slot *rep_find(struct rep_slot *slots, uint32_t mask, uint64_t key)
{
    for (uint32_t i = (uint32_t)key & mask;; i = (i + 1) & mask)
        if (slots[i].key == key || !slots[i].key)
            return &slots[i];
}

static void map_put(struct font_layout *l, uint32_t cp, uint32_t entry)
{
    for (uint32_t i = (cp * 2654435761u) & l->mask;; i = (i + 1) & l->mask) {
        if (!l->keys[i] || l->keys[i] == cp) {
            l->keys[i] = cp;
            l->values[i] = entry;
            return;
        }
    }
}

uint32_t font_layout_entry_slow(const struct font_layout *l, uint32_t cp)
{
    if (l->mask) {
        for (uint32_t i = (cp * 2654435761u) & l->mask;; i = (i + 1) & l->mask) {
            if (l->keys[i] == cp)
                return l->values[i];
            if (!l->keys[i])
                break;
        }
    }
    return (uint32_t)LAYOUT_COMPLEX << LAYOUT_ROLE_SHIFT | (cp & LAYOUT_CP_MASK);
}

static uint32_t entry(enum layout_role role, uint32_t cp)
{
    return (uint32_t)role << LAYOUT_ROLE_SHIFT | (cp & LAYOUT_CP_MASK);
}

static void make_opaque(struct font_layout *l)
{
    l->opaque = true;
    memset(l->pairs, 0xFF, sizeof(l->pairs));
    memset(l->chain_pairs, 0xFF, sizeof(l->chain_pairs));
    memset(l->trigger, 0xFF, sizeof(l->trigger));
    for (uint32_t cp = 0; cp < 128; cp++)
        l->ascii[cp] = entry(LAYOUT_COMPLEX, cp);
}

static bool fill_map(struct font_layout *l, hb_face_t *face, const struct analysis *a)
{
    hb_map_t *mapping = hb_map_create();
    hb_set_t *unicodes = hb_set_create();
    hb_face_collect_nominal_glyph_mapping(face, mapping, unicodes);
    unsigned count = hb_set_get_population(unicodes);
    uint32_t mask = 1;
    while (mask < count * 2 + 16) mask <<= 1;
    struct rep_slot *reps = calloc(mask, sizeof(*reps));
    l->mask = mask - 1;
    l->keys = calloc(mask, sizeof(*l->keys));
    l->values = calloc(mask, sizeof(*l->values));
    bool ok = reps && l->keys && l->values && hb_map_allocation_successful(mapping);
    hb_unicode_funcs_t *uf = hb_unicode_funcs_get_default();

    for (int pass = 0; ok && pass < 2; pass++) {
        hb_codepoint_t cp = HB_SET_VALUE_INVALID;
        while (hb_set_next(unicodes, &cp)) {
            if (cp > LAYOUT_CP_MASK || cp <= ' ')
                continue;
            hb_codepoint_t g = hb_map_get(mapping, cp);
            unsigned gdef = hb_ot_layout_get_glyph_class(face, g);
            enum layout_role role;
            if (g >= a->glyphs || cp_complex(uf, cp) ||
                gdef == HB_OT_LAYOUT_GLYPH_CLASS_MARK ||
                gdef == HB_OT_LAYOUT_GLYPH_CLASS_LIGATURE)
                role = LAYOUT_COMPLEX;
            else if (a->input[g >> 6] >> (g & 63) & 1)
                role = LAYOUT_INPUT;
            else
                role = a->sig[g] ? LAYOUT_CONTEXT : LAYOUT_NEUTRAL;
            uint32_t stand_in = cp;
            if (role == LAYOUT_CONTEXT || role == LAYOUT_NEUTRAL) {
                uint64_t key = rep_key(a->sig[g], gdef,
                                       hb_unicode_general_category(uf, cp));
                struct rep_slot *slot = rep_find(reps, mask - 1, key);
                if (!slot->key)
                    *slot = (struct rep_slot){ .key = key, .cp = cp };
                stand_in = slot->cp;
            }
            if (pass == 0)
                continue;
            if (cp < 128)
                l->ascii[cp] = entry(role, stand_in);
            else
                map_put(l, cp, entry(role, stand_in));
        }
    }
    free(reps);
    hb_map_destroy(mapping);
    hb_set_destroy(unicodes);
    return ok;
}

struct font_layout *font_layout_build(hb_face_t *face)
{
    struct font_layout *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    for (uint32_t cp = 0; cp < 128; cp++)
        l->ascii[cp] = entry(LAYOUT_COMPLEX, cp);
    if (!face || hb_aat_layout_has_substitution(face) ||
        hb_aat_layout_has_positioning(face) || hb_aat_layout_has_tracking(face) ||
        face_has_table(face, HB_TAG('k', 'e', 'r', 'n'))) {
        make_opaque(l);
        return l;
    }

    struct analysis a = { .glyphs = hb_face_get_glyph_count(face), .out = l };
    hb_font_t *font = hb_font_create(face);
    for (unsigned cp = 33; cp < LAYOUT_PAIR_CPS; cp++) {
        hb_codepoint_t g = 0;
        if (cp != 127 && hb_font_get_nominal_glyph(font, cp, &g)) {
            a.ascii_gid[cp] = g;
            a.ascii_all[cp >> 6] |= UINT64_C(1) << (cp & 63);
        }
    }
    hb_font_destroy(font);
    a.sig = calloc(a.glyphs ? a.glyphs : 1, sizeof(*a.sig));
    a.input = calloc(a.glyphs / 64 + 1, sizeof(*a.input));
    a.seen_sets = hb_set_create();
    a.lookups = hb_set_create();
    a.pending = hb_set_create();
    bool ok = a.sig && a.input && analyze_table(&a, face, HB_OT_TAG_GSUB) &&
              analyze_table(&a, face, HB_OT_TAG_GPOS) &&
              a.back <= REACH_MAX && a.ahead <= REACH_MAX;
    if (ok) {
        l->reach_back = (uint8_t)a.back;
        l->reach_ahead = (uint8_t)a.ahead;
        ok = fill_map(l, face, &a);
    }
    if (!ok) {
        free(l->keys);
        free(l->values);
        l->keys = l->values = NULL;
        l->mask = 0;
        make_opaque(l);
    }
    hb_set_destroy(a.seen_sets);
    hb_set_destroy(a.lookups);
    hb_set_destroy(a.pending);
    free(a.sig);
    free(a.input);
    return l;
}

void font_layout_free(struct font_layout *l)
{
    if (!l)
        return;
    free(l->keys);
    free(l->values);
    free(l);
}
