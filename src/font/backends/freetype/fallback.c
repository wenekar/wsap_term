#define _GNU_SOURCE
#include <stdatomic.h>
#include "font/backends/freetype/internal.h"
#include "font/backends/freetype/cards_font.inc"
#include "util/hash.h"

#include <fontconfig/fontconfig.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

struct fallback_candidate {
    FcCharSet *coverage;
    const char *path;
    const unsigned char *mem;
    size_t mem_len;
    int index;
    bool color : 1;
    bool is_emoji : 1;
};

static FcFontSet *fallback_fonts;

static _Atomic bool fallback_ready_flag;

static struct fallback_candidate *fallback_candidates;

static FcCharSet *embedded_charset;
static int fallback_total;

#define FALLBACK_PAGES (UNICODE_LIMIT >> 8)

static uint64_t *fallback_page_index;
static int fallback_words;
static __thread uint64_t *cover_scratch;
static __thread uint64_t *run_set;

#define FALLBACK_HASH_SIZE 4096
struct fallback_hash {
    uint32_t cp[FALLBACK_HASH_SIZE];
    uint64_t *cover[FALLBACK_HASH_SIZE];
};
static __thread struct fallback_hash *fallback_hash;

#define COVER_NIL ((const uint64_t *)(uintptr_t)1)
#define FLAT_TABLE_SIZE 65536
static _Atomic(void *) cover_flat;
static _Atomic(void *) eff_flat;

static void *flat_table(_Atomic(void *) *slot)
{
    void *p = atomic_load_explicit(slot, memory_order_acquire);
    if (p)
        return p;
    p = calloc(FLAT_TABLE_SIZE, sizeof(uint64_t));
    if (!p)
        return NULL;
    void *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(slot, &expected, p,
            memory_order_acq_rel, memory_order_acquire))
        return p;
    free(p);
    return expected;
}
#define EFF_COVER_SIZE 256
struct eff_cover_table {
    uint32_t cp[EFF_COVER_SIZE];
    uint64_t *set[EFF_COVER_SIZE];
};
static __thread struct eff_cover_table eff_cover;

static __thread int *cand_slots;
static __thread unsigned cover_version = 1;

static __thread uint64_t cfg_usable_bits;

#define CFG_COVER_SIZE 2048
#define ANY_COVER_SIZE 4096
struct cp_val_table_cfg {
    uint32_t cp[CFG_COVER_SIZE];
    uint64_t val[CFG_COVER_SIZE];
};
static __thread struct cp_val_table_cfg cfg_cover;

static _Atomic(void *) cfg_cover_flat;

static _Atomic(void *) coverable_flat;
struct cp_val_table_any {
    uint32_t cp[ANY_COVER_SIZE];
    uint64_t val[ANY_COVER_SIZE];
};
static __thread struct cp_val_table_any any_cover;

static void fallback_caches_clear(void)
{
    if (fallback_hash) {
        for (int i = 0; i < FALLBACK_HASH_SIZE; i++)
            free(fallback_hash->cover[i]);
        free(fallback_hash);
        fallback_hash = NULL;
    }
    for (int i = 0; i < EFF_COVER_SIZE; i++)
        free(eff_cover.set[i]);
    memset(&eff_cover, 0, sizeof(eff_cover));
}

static uint64_t *alloc_cover(void)
{
    return calloc((size_t)fallback_words, sizeof(uint64_t));
}

static int cp_map_slot(const uint32_t *keys, unsigned mask, uint32_t cp)
{
    unsigned h = (unsigned)(cp * KNUTH_HASH_MULT) & mask;
    for (unsigned probe = 0; probe <= mask; probe++) {
        unsigned idx = (h + probe) & mask;
        if (keys[idx] == cp + 1 || !keys[idx])
            return (int)idx;
    }
    return -1;
}

static int bitset_next(const uint64_t *set, int from, int n)
{
    int words = (n + 63) >> 6;
    int w = from >> 6;
    if (w < 0 || w >= words)
        return -1;
    uint64_t v = set[w] & (~UINT64_C(0) << (from & 63));
    for (;;) {
        if (v) {
            int i = (w << 6) + __builtin_ctzll(v);
            return i < n ? i : -1;
        }
        if (++w >= words)
            return -1;
        v = set[w];
    }
}

static bool bitset_any(const uint64_t *set, int words)
{
    if (!set)
        return false;
    uint64_t acc = 0;
    for (int w = 0; w < words; w++)
        acc |= set[w];
    return acc != 0;
}

static void bitset_and(uint64_t *dst, const uint64_t *src, int words)
{
#if defined(__AVX2__)
    int w = 0;
    for (; w + 4 <= words; w += 4)
        _mm256_storeu_si256((__m256i *)(dst + w),
            _mm256_and_si256(_mm256_loadu_si256((const __m256i *)(dst + w)),
                             _mm256_loadu_si256((const __m256i *)(src + w))));
    for (; w < words; w++)
        dst[w] &= src[w];
#else
    for (int w = 0; w < words; w++)
        dst[w] &= src[w];
#endif
}

static void build_cover(uint32_t cp, uint64_t *out)
{
    if (!fallback_page_index || !fallback_candidates || fallback_words <= 0 ||
        fallback_total <= 0 || (cp >> 8) >= FALLBACK_PAGES)
        return;
    const uint64_t *row =
        fallback_page_index + (size_t)(cp >> 8) * fallback_words;
    for (int i = bitset_next(row, 0, fallback_total); i >= 0;
         i = bitset_next(row, i + 1, fallback_total)) {
        struct fallback_candidate *c = &fallback_candidates[i];
        if (c->path && FcCharSetHasChar(c->coverage, cp))
            out[i >> 6] |= UINT64_C(1) << (i & 63);
    }
}

static const uint64_t *publish_cover(_Atomic(const uint64_t *) *flat,
                                     uint32_t cp, const uint64_t *val)
{
    const uint64_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(&flat[cp], &expected,
            (uint64_t *)val, memory_order_release, memory_order_acquire))
        return val;
    return expected == COVER_NIL ? NULL : expected;
}

struct cover_fill {
    const uint64_t *set;
    bool cache;
    bool owned;
};

static const uint64_t *flat_cover(_Atomic(const uint64_t *) *flat, uint32_t cp,
        struct cover_fill (*fill)(uint32_t cp, void *ctx), void *ctx)
{
    const uint64_t *s = flat
        ? atomic_load_explicit(&flat[cp], memory_order_acquire) : NULL;
    if (s)
        return s == COVER_NIL ? NULL : s;
    struct cover_fill f = fill(cp, ctx);
    if (!f.set)
        return NULL;
    if (!f.cache || !flat)
        return f.set == COVER_NIL ? NULL : f.set;
    const uint64_t *published = publish_cover(flat, cp, f.set);
    if (published != f.set && f.owned)
        free((void *)f.set);
    return published == COVER_NIL ? NULL : published;
}

static struct cover_fill cover_build(uint32_t cp, void *ctx)
{
    (void)ctx;
    uint64_t *set = alloc_cover();
    if (!set)
        return (struct cover_fill){ NULL, false, false };
    build_cover(cp, set);
    return (struct cover_fill){ set, true, true };
}

static const uint64_t *cover_set(uint32_t cp)
{
    if (!fallback_ready() || cp >= UNICODE_LIMIT)
        return NULL;
    if (cp < 65536)
        return flat_cover(flat_table(&cover_flat), cp, cover_build, NULL);
    if (!fallback_hash)
        fallback_hash = calloc(1, sizeof(*fallback_hash));
    if (fallback_hash) {
        int slot = cp_map_slot(fallback_hash->cp, FALLBACK_HASH_SIZE - 1, cp);
        if (slot >= 0) {
            if (fallback_hash->cp[slot] == cp + 1)
                return fallback_hash->cover[slot];
            uint64_t *set = alloc_cover();
            if (set) {
                build_cover(cp, set);
                fallback_hash->cover[slot] = set;
                fallback_hash->cp[slot] = cp + 1;
                return set;
            }
        }
    }
    if (!cover_scratch) {
        cover_scratch = calloc((size_t)fallback_words + 1, sizeof(*cover_scratch));
        if (!cover_scratch)
            return NULL;
    }
    memset(cover_scratch, 0, (size_t)fallback_words * sizeof(*cover_scratch));
    build_cover(cp, cover_scratch);
    return cover_scratch;
}

hb_unicode_funcs_t *hb_ufuncs(void)
{
    static hb_unicode_funcs_t *uf;
    if (!uf)
        uf = hb_unicode_funcs_get_default();
    return uf;
}

static bool cp_is_space_separator(uint32_t cp)
{
    return hb_unicode_general_category(hb_ufuncs(), cp) ==
           HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR;
}

static bool decompose_cover_alternates(uint32_t cp, int depth, hb_codepoint_t *a,
                            hb_codepoint_t *b, bool *space)
{
    *a = 0;
    *b = 0;
    *space = cp_is_space_separator(cp);
    bool dec = hb_unicode_decompose(hb_ufuncs(), cp, a, b);
    return depth < 8 && (dec || *space);
}

static bool face_covers(FT_Face ft, uint32_t cp, int depth)
{
    if (FT_Get_Char_Index(ft, cp))
        return true;
    hb_codepoint_t a, b;
    bool space;
    if (!decompose_cover_alternates(cp, depth, &a, &b, &space))
        return false;
    if (space && FT_Get_Char_Index(ft, ' '))
        return true;
    return a && face_covers(ft, a, depth + 1) &&
           (!b || face_covers(ft, b, depth + 1));
}

static const uint64_t *effective_cover_set(uint32_t cp, int depth);

static void cover_merge(uint64_t *set, const uint64_t *raw, hb_codepoint_t a,
                        hb_codepoint_t b, bool space, int depth)
{
    if (raw)
        memcpy(set, raw, (size_t)fallback_words * sizeof(*set));
    if (a) {
        const uint64_t *sa = effective_cover_set(a, depth + 1);
        const uint64_t *sb = b ? effective_cover_set(b, depth + 1) : NULL;
        if (sa && (!b || sb))
            for (int w = 0; w < fallback_words; w++)
                set[w] |= sa[w] & (b ? sb[w] : ~UINT64_C(0));
    }
    if (space) {
        const uint64_t *sp = cover_set(' ');
        if (sp)
            for (int w = 0; w < fallback_words; w++)
                set[w] |= sp[w];
    }
}

static struct cover_fill eff_build(uint32_t cp, void *ctx)
{
    (void)ctx;
    hb_codepoint_t a, b;
    bool space;
    const uint64_t *raw = cover_set(cp);
    if (!decompose_cover_alternates(cp, 0, &a, &b, &space))
        return (struct cover_fill){ raw ? raw : COVER_NIL, true, false };
    uint64_t *set = alloc_cover();
    if (!set)
        return (struct cover_fill){ raw, false, false };
    cover_merge(set, raw, a, b, space, 0);
    return (struct cover_fill){ set, true, true };
}

static const uint64_t *effective_cover_set(uint32_t cp, int depth)
{
    hb_codepoint_t a, b;
    bool space;
    const uint64_t *raw;
    uint64_t *set;

    if (depth == 0 && cp < 65536)
        return flat_cover(flat_table(&eff_flat), cp, eff_build, NULL);

    raw = cover_set(cp);
    if (!decompose_cover_alternates(cp, depth, &a, &b, &space))
        return raw;
    int slot = cp_map_slot(eff_cover.cp, EFF_COVER_SIZE - 1, cp);
    if (slot < 0)
        return raw;
    if (eff_cover.cp[slot] == cp + 1)
        return eff_cover.set[slot];
    set = alloc_cover();
    if (!set)
        return raw;
    eff_cover.set[slot] = set;
    eff_cover.cp[slot] = cp + 1;
    cover_merge(set, raw, a, b, space, depth);
    return set;
}

bool fallback_ready(void)
{
    return atomic_load_explicit(&fallback_ready_flag, memory_order_acquire);
}

void fallback_destroy(void)
{
    if (embedded_charset) {
        FcCharSetDestroy(embedded_charset);
        embedded_charset = NULL;
    }
    _Atomic(const uint64_t *) *ef = atomic_exchange_explicit(&eff_flat, NULL,
        memory_order_relaxed);
    _Atomic(const uint64_t *) *cf = atomic_exchange_explicit(&cover_flat, NULL,
        memory_order_relaxed);
    for (int i = 0; i < FLAT_TABLE_SIZE; i++) {
        const uint64_t *e = ef
            ? atomic_load_explicit(&ef[i], memory_order_relaxed) : NULL;
        const uint64_t *c = cf
            ? atomic_load_explicit(&cf[i], memory_order_relaxed) : NULL;
        if (e && e != COVER_NIL && e != c) free((void *)e);
        if (c && c != COVER_NIL) free((void *)c);
    }
    free(ef);
    free(cf);
    free(fallback_candidates);
    fallback_candidates = NULL;
    free(fallback_page_index);
    fallback_page_index = NULL;
    free(cover_scratch);
    cover_scratch = NULL;
    free(run_set);
    run_set = NULL;
    fallback_words = 0;
    atomic_store_explicit(&fallback_ready_flag, false, memory_order_release);
    if (fallback_fonts) FcFontSetDestroy(fallback_fonts);
    fallback_fonts = NULL;
    fallback_total = 0;
    fallback_caches_clear();
}

void fallback_thread_reset(void)
{
    memset(&cfg_cover, 0, sizeof(cfg_cover));
    memset(&any_cover, 0, sizeof(any_cover));
    cfg_usable_bits = 0;
    free(cover_scratch);
    cover_scratch = NULL;
    free(run_set);
    run_set = NULL;
    free(cand_slots);
    cand_slots = NULL;
    fallback_caches_clear();
}

void fallback_coverage_reset(void)
{
    memset(&cfg_cover, 0, sizeof(cfg_cover));
    free(atomic_exchange_explicit(&cfg_cover_flat, NULL, memory_order_relaxed));
    free(atomic_exchange_explicit(&coverable_flat, NULL, memory_order_relaxed));
    memset(&any_cover, 0, sizeof(any_cover));
    cfg_usable_bits = 0;
}

static bool init_embedded_charset(void)
{
    FT_Face probe;
    if (FT_New_Memory_Face(ft_lib, embedded_cards_font,
                           (FT_Long)sizeof(embedded_cards_font), 0, &probe))
        return false;
    FcCharSet *cs = FcCharSetCreate();
    if (cs) {
        FT_ULong cp;
        FT_UInt gid;
        for (cp = FT_Get_First_Char(probe, &gid); gid;
             cp = FT_Get_Next_Char(probe, cp, &gid))
            FcCharSetAddChar(cs, (FcChar32)cp);
    }
    FT_Done_Face(probe);
    if (!cs)
        return false;
    embedded_charset = cs;
    return true;
}

static bool fallback_init_locked(void)
{
    FcPattern *pat = FcPatternCreate();
    if (!pat)
        return false;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"monospace");
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    fallback_fonts = FcFontSort(NULL, pat, FcFalse, NULL, &result);
    FcPatternDestroy(pat);

    if (!fallback_fonts || result != FcResultMatch) {
        if (fallback_fonts)
            FcFontSetDestroy(fallback_fonts);
        fallback_fonts = NULL;
        return false;
    }
    int nfont = fallback_fonts->nfont;
    int embedded = init_embedded_charset() ? 1 : 0;
    fallback_total = nfont + embedded;
    fallback_words = (fallback_total + 63) / 64;
    fallback_candidates = calloc((size_t)fallback_total + 1,
                                 sizeof(*fallback_candidates));
    cover_scratch = calloc((size_t)fallback_words + 1, sizeof(*cover_scratch));
    run_set = calloc((size_t)fallback_words + 1, sizeof(*run_set));
    fallback_page_index = calloc((size_t)FALLBACK_PAGES * fallback_words,
                                 sizeof(*fallback_page_index));
    if (!fallback_candidates || !cover_scratch || !run_set || !fallback_page_index)
        goto fail;

    for (int i = 0; i < fallback_total; i++) {
        struct fallback_candidate *candidate = &fallback_candidates[i];
        if (i < embedded) {
            candidate->coverage = embedded_charset;
            candidate->path = embedded_cards_path;
            candidate->mem = embedded_cards_font;
            candidate->mem_len = sizeof(embedded_cards_font);
        } else {
            FcPattern *match = fallback_fonts->fonts[i - embedded];
            FcChar8 *path;
            FcBool color = FcFalse;
            if (FcPatternGetCharSet(match, FC_CHARSET, 0, &candidate->coverage) != FcResultMatch ||
                FcPatternGetString(match, FC_FILE, 0, &path) != FcResultMatch)
                continue;
            if (strstr((const char *)path, "LastResort"))
                continue;
            candidate->path = (const char *)path;
            FcPatternGetInteger(match, FC_INDEX, 0, &candidate->index);
            FcPatternGetBool(match, FC_COLOR, 0, &color);
            candidate->color = color;
            candidate->is_emoji = pattern_is_emoji(match);
        }
        if (candidate->coverage) {
            FcChar32 map[FC_CHARSET_MAP_SIZE], next;
            for (FcChar32 base = FcCharSetFirstPage(candidate->coverage, map, &next);
                 base != FC_CHARSET_DONE;
                 base = FcCharSetNextPage(candidate->coverage, map, &next)) {
                unsigned page = base >> 8;
                if (page < FALLBACK_PAGES)
                    fallback_page_index[(size_t)page * fallback_words +
                                        (i >> 6)] |= UINT64_C(1) << (i & 63);
            }
        }
    }
    fallback_caches_clear();
    atomic_store_explicit(&fallback_ready_flag, true, memory_order_release);
    return true;
fail:
    fallback_destroy();
    return false;
}

bool fallback_init(void)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    bool ok = fallback_ready() ? true : fallback_init_locked();
    pthread_mutex_unlock(&mu);
    return ok;
}

static bool candidate_in_slot(const struct fallback_candidate *candidate, int slot)
{
    return candidate && candidate->path && slot >= 0 && slot < num_faces && faces[slot].path &&
           faces[slot].index == candidate->index &&
           !strcmp(faces[slot].path, candidate->path);
}

static int candidate_face_slot(const struct fallback_candidate *candidate)
{
    if (!candidate || !fallback_candidates || fallback_total <= 0)
        return -1;
    int ci = (int)(candidate - fallback_candidates);
    if (ci < 0 || ci >= fallback_total)
        return -1;
    if (!cand_slots) {
        cand_slots = malloc((size_t)fallback_total * sizeof(*cand_slots));
        if (cand_slots)
            for (int i = 0; i < fallback_total; i++) cand_slots[i] = -1;
    }
    int memo = cand_slots ? cand_slots[ci] : -1;
    if (candidate_in_slot(candidate, memo))
        return memo;
    for (int j = 0; j < num_faces; j++) {
        if (candidate_in_slot(candidate, j)) {
            if (cand_slots) cand_slots[ci] = j;
            return j;
        }
    }
    int slot = candidate->mem
        ? load_embedded_face(candidate->mem, candidate->mem_len,
                        candidate->is_emoji)
        : ft_load_face(candidate->path, candidate->index,
                       candidate->is_emoji);
    if (cand_slots) cand_slots[ci] = slot;
    return slot;
}

bool fallback_coverage_exempt(uint32_t cp)
{
    return cp == 0x00AD || cp == 0x034F || cp == 0x061C ||
           (cp >= 0x115F && cp <= 0x1160) ||
           (cp >= 0x17B4 && cp <= 0x17B5) ||
           (cp >= 0x180B && cp <= 0x180F) ||
           (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x2028 && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x206F) ||
           cp == 0x3164 || cp == 0xFEFF || cp == 0xFFA0 ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xFFF0 && cp <= 0xFFF8) ||
           (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
           (cp >= 0x1D173 && cp <= 0x1D17A) ||
           (cp >= 0xE0000 && cp <= 0xE0FFF);
}

static void compute_covering_set(const uint32_t *cps, int len)
{
    cover_version++;
    if (!run_set) {
        run_set = calloc((size_t)fallback_words + 1, sizeof(*run_set));
        if (!run_set)
            return;
    }
    memset(run_set, ~0, (size_t)fallback_words * sizeof(*run_set));
    for (int k = 0; k < len; k++) {
        const uint64_t *s;
        if (k == 0) {

            s = cover_set(cps[0]);
        } else {
            if (!needs_coverage(k, cps[k]))
                continue;

            s = effective_cover_set(cps[k], 0);
        }
        if (s)
            bitset_and(run_set, s, fallback_words);
        else
            memset(run_set, 0, (size_t)fallback_words * sizeof(*run_set));
        if (!bitset_any(run_set, fallback_words))
            return;
    }
}

bool fallback_run_covered(const uint32_t *cps, int len, unsigned *gen)
{
    if (!fallback_ready() && !fallback_init()) {
        *gen = cover_version;
        return false;
    }
    compute_covering_set(cps, len);
    *gen = cover_version;
    return bitset_any(run_set, fallback_words);
}

static void ensure_cover_set(struct font_face_query *q, const uint32_t *cps,
                             int len)
{
    if (q->cover_seen != cover_version) {
        compute_covering_set(cps, len);
        q->cover_seen = cover_version;
    }
}

struct face_iter_ops {
    bool (*accept_configured)(struct font_face_query *q, int i,
                              const uint32_t *cps, int len, void *ctx);
    const uint64_t *(*fallback_set)(struct font_face_query *q,
                                    const uint32_t *cps, int len);
    int (*accept_fallback)(struct font_face_query *q, int i,
                           const struct fallback_candidate *c, void *ctx);
};

static inline bool is_cfg_phase(const struct font_face_query *q)
{
    return q->prefer_emoji ? q->phase < 2 : (q->phase == 0 || q->phase == 3);
}

static inline bool phase_emoji(const struct font_face_query *q)
{
    return q->prefer_emoji ? (q->phase != 1) : (q->phase >= 2);
}

static inline int next_phase(const struct font_face_query *q)
{
    return q->prefer_emoji && q->phase == 2 ? 4 : q->phase + 1;
}

static int face_iter_next(struct font_face_query *q, const uint32_t *cps,
                          int len, const struct face_iter_ops *ops, void *ctx)
{
    if (!q || !ft_lib || q->cp >= UNICODE_LIMIT || len < 1)
        return -1;
    while (q->phase < 4) {
        if (is_cfg_phase(q)) {
            while (q->idx < configured_faces) {
                int i = q->idx++;
                if (ops->accept_configured(q, i, cps, len, ctx))
                    return i;
            }
        } else {
            if (!fallback_ready() && !fallback_init())
                return -1;
            ensure_cover_set(q, cps, len);
            const uint64_t *set = ops->fallback_set(q, cps, len);
            while (q->idx < fallback_total) {
                int i = set ? bitset_next(set, q->idx, fallback_total) : -1;
                if (i < 0)
                    break;
                q->idx = i + 1;
                int slot = ops->accept_fallback(q, i,
                                                &fallback_candidates[i], ctx);
                if (slot == -2)
                    return -1;
                if (slot >= 0)
                    return slot;
            }
        }
        q->phase = next_phase(q);
        q->idx = 0;
    }
    return -1;
}

static bool covering_accept_cfg(struct font_face_query *q, int i,
                                const uint32_t *cps, int len, void *ctx)
{
    (void)cps; (void)len; (void)ctx;
    if (faces[i].is_emoji != phase_emoji(q))
        return false;
    ensure_primary_face(i);
    return faces[i].styles[0] != NULL;
}

static const uint64_t *covering_fb_set(struct font_face_query *q,
                                       const uint32_t *cps, int len)
{
    (void)q; (void)cps; (void)len;
    return run_set;
}

static int covering_accept_fb(struct font_face_query *q, int i,
                              const struct fallback_candidate *c, void *ctx)
{
    (void)i; (void)ctx;
    bool any_color = q->prefer_emoji && q->phase == 2;
    if (!any_color && c->color != phase_emoji(q))
        return -1;
    return candidate_face_slot(c);
}

int font_next_face_covering(struct font_face_query *q,
                            const uint32_t *cps, int len)
{
    static const struct face_iter_ops ops = {
        covering_accept_cfg, covering_fb_set, covering_accept_fb,
    };
    return face_iter_next(q, cps, len, &ops, NULL);
}

static bool run_accept_cfg(struct font_face_query *q, int i,
                           const uint32_t *cps, int len, void *vctx)
{
    uint16_t flags = (uint16_t)(uintptr_t)vctx;
    if (faces[i].is_emoji != phase_emoji(q))
        return false;
    ensure_primary_face(i);
    if (!faces[i].styles[0])
        return false;
    if (q->phase == 3)
        return true;

    for (int j = 0; j < i; j++)
        if (!faces[j].is_emoji) {
            ensure_primary_face(j);
            if (faces[j].styles[0]) {
                q->vfail = true;
                break;
            }
        }
    bool ok = i < 64 && ((q->cfg_mask >> i) & 1);
    if (!ok) {
        enum font_variant want = font_variant_of(flags);
        if (want && pick_face_style(i, cps, len, want))
            ok = true;
    }
    if (ok)
        return true;
    q->vfail = true;
    return false;
}

static const uint64_t *run_fb_set(struct font_face_query *q,
                                  const uint32_t *cps, int len)
{
    (void)q; (void)len;
    return cover_set(cps[0]);
}

static int run_accept_fb(struct font_face_query *q, int i,
                         const struct fallback_candidate *c, void *vctx)
{
    (void)vctx;
    if (!c->path || c->color != phase_emoji(q))
        return -1;
    int slot = candidate_face_slot(c);
    if (slot < 0 || faces[slot].is_emoji || slot < configured_faces || !run_set)
        return -1;
    if ((run_set[i >> 6] >> (i & 63)) & 1)
        return slot;
    q->vfail = true;
    return -1;
}

int font_next_face_for_run(struct font_face_query *q, const uint32_t *cps,
                           int len, uint16_t flags)
{
    static const struct face_iter_ops ops = {
        run_accept_cfg, run_fb_set, run_accept_fb,
    };
    return face_iter_next(q, cps, len, &ops, (void *)(uintptr_t)flags);
}

static uint64_t cfg_cover_compute(uint32_t cp)
{
    uint64_t m = 0;
    for (int i = 0; i < configured_faces; i++) {
        if (faces[i].is_emoji)
            continue;
        ensure_primary_face(i);
        if (faces[i].styles[0] && face_covers(faces[i].styles[0], cp, 0))
            m |= UINT64_C(1) << i;
    }
    return m;
}

static uint64_t cp_cache(uint32_t cp, _Atomic uint64_t *flat,
                         uint32_t *keys, uint64_t *vals, unsigned hash_mask,
                         uint64_t miss, uint64_t (*compute)(uint32_t))
{
    if (cp < 65536) {
        if (!flat)
            return compute(cp);
        uint64_t v = atomic_load_explicit(&flat[cp], memory_order_acquire);
        if (v)
            return v - 1;
        uint64_t m = compute(cp);
        atomic_store_explicit(&flat[cp], m + 1, memory_order_release);
        return m;
    }
    int slot = cp_map_slot(keys, hash_mask, cp);
    if (slot < 0)
        return miss;
    if (keys[slot] == cp + 1)
        return vals[slot];
    vals[slot] = compute(cp);
    keys[slot] = cp + 1;
    return vals[slot];
}

static uint64_t cfg_cover_mask(uint32_t cp)
{
    if (cp >= UNICODE_LIMIT)
        return 0;
    if (configured_faces > 63)
        return UINT64_MAX;
    return cp_cache(cp, flat_table(&cfg_cover_flat), cfg_cover.cp,
                    cfg_cover.val, CFG_COVER_SIZE - 1, UINT64_MAX,
                    cfg_cover_compute);
}

static uint64_t cp_coverable_slow(uint32_t cp)
{
    if ((cfg_cover_mask(cp) & cfg_usable_bits) != 0)
        return 1;
    if (!fallback_ready() && !fallback_init())
        return 0;
    const uint64_t *s = effective_cover_set(cp, 0);
    return s && bitset_any(s, fallback_words);
}

static bool cp_coverable(uint32_t cp)
{
    if (cp >= UNICODE_LIMIT)
        return false;
    return cp_cache(cp, flat_table(&coverable_flat), any_cover.cp,
                    any_cover.val, ANY_COVER_SIZE - 1, 1,
                    cp_coverable_slow) != 0;
}

void build_cfg_usable(void)
{
    if (!cfg_usable_bits)
        for (int i = 0; i < configured_faces && i < 64; i++)
            if (!faces[i].is_emoji) {
                ensure_primary_face(i);
                if (faces[i].styles[0])
                    cfg_usable_bits |= UINT64_C(1) << i;
            }
}

static uint64_t scan_run_coverage(const uint32_t *cps, int len, uint64_t *hash,
                        int *dead_at)
{
    *dead_at = -1;
    build_cfg_usable();
    uint64_t cfg_and = cfg_usable_bits;
    for (int k = 0; k < len; k++) {
        *hash = hash64_step(*hash, cps[k]);
        if (!needs_coverage(k, cps[k]))
            continue;
        if (!cp_coverable(cps[k])) {
            *dead_at = k;
            return 0;
        }
        if (cfg_and)
            cfg_and &= cfg_cover_mask(cps[k]);
    }
    return cfg_and;
}

uint64_t ft_scan_run(const uint32_t *cps, int len, uint16_t style,
                     uint64_t *hash, int *dead_at)
{
    uint64_t h = hash64_step(HASH64_BASIS, style);
    uint64_t cfg_and = scan_run_coverage(cps, len, &h, dead_at);
    if (hash)
        *hash = h;
    return cfg_and;
}

bool run_coverable(const uint32_t *cps, int len)
{
    uint64_t cfg_and = ~UINT64_C(0);
    if (configured_faces <= 64) {
        for (int k = 0; k < len && cfg_and; k++)
            if (needs_coverage(k, cps[k]))
                cfg_and &= cfg_cover_mask(cps[k]);
        if (cfg_and)
            return true;
    }
    if (!fallback_ready() && !fallback_init())
        return cfg_and != 0;
    compute_covering_set(cps, len);
    return bitset_any(run_set, fallback_words);
}
