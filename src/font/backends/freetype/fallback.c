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

struct fallback_leaf {
    uint64_t words[4];
};

struct fallback_page_leaf {
    uint16_t page;
    uint16_t leaf;
};

struct fallback_candidate {
    const struct fallback_page_leaf *leaves;
    const char *path;
    const unsigned char *mem;
    size_t mem_len;
    int index;
    uint16_t n_leaves;
    bool color : 1;
    bool is_emoji : 1;
};

static pthread_mutex_t fallback_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool fallback_ready_flag;

static struct fallback_candidate *fallback_candidates;
static struct fallback_leaf *fallback_leaves;
static struct fallback_page_leaf *fallback_page_leaves;
static char *fallback_paths;
static int fallback_total;

#define FALLBACK_PAGES (UNICODE_LIMIT >> 8)

static uint16_t *fallback_page_map;
static uint64_t *fallback_page_rows;
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
    return fallback_page_rows +
           (size_t)fallback_page_map[cp >> 8] * fallback_words;
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

static bool candidate_has_char(const struct fallback_candidate *c, uint32_t cp)
{
    if (!c->leaves || !c->n_leaves)
        return false;
    uint16_t page = (uint16_t)(cp >> 8);
    int lo = 0, hi = (int)c->n_leaves - 1;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        uint16_t p = c->leaves[mid].page;
        if (p < page) {
            lo = mid + 1;
        } else if (p > page) {
            hi = mid - 1;
        } else {
            const struct fallback_leaf *leaf =
                &fallback_leaves[c->leaves[mid].leaf];
            unsigned bit = cp & 255u;
            return (leaf->words[bit >> 6] >> (bit & 63u)) & 1u;
        }
    }
    return false;
}

static bool fb_has_char(void *font, uint32_t cp)
{
    return candidate_has_char((const struct fallback_candidate *)font, cp);
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
    if (!c->path || !candidate_has_char(c, cps[0]))
        return false;
    for (int k = 1; k < len; k++)
        if (needs_coverage(k, cps[k]) &&
            !font_covers(fb_has_char, (void *)c, cps[k], 0))
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
        if (c->path && font_covers(fb_has_char, (void *)c, cp, 0))
            return true;
    }
    return false;
}

bool fallback_ready(void)
{
    return atomic_load_explicit(&fallback_ready_flag, memory_order_acquire);
}

static void fallback_destroy_locked(void)
{
    atomic_store_explicit(&fallback_ready_flag, false, memory_order_release);
    free(fallback_candidates);
    fallback_candidates = NULL;
    free(fallback_leaves);
    fallback_leaves = NULL;
    free(fallback_page_leaves);
    fallback_page_leaves = NULL;
    free(fallback_paths);
    fallback_paths = NULL;
    free(fallback_page_map);
    fallback_page_map = NULL;
    free(fallback_page_rows);
    fallback_page_rows = NULL;
    free(run_set);
    run_set = NULL;
    fallback_words = 0;
    fallback_total = 0;
}

void fallback_destroy(void)
{
    pthread_mutex_lock(&fallback_mu);
    fallback_destroy_locked();
    pthread_mutex_unlock(&fallback_mu);
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

struct fb_slice {
    uint32_t off;
    uint16_t count;
};

struct fb_path_ref {
    uint32_t off;
    uint32_t len;
};

struct fb_builder {
    struct fallback_leaf *leaves;
    size_t n_leaves, cap_leaves;
    uint32_t *leaf_tab;
    size_t leaf_tab_cap;

    struct fallback_page_leaf *pl_buf;
    size_t n_pl, cap_pl;
    struct fb_slice *slices;
    size_t n_slices, cap_slices;
    uint32_t *slice_tab;
    size_t slice_tab_cap;

    char *paths;
    size_t n_paths, cap_paths;
    struct fb_path_ref *path_refs;
    size_t n_path_refs, cap_path_refs;
    uint32_t *path_tab;
    size_t path_tab_cap;

    struct fallback_page_leaf *tmp_pages;
    uint16_t *page_map;
    uint16_t nonempty_pages;
};

static uint64_t bytes_hash(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint64_t h = HASH64_BASIS;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, p + i, sizeof(v));
        h = hash64_step(h, v);
    }
    for (; i < len; i++)
        h = hash64_step(h, p[i]);
    return hash64_finish(h);
}

static bool grow_array(void **ptr, size_t *cap, size_t elem_size, size_t min_cap)
{
    size_t new_cap = *cap ? *cap * 2 : 64;
    if (new_cap < min_cap)
        new_cap = min_cap;
    void *next = realloc(*ptr, new_cap * elem_size);
    if (!next)
        return false;
    *ptr = next;
    *cap = new_cap;
    return true;
}

static uint64_t leaf_hash(const struct fallback_leaf *leaf)
{
    return bytes_hash(leaf->words, sizeof(leaf->words));
}

static bool leaf_rehash(struct fb_builder *b, size_t new_cap)
{
    uint32_t *tab = calloc(new_cap, sizeof(*tab));
    if (!tab)
        return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < b->n_leaves; i++) {
        size_t pos = (size_t)leaf_hash(&b->leaves[i]) & mask;
        while (tab[pos])
            pos = (pos + 1) & mask;
        tab[pos] = (uint32_t)i + 1;
    }
    free(b->leaf_tab);
    b->leaf_tab = tab;
    b->leaf_tab_cap = new_cap;
    return true;
}

static bool intern_leaf(struct fb_builder *b, const struct fallback_leaf *leaf,
                        uint16_t *out_id)
{
    if (!b->leaf_tab_cap && !leaf_rehash(b, 1024))
        return false;
    size_t mask = b->leaf_tab_cap - 1;
    uint64_t h = leaf_hash(leaf);
    size_t pos = (size_t)h & mask;
    while (b->leaf_tab[pos]) {
        uint32_t idx = b->leaf_tab[pos] - 1;
        if (!memcmp(&b->leaves[idx], leaf, sizeof(*leaf))) {
            *out_id = (uint16_t)idx;
            return true;
        }
        pos = (pos + 1) & mask;
    }
    if (b->n_leaves >= UINT16_MAX)
        return false;
    if (b->n_leaves == b->cap_leaves &&
        !grow_array((void **)&b->leaves, &b->cap_leaves, sizeof(*b->leaves), 512))
        return false;
    if ((b->n_leaves + 1) * 2 > b->leaf_tab_cap) {
        if (!leaf_rehash(b, b->leaf_tab_cap * 2))
            return false;
        mask = b->leaf_tab_cap - 1;
        pos = (size_t)h & mask;
        while (b->leaf_tab[pos])
            pos = (pos + 1) & mask;
    }
    uint16_t id = (uint16_t)b->n_leaves++;
    b->leaves[id] = *leaf;
    b->leaf_tab[pos] = (uint32_t)id + 1;
    *out_id = id;
    return true;
}

static bool slice_rehash(struct fb_builder *b, size_t new_cap)
{
    uint32_t *tab = calloc(new_cap, sizeof(*tab));
    if (!tab)
        return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < b->n_slices; i++) {
        const struct fb_slice *s = &b->slices[i];
        size_t pos = (size_t)bytes_hash(b->pl_buf + s->off,
                                        (size_t)s->count * sizeof(*b->pl_buf)) & mask;
        while (tab[pos])
            pos = (pos + 1) & mask;
        tab[pos] = (uint32_t)i + 1;
    }
    free(b->slice_tab);
    b->slice_tab = tab;
    b->slice_tab_cap = new_cap;
    return true;
}

static bool intern_slice(struct fb_builder *b,
                         const struct fallback_page_leaf *pages,
                         uint16_t count, uint32_t *out_off)
{
    if (!count) {
        *out_off = 0;
        return true;
    }
    if (!b->slice_tab_cap && !slice_rehash(b, 512))
        return false;
    size_t bytes = (size_t)count * sizeof(*pages);
    uint64_t h = bytes_hash(pages, bytes);
    size_t mask = b->slice_tab_cap - 1;
    size_t pos = (size_t)h & mask;
    while (b->slice_tab[pos]) {
        const struct fb_slice *s = &b->slices[b->slice_tab[pos] - 1];
        if (s->count == count && !memcmp(b->pl_buf + s->off, pages, bytes)) {
            *out_off = s->off;
            return true;
        }
        pos = (pos + 1) & mask;
    }
    while (b->n_pl + count > b->cap_pl)
        if (!grow_array((void **)&b->pl_buf, &b->cap_pl, sizeof(*b->pl_buf),
                        b->n_pl + count))
            return false;
    if (b->n_slices == b->cap_slices &&
        !grow_array((void **)&b->slices, &b->cap_slices, sizeof(*b->slices), 256))
        return false;
    if ((b->n_slices + 1) * 2 > b->slice_tab_cap) {
        if (!slice_rehash(b, b->slice_tab_cap * 2))
            return false;
        mask = b->slice_tab_cap - 1;
        pos = (size_t)h & mask;
        while (b->slice_tab[pos])
            pos = (pos + 1) & mask;
    }
    uint32_t off = (uint32_t)b->n_pl;
    memcpy(b->pl_buf + off, pages, bytes);
    b->n_pl += count;
    uint32_t sid = (uint32_t)b->n_slices++;
    b->slices[sid] = (struct fb_slice){ .off = off, .count = count };
    b->slice_tab[pos] = sid + 1;
    *out_off = off;
    return true;
}

static bool path_rehash(struct fb_builder *b, size_t new_cap)
{
    uint32_t *tab = calloc(new_cap, sizeof(*tab));
    if (!tab)
        return false;
    size_t mask = new_cap - 1;
    for (size_t i = 0; i < b->n_path_refs; i++) {
        const struct fb_path_ref *r = &b->path_refs[i];
        size_t pos = (size_t)bytes_hash(b->paths + r->off, r->len) & mask;
        while (tab[pos])
            pos = (pos + 1) & mask;
        tab[pos] = (uint32_t)i + 1;
    }
    free(b->path_tab);
    b->path_tab = tab;
    b->path_tab_cap = new_cap;
    return true;
}

static bool intern_path(struct fb_builder *b, const char *path, uint32_t *out_off)
{
    if (!b->path_tab_cap && !path_rehash(b, 512))
        return false;
    size_t len = strlen(path);
    uint64_t h = bytes_hash(path, len);
    size_t mask = b->path_tab_cap - 1;
    size_t pos = (size_t)h & mask;
    while (b->path_tab[pos]) {
        const struct fb_path_ref *r = &b->path_refs[b->path_tab[pos] - 1];
        if (r->len == len && !memcmp(b->paths + r->off, path, len)) {
            *out_off = r->off;
            return true;
        }
        pos = (pos + 1) & mask;
    }
    while (b->n_paths + len + 1 > b->cap_paths)
        if (!grow_array((void **)&b->paths, &b->cap_paths, sizeof(*b->paths),
                        b->n_paths + len + 1))
            return false;
    if (b->n_path_refs == b->cap_path_refs &&
        !grow_array((void **)&b->path_refs, &b->cap_path_refs,
                    sizeof(*b->path_refs), 256))
        return false;
    if ((b->n_path_refs + 1) * 2 > b->path_tab_cap) {
        if (!path_rehash(b, b->path_tab_cap * 2))
            return false;
        mask = b->path_tab_cap - 1;
        pos = (size_t)h & mask;
        while (b->path_tab[pos])
            pos = (pos + 1) & mask;
    }
    uint32_t off = (uint32_t)b->n_paths;
    memcpy(b->paths + off, path, len + 1);
    b->n_paths += len + 1;
    uint32_t rid = (uint32_t)b->n_path_refs++;
    b->path_refs[rid] = (struct fb_path_ref){ .off = off, .len = (uint32_t)len };
    b->path_tab[pos] = rid + 1;
    *out_off = off;
    return true;
}

static int cmp_page_leaf(const void *a, const void *b)
{
    const struct fallback_page_leaf *pa = a;
    const struct fallback_page_leaf *pb = b;
    return (pa->page > pb->page) - (pa->page < pb->page);
}

static void record_page_nonempty(struct fb_builder *b, uint16_t page)
{
    if (!b->page_map[page])
        b->page_map[page] = ++b->nonempty_pages;
}

static bool collect_fc_charset(struct fb_builder *b, const FcCharSet *cs,
                               uint32_t *out_off, uint16_t *out_count)
{
    uint16_t count = 0;
    bool unsorted = false;
    FcChar32 map[FC_CHARSET_MAP_SIZE], next;
    for (FcChar32 base = FcCharSetFirstPage(cs, map, &next);
         base != FC_CHARSET_DONE;
         base = FcCharSetNextPage(cs, map, &next)) {
        unsigned page = base >> 8;
        if (page >= FALLBACK_PAGES)
            continue;
        struct fallback_leaf leaf;
        for (int w = 0; w < 4; w++)
            leaf.words[w] = (uint64_t)map[2 * w] | ((uint64_t)map[2 * w + 1] << 32);
        uint16_t leaf_id;
        if (!intern_leaf(b, &leaf, &leaf_id))
            return false;
        record_page_nonempty(b, (uint16_t)page);
        if (count > 0 && (uint16_t)page <= b->tmp_pages[count - 1].page)
            unsorted = true;
        b->tmp_pages[count++] = (struct fallback_page_leaf){
            .page = (uint16_t)page, .leaf = leaf_id,
        };
    }
    if (unsorted)
        qsort(b->tmp_pages, count, sizeof(*b->tmp_pages), cmp_page_leaf);
    *out_count = count;
    return intern_slice(b, b->tmp_pages, count, out_off);
}

static bool collect_embedded_charset(struct fb_builder *b, uint32_t *out_off,
                                     uint16_t *out_count)
{
    *out_off = 0;
    *out_count = 0;
    FT_Face probe;
    if (FT_New_Memory_Face(ft_lib, embedded_cards_font,
                           (FT_Long)sizeof(embedded_cards_font), 0, &probe))
        return false;
    uint16_t count = 0;
    unsigned cur_page = FALLBACK_PAGES;
    struct fallback_leaf cur_leaf = {0};
    bool ok = true;
    FT_UInt gid;
    for (FT_ULong cp = FT_Get_First_Char(probe, &gid); gid;
         cp = FT_Get_Next_Char(probe, cp, &gid)) {
        unsigned page = (unsigned)(cp >> 8);
        if (page >= FALLBACK_PAGES)
            continue;
        if (page != cur_page) {
            if (cur_page < FALLBACK_PAGES) {
                uint16_t leaf_id;
                if (!intern_leaf(b, &cur_leaf, &leaf_id)) {
                    ok = false;
                    break;
                }
                record_page_nonempty(b, (uint16_t)cur_page);
                b->tmp_pages[count++] = (struct fallback_page_leaf){
                    .page = (uint16_t)cur_page, .leaf = leaf_id,
                };
            }
            cur_page = page;
            cur_leaf = (struct fallback_leaf){0};
        }
        unsigned bit = (unsigned)(cp & 255u);
        cur_leaf.words[bit >> 6] |= UINT64_C(1) << (bit & 63u);
    }
    FT_Done_Face(probe);
    if (ok && cur_page < FALLBACK_PAGES) {
        uint16_t leaf_id;
        if (intern_leaf(b, &cur_leaf, &leaf_id)) {
            record_page_nonempty(b, (uint16_t)cur_page);
            b->tmp_pages[count++] = (struct fallback_page_leaf){
                .page = (uint16_t)cur_page, .leaf = leaf_id,
            };
        } else {
            ok = false;
        }
    }
    if (!ok)
        return false;
    *out_count = count;
    return intern_slice(b, b->tmp_pages, count, out_off);
}

static void builder_free_scratch(struct fb_builder *b)
{
    free(b->leaf_tab);
    free(b->slices);
    free(b->slice_tab);
    free(b->path_refs);
    free(b->path_tab);
    free(b->tmp_pages);
}

static void builder_free_all(struct fb_builder *b)
{
    builder_free_scratch(b);
    free(b->leaves);
    free(b->pl_buf);
    free(b->paths);
    free(b->page_map);
}

static void shrink_allocation(void **ptr, size_t used_bytes)
{
    if (!used_bytes) {
        free(*ptr);
        *ptr = NULL;
        return;
    }
    void *shrunk = realloc(*ptr, used_bytes);
    if (shrunk)
        *ptr = shrunk;
}

static bool fallback_init_locked(void)
{
    if (!fc_pin())
        return false;
    fc_query_lock();

    FcPattern *pat = FcPatternCreate();
    if (!pat) {
        fc_query_unlock();
        fc_unpin();
        return false;
    }
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"monospace");
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcFontSet *fonts = FcFontSort(NULL, pat, FcFalse, NULL, &result);
    FcPatternDestroy(pat);

    if (!fonts || result != FcResultMatch) {
        if (fonts)
            FcFontSetDestroy(fonts);
        fc_query_unlock();
        fc_unpin();
        return false;
    }

    int nfont = fonts->nfont;
    struct fb_builder b = {0};
    b.page_map = calloc(FALLBACK_PAGES, sizeof(*b.page_map));
    b.tmp_pages = malloc(FALLBACK_PAGES * sizeof(*b.tmp_pages));
    uint32_t embedded_off = 0;
    uint16_t embedded_cnt = 0;
    int embedded = (b.page_map && b.tmp_pages &&
                    collect_embedded_charset(&b, &embedded_off, &embedded_cnt)) ? 1 : 0;
    int total = nfont + embedded;
    int words = (total + 63) / 64;

    struct fallback_candidate *cands = calloc((size_t)total + 1, sizeof(*cands));
    uint32_t *slice_offs = calloc((size_t)total + 1, sizeof(*slice_offs));
    uint32_t *path_offs = malloc(((size_t)total + 1) * sizeof(*path_offs));
    if (!b.page_map || !b.tmp_pages || !cands || !slice_offs || !path_offs)
        goto fail;
    for (int i = 0; i <= total; i++)
        path_offs[i] = UINT32_MAX;

    if (embedded) {
        cands[0].path = embedded_cards_path;
        cands[0].mem = embedded_cards_font;
        cands[0].mem_len = sizeof(embedded_cards_font);
        cands[0].n_leaves = embedded_cnt;
        slice_offs[0] = embedded_off;
    }

    for (int i = embedded; i < total; i++) {
        FcPattern *match = fonts->fonts[i - embedded];
        FcCharSet *cs = NULL;
        FcChar8 *path = NULL;
        FcBool color = FcFalse;
        if (FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) != FcResultMatch ||
            FcPatternGetString(match, FC_FILE, 0, &path) != FcResultMatch)
            continue;
        if (strstr((const char *)path, "LastResort"))
            continue;
        if (!intern_path(&b, (const char *)path, &path_offs[i]) ||
            !collect_fc_charset(&b, cs, &slice_offs[i], &cands[i].n_leaves))
            goto fail;
        FcPatternGetInteger(match, FC_INDEX, 0, &cands[i].index);
        FcPatternGetBool(match, FC_COLOR, 0, &color);
        cands[i].color = color;
        cands[i].is_emoji = pattern_is_emoji(match);
    }

    FcFontSetDestroy(fonts);
    fonts = NULL;

    uint64_t *page_rows = calloc((size_t)(1 + b.nonempty_pages) * words,
                                 sizeof(*page_rows));
    uint64_t *rset = run_set ? run_set : calloc((size_t)words + 1, sizeof(*rset));
    if (!page_rows || !rset) {
        free(page_rows);
        if (rset != run_set)
            free(rset);
        goto fail;
    }

    shrink_allocation((void **)&b.leaves, b.n_leaves * sizeof(*b.leaves));
    shrink_allocation((void **)&b.pl_buf, b.n_pl * sizeof(*b.pl_buf));
    shrink_allocation((void **)&b.paths, b.n_paths * sizeof(*b.paths));

    for (int i = 0; i < total; i++) {
        if (path_offs[i] != UINT32_MAX)
            cands[i].path = b.paths + path_offs[i];
        if (cands[i].n_leaves) {
            cands[i].leaves = b.pl_buf + slice_offs[i];
            for (uint16_t k = 0; k < cands[i].n_leaves; k++) {
                uint16_t r = b.page_map[cands[i].leaves[k].page];
                page_rows[(size_t)r * words + (i >> 6)] |= UINT64_C(1) << (i & 63);
            }
        }
    }

    builder_free_scratch(&b);
    free(slice_offs);
    free(path_offs);

    fallback_candidates = cands;
    fallback_leaves = b.leaves;
    fallback_page_leaves = b.pl_buf;
    fallback_paths = b.paths;
    fallback_page_map = b.page_map;
    fallback_page_rows = page_rows;
    fallback_total = total;
    fallback_words = words;
    run_set = rset;
    atomic_store_explicit(&fallback_ready_flag, true, memory_order_release);

    fc_query_unlock();
    fc_unpin();
    return true;

fail:
    if (fonts)
        FcFontSetDestroy(fonts);
    fc_query_unlock();
    builder_free_all(&b);
    free(cands);
    free(slice_offs);
    free(path_offs);
    fc_unpin();
    fallback_destroy_locked();
    return false;
}

bool fallback_init(void)
{
    pthread_mutex_lock(&fallback_mu);
    bool ok = fallback_ready() ? true : fallback_init_locked();
    pthread_mutex_unlock(&fallback_mu);
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
        !candidate_has_char(c, cps[0]))
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
