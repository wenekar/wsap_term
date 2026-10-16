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
static __thread uint64_t *run_set;

static __thread int *cand_slots;
static __thread unsigned cover_version = 1;

static __thread uint64_t cfg_usable_bits;

#define CP_KNOWN (UINT64_C(1) << 63)
#define CP_COVERABLE (UINT64_C(1) << 62)
#define CP_CFG_MASK (CP_COVERABLE - 1)
#define CP_CFG_FACES 62

static _Atomic(_Atomic uint64_t *) cp_pages[FALLBACK_PAGES];

static const uint64_t *page_row(uint32_t cp)
{
    return fallback_page_index + (size_t)(cp >> 8) * fallback_words;
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

static bool has_cover_alternates(uint32_t cp)
{
    hb_codepoint_t a, b;
    bool space;
    return decompose_cover_alternates(cp, 0, &a, &b, &space);
}

typedef bool (*has_char_fn)(void *font, uint32_t cp);

static bool ft_has_char(void *font, uint32_t cp)
{
    return FT_Get_Char_Index((FT_Face)font, cp) != 0;
}

static bool fc_has_char(void *font, uint32_t cp)
{
    return FcCharSetHasChar((FcCharSet *)font, cp);
}

static bool font_covers(has_char_fn has, void *font, uint32_t cp, int depth)
{
    if (has(font, cp))
        return true;
    hb_codepoint_t a, b;
    bool space;
    if (!decompose_cover_alternates(cp, depth, &a, &b, &space))
        return false;
    if (space && has(font, ' '))
        return true;
    return a && font_covers(has, font, a, depth + 1) &&
           (!b || font_covers(has, font, b, depth + 1));
}

static bool candidate_covers_run(const struct fallback_candidate *c,
                                 const uint32_t *cps, int len)
{
    if (!c->path || !FcCharSetHasChar(c->coverage, cps[0]))
        return false;
    for (int k = 1; k < len; k++)
        if (needs_coverage(k, cps[k]) &&
            !font_covers(fc_has_char, c->coverage, cps[k], 0))
            return false;
    return true;
}

static bool fallback_covers(uint32_t cp)
{
    const uint64_t *row = has_cover_alternates(cp) ? NULL : page_row(cp);
    for (int i = 0; i < fallback_total; i++) {
        if (row && (i = bitset_next(row, i, fallback_total)) < 0)
            break;
        const struct fallback_candidate *c = &fallback_candidates[i];
        if (c->path && font_covers(fc_has_char, c->coverage, cp, 0))
            return true;
    }
    return false;
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
    free(fallback_candidates);
    fallback_candidates = NULL;
    free(fallback_page_index);
    fallback_page_index = NULL;
    free(run_set);
    run_set = NULL;
    fallback_words = 0;
    atomic_store_explicit(&fallback_ready_flag, false, memory_order_release);
    if (fallback_fonts) FcFontSetDestroy(fallback_fonts);
    fallback_fonts = NULL;
    fallback_total = 0;
}

void fallback_thread_reset(void)
{
    cfg_usable_bits = 0;
    free(run_set);
    run_set = NULL;
    free(cand_slots);
    cand_slots = NULL;
}

void fallback_coverage_reset(void)
{
    for (size_t i = 0; i < FALLBACK_PAGES; i++)
        if (atomic_load_explicit(&cp_pages[i], memory_order_relaxed))
            free((void *)atomic_exchange_explicit(&cp_pages[i], NULL,
                                                  memory_order_relaxed));
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
    run_set = calloc((size_t)fallback_words + 1, sizeof(*run_set));
    fallback_page_index = calloc((size_t)FALLBACK_PAGES * fallback_words,
                                 sizeof(*fallback_page_index));
    if (!fallback_candidates || !run_set || !fallback_page_index)
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
        if (k > 0 && (!needs_coverage(k, cps[k]) || has_cover_alternates(cps[k])))
            continue;
        if (cps[k] >= UNICODE_LIMIT) {
            memset(run_set, 0, (size_t)fallback_words * sizeof(*run_set));
            return;
        }
        bitset_and(run_set, page_row(cps[k]), fallback_words);
        if (!bitset_any(run_set, fallback_words))
            return;
    }
}

static int first_candidate_covering(const uint32_t *cps, int len)
{
    compute_covering_set(cps, len);
    if (!run_set)
        return -1;
    for (int i = bitset_next(run_set, 0, fallback_total); i >= 0;
         i = bitset_next(run_set, i + 1, fallback_total))
        if (candidate_covers_run(&fallback_candidates[i], cps, len))
            return i;
    return -1;
}

bool fallback_run_covered(const uint32_t *cps, int len)
{
    if (!fallback_ready() && !fallback_init())
        return false;
    return first_candidate_covering(cps, len) >= 0;
}

struct face_iter_ops {
    bool (*accept_configured)(struct font_face_query *q, int i,
                              const uint32_t *cps, int len, void *ctx);
    const uint64_t *(*fallback_set)(struct font_face_query *q,
                                    const uint32_t *cps, int len);
    int (*accept_fallback)(struct font_face_query *q, int i,
                           const struct fallback_candidate *c,
                           const uint32_t *cps, int len, void *ctx);
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
            const uint64_t *set = ops->fallback_set(q, cps, len);
            while (q->idx < fallback_total) {
                int i = set ? bitset_next(set, q->idx, fallback_total) : -1;
                if (i < 0)
                    break;
                q->idx = i + 1;
                int slot = ops->accept_fallback(q, i, &fallback_candidates[i],
                                                cps, len, ctx);
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
    if (q->cover_seen != cover_version) {
        compute_covering_set(cps, len);
        q->cover_seen = cover_version;
    }
    return run_set;
}

static int covering_accept_fb(struct font_face_query *q, int i,
                              const struct fallback_candidate *c,
                              const uint32_t *cps, int len, void *ctx)
{
    (void)i; (void)ctx;
    bool any_color = q->prefer_emoji && q->phase == 2;
    if (!any_color && c->color != phase_emoji(q))
        return -1;
    if (!candidate_covers_run(c, cps, len))
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
    return cps[0] < UNICODE_LIMIT ? page_row(cps[0]) : NULL;
}

static int run_accept_fb(struct font_face_query *q, int i,
                         const struct fallback_candidate *c,
                         const uint32_t *cps, int len, void *vctx)
{
    (void)i; (void)vctx;
    if (!c->path || c->color != phase_emoji(q) ||
        !FcCharSetHasChar(c->coverage, cps[0]))
        return -1;
    int slot = candidate_face_slot(c);
    if (slot < 0 || faces[slot].is_emoji || slot < configured_faces)
        return -1;
    if (candidate_covers_run(c, cps, len))
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

static _Atomic uint64_t *cp_page(uint32_t cp)
{
    _Atomic(_Atomic uint64_t *) *slot = &cp_pages[cp >> 8];
    _Atomic uint64_t *page = atomic_load_explicit(slot, memory_order_acquire);
    if (page)
        return page;
    page = calloc(256, sizeof(*page));
    if (!page)
        return NULL;
    _Atomic uint64_t *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(slot, &expected, page,
            memory_order_acq_rel, memory_order_acquire))
        return page;
    free((void *)page);
    return expected;
}

static uint64_t cp_info_compute(uint32_t cp)
{
    uint64_t cfg = UINT64_MAX;
    if (configured_faces <= CP_CFG_FACES) {
        cfg = 0;
        for (int i = 0; i < configured_faces; i++) {
            if (faces[i].is_emoji)
                continue;
            ensure_primary_face(i);
            if (faces[i].styles[0] &&
                font_covers(ft_has_char, faces[i].styles[0], cp, 0))
                cfg |= UINT64_C(1) << i;
        }
    }
    build_cfg_usable();
    bool coverable = (cfg & cfg_usable_bits) != 0 ||
                     ((fallback_ready() || fallback_init()) && fallback_covers(cp));
    return CP_KNOWN | (coverable ? CP_COVERABLE : 0) | (cfg & CP_CFG_MASK);
}

static uint64_t cp_info(uint32_t cp)
{
    if (cp >= UNICODE_LIMIT)
        return 0;
    _Atomic uint64_t *page = cp_page(cp);
    uint64_t v = page ? atomic_load_explicit(&page[cp & 255], memory_order_relaxed) : 0;
    if (!v) {
        v = cp_info_compute(cp);
        if (page)
            atomic_store_explicit(&page[cp & 255], v, memory_order_relaxed);
    }
    return v;
}

static uint64_t cfg_cover_mask(uint32_t cp)
{
    if (cp >= UNICODE_LIMIT)
        return 0;
    if (configured_faces > CP_CFG_FACES)
        return UINT64_MAX;
    return cp_info(cp) & CP_CFG_MASK;
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
        uint64_t info = cp_info(cps[k]);
        if (!(info & CP_COVERABLE)) {
            *dead_at = k;
            return 0;
        }
        if (configured_faces <= CP_CFG_FACES)
            cfg_and &= info & CP_CFG_MASK;
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
    return first_candidate_covering(cps, len) >= 0;
}
