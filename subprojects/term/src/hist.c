#include "hist.h"
#include "compat.h"
#include "kernels.h"
#include "unicode/grapheme.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RUN_FG   (1u << 0)
#define RUN_BG   (1u << 1)
#define RUN_ATTR (1u << 2)
#define RUN_LINK (1u << 3)
#define RUN_MAX  18

#define PREFIX_WIDE    0xF5
#define PREFIX_EMOJI   0xF6
#define PREFIX_TEXT    0xF7
#define PREFIX_CLUSTER 0xF8

#define STYLE_FLAGS (CELL_BOLD | CELL_ITALIC | CELL_ANY_UNDERLINE | CELL_STRIKE | \
                     CELL_OVERLINE | CELL_FAINT | CELL_LINK)
#define SHAPE_FLAGS (CELL_WIDE | CELL_TRAIL | CELL_WRAP_PAD | CELL_EMOJI | CELL_TEXT)

#define NO_TEXT UINT32_MAX
#define ROWS_UNKNOWN 255
#define CELL_BYTES 5
#define CLUSTER_BYTES (2 + WSAP_VTCORE_GRAPHEME_MAX_LEN * 4)

static inline struct hline *part(const struct hist *h, uint64_t seq)
{
    return &h->ring[seq & h->ring_mask];
}

static inline uint8_t *part_text(const struct hist *h, const struct hline *p)
{
    return h->pages[p->off >> 16] + (p->off & 0xffff);
}

static inline struct hline *open_part(const struct hist *h)
{
    return hist_is_open(h) ? part(h, h->head - 1) : NULL;
}

static WSAP_VTCORE_NOINLINE bool ring_grow(struct hist *h, uint64_t n)
{
    uint64_t cap = h->ring ? (uint64_t)h->ring_mask + 1 : 65536;
    if (cap < 65536)
        cap = 65536;
    cap = wsap_vtcore_grow_cap64(cap, h->head - h->tail + n);
    struct hline *ring = wsap_vtcore_aligned_alloc(64, (size_t)cap * sizeof(*ring));
    if (!ring)
        return false;
    struct hist_block *blocks = calloc((size_t)cap / 16, sizeof(*blocks));
    if (!blocks) {
        wsap_vtcore_aligned_free(ring);
        return false;
    }
    uint32_t *exact = h->exact_rows ? calloc((size_t)cap, sizeof(*exact)) : NULL;
    if (h->exact_rows && !exact) {
        free(blocks);
        wsap_vtcore_aligned_free(ring);
        return false;
    }
    for (uint64_t s = h->tail; s < h->head; s++) {
        ring[s & (cap - 1)] = *part(h, s);
        if (exact)
            exact[s & (cap - 1)] = h->exact_rows[s & h->ring_mask];
    }
    free(h->exact_rows);
    h->exact_rows = exact;
    wsap_vtcore_aligned_free(h->ring);
    free(h->blocks);
    h->blocks = blocks;
    h->ring = ring;
    h->ring_mask = (uint32_t)(cap - 1);
    return true;
}

static inline bool ring_fit(struct hist *h, uint64_t n)
{
    if (WSAP_VTCORE_EXPECT(h->ring && h->head - h->tail + n <= (uint64_t)h->ring_mask + 1, 1))
        return true;
    return ring_grow(h, n);
}

static inline uint64_t cont_at(const struct hist *h, uint32_t i)
{
    return h->cont[(h->cont_first + i) & h->cont_mask];
}

static bool cont_push(struct hist *h, uint64_t seq)
{
    if (!h->cont || h->cont_count > h->cont_mask) {
        uint32_t cap = h->cont ? (h->cont_mask + 1) * 2 : 16;
        uint64_t *cont = malloc((size_t)cap * sizeof(*cont));
        if (!cont)
            return false;
        if (h->cont) {
            uint32_t count = h->cont_count;
            uint32_t first = h->cont_first;
            uint32_t old_cap = h->cont_mask + 1;
            uint32_t chunk = old_cap - first;
            if (chunk > count)
                chunk = count;
            memcpy(cont, h->cont + first, (size_t)chunk * sizeof(*cont));
            if (count > chunk)
                memcpy(cont + chunk, h->cont, (size_t)(count - chunk) * sizeof(*cont));
            free(h->cont);
        }
        h->cont = cont;
        h->cont_mask = cap - 1;
        h->cont_first = 0;
    }
    h->cont[(h->cont_first + h->cont_count++) & h->cont_mask] = seq;
    return true;
}

static void cont_trim(struct hist *h)
{
    while (h->cont_count && cont_at(h, 0) <= h->tail) {
        h->cont_first = (h->cont_first + 1) & h->cont_mask;
        h->cont_count--;
    }
    while (h->cont_count && cont_at(h, h->cont_count - 1) >= h->head)
        h->cont_count--;
}

static uint64_t line_first(const struct hist *h, uint64_t id)
{
    uint64_t want = h->tail + (id - h->base_id);
    if (id == h->base_id || !h->cont_count)
        return want;
    uint32_t lo = 0, hi = h->cont_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cont_at(h, mid) - mid <= want)
            lo = mid + 1;
        else
            hi = mid;
    }
    return want + lo;
}

static inline uint64_t line_end(const struct hist *h, uint64_t first)
{
    if (!h->cont_count)
        return first + 1;
    uint64_t s = first + 1;
    while (s < h->head && (part(h, s)->flags & HLINE_CONT))
        s++;
    return s;
}

static inline bool line_valid(const struct hist *h, uint64_t id)
{
    return id >= h->base_id && id < h->base_id + h->lines;
}

static WSAP_VTCORE_ALWAYS_INLINE inline void page_drop_ref(struct hist *h, uint32_t pg)
{
    if (--h->page_parts[pg] == 0) {
        if (pg == h->cur_page)
            h->page_used[pg] = 0;
        else
            h->free_pages[h->nfree++] = (uint16_t)pg;
    }
}

static void rows_uncount(struct hist *h, uint64_t id)
{
    if (id < h->rows_id)
        h->counted_rows -= wsap_vtcore_hist_line_estimate(h, id, h->wrap_cols);
}

static void line_gone(struct hist *h)
{
    h->base_id++;
    h->lines--;
    if (h->rows_id < h->base_id)
        h->rows_id = h->base_id;
}

static bool evict_part(struct hist *h)
{
    if (h->tail == h->head)
        return false;
    struct hline *p = part(h, h->tail);
    if (p->flags & HLINE_OPEN)
        return false;
    bool cut = h->tail + 1 < h->head && (part(h, h->tail + 1)->flags & HLINE_CONT);
    if (cut) {
        h->index_epoch++;
        if (h->base_cell_id != h->base_id) {
            h->base_cell_id = h->base_id;
            h->base_cell = 0;
        }
        h->base_cell += p->cells;
        if (h->lines == 1) {
            h->last_cells -= p->cells;
            h->last_wide -= p->wide;
        }
        h->rows_id = h->base_id;
        h->counted_rows = 0;
        h->walk_on = false;
    } else {
        rows_uncount(h, h->base_id);
    }
    if (p->off != NO_TEXT)
        page_drop_ref(h, p->off >> 16);
    h->tail++;
    if (cut)
        cont_trim(h);
    else
        line_gone(h);
    return true;
}

static WSAP_VTCORE_NOINLINE void evict_line_parts(struct hist *h)
{
    uint64_t end = line_end(h, h->tail);
    if (part(h, end - 1)->flags & HLINE_OPEN)
        return;
    rows_uncount(h, h->base_id);
    for (uint64_t s = h->tail; s < end; s++)
        if (part(h, s)->off != NO_TEXT)
            page_drop_ref(h, part(h, s)->off >> 16);
    h->tail = end;
    cont_trim(h);
    line_gone(h);
}

static void evict_line(struct hist *h);

static inline void page_drop_n(struct hist *h, uint32_t pg, uint32_t n)
{
    if ((h->page_parts[pg] -= n) == 0) {
        if (pg == h->cur_page)
            h->page_used[pg] = 0;
        else
            h->free_pages[h->nfree++] = (uint16_t)pg;
    }
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline bool same_page_narrow_rows_avx512(const struct hline *p, uint32_t page)
{
    const __m512i pg = _mm512_set1_epi32((int)(page << 16));
    const __m512i mask = _mm512_set1_epi32((int)0xffff0000u);
    const __m512i tag = _mm512_set1_epi32(0x01010000);
    for (int i = 0; i < 16; i += 4) {
        __m512i v = _mm512_loadu_si512((const void *)(p + i));
        if ((_mm512_cmpeq_epi32_mask(v, tag) & 0x8888) != 0x8888 ||
            (_mm512_cmpeq_epi32_mask(_mm512_and_si512(v, mask), pg) & 0x1111) != 0x1111)
            return false;
    }
    return true;
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline bool same_page_narrow_rows_avx2(const struct hline *p, uint32_t page)
{
    const __m256i pg = _mm256_set1_epi32((int)(page << 16));
    const __m256i mask = _mm256_set1_epi32((int)0xffff0000u);
    const __m256i tag = _mm256_set1_epi32(0x01010000);
    for (int i = 0; i < 16; i += 2) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(p + i));
        if ((_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(v, tag))) & 0x88) != 0x88 ||
            (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(v, mask), pg))) & 0x11) != 0x11)
            return false;
    }
    return true;
}
#endif

static inline bool same_page_narrow_rows(const struct hline *p, uint32_t page)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return same_page_narrow_rows_avx512(p, page);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return same_page_narrow_rows_avx2(p, page);
#endif
#if defined(__aarch64__)
    const uint32x4_t expected = {page << 16, 0, 0, 0x01010000};
    const uint32x4_t mask = {0xffff0000u, 0, 0, 0xffffffffu};
    for (int i = 0; i < 16; i++) {
        uint32x4_t v = vreinterpretq_u32_u8(vld1q_u8((const uint8_t *)(p + i)));
        if (vminvq_u32(vceqq_u32(vandq_u32(v, mask), expected)) != UINT32_MAX)
            return false;
    }
    return true;
#else
    for (int i = 0; i < 16; i++) {
        if (p[i].off == NO_TEXT || (p[i].off >> 16) != page ||
            p[i].flags != HLINE_NARROW || p[i].attr_len || p[i].wrap_rows != 1)
            return false;
    }
    return true;
#endif
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline bool same_page_rows_64(const struct hline *p, uint32_t page,
                                    uint32_t *text_count, uint32_t *row_count)
{
    const __m512i pg = _mm512_set1_epi32((int)(page << 16));
    const __m512i mask = _mm512_set1_epi32((int)0xffff0000u);
    const __m512i open = _mm512_set1_epi32(0x00300000);
    const __m512i unknown = _mm512_set1_epi32((int)0xff000000u);
    const __m512i no_text = _mm512_set1_epi32(-1);
    __m512i sums = _mm512_setzero_si512();
    __mmask16 tag_fail = 0, pg_fail = 0;
    uint32_t blanks = 0;
    for (int i = 0; i < 64; i += 4) {
        __m512i v = _mm512_loadu_si512((const void *)(p + i));
        __mmask16 empty = _mm512_cmpeq_epi32_mask(v, no_text) & 0x1111;
        blanks += wsap_vtcore_popcount32(empty);
        tag_fail |= (_mm512_test_epi32_mask(v, open) |
                     _mm512_cmpeq_epi32_mask(_mm512_and_si512(v, unknown), unknown)) & 0x8888;
        pg_fail |= (uint16_t)(~(_mm512_cmpeq_epi32_mask(_mm512_and_si512(v, mask), pg) | empty) & 0x1111);
        sums = _mm512_mask_add_epi32(sums, 0x8888, sums, _mm512_srli_epi32(v, 24));
    }
    *text_count = 64 - blanks;
    *row_count = (uint32_t)_mm512_reduce_add_epi32(sums);
    return (tag_fail | pg_fail) == 0;
}
#endif

static uint64_t evict_simple_run(struct hist *h, uint64_t k)
{
    if (k == 0 || h->tail == h->head || !h->ring)
        return 0;
    const struct hline *ring = h->ring;
    uint32_t rmask = h->ring_mask;
    uint64_t tail = h->tail, head = h->head, base = h->base_id;
    uint64_t counted = h->rows_id > base ? h->rows_id - base : 0;
    uint64_t rows_take = 0;
    uint64_t n = 0;
    uint32_t page = 0, run = 0;
    bool have_page = false;

    while (n < k && tail < head) {
        if ((tail & 15) == 0 && n + 16 <= k && tail + 16 <= head &&
            (n >= counted || n + 16 <= counted)) {
            const struct hist_block *b = &h->blocks[(tail & rmask) / 16];
            if (b->seq == tail + 1 && b->epoch == h->index_epoch && b->cols == h->wrap_cols) {
                if (b->refs) {
                    if (have_page && page != b->page) {
                        page_drop_n(h, page, run);
                        run = 0;
                    }
                    page = b->page;
                    have_page = true;
                    run += b->refs;
                }
                if (n < counted) rows_take += b->rows;
                tail += 16;
                n += 16;
                continue;
            }
        }
#if defined(WSAP_VTCORE_KERNEL_AVX512)
        if (wsap_vtcore_use_avx512() && (tail & 63) == 0 && n + 64 <= k && tail + 64 <= head &&
            ((tail & rmask) + 64 <= rmask + 1)) {
            const struct hline *f = &ring[tail & rmask];

            uint32_t pg = f->off >> 16;
            uint32_t text_count, row_count;
            if (f->off != NO_TEXT && (n >= counted || n + 64 <= counted) &&
                same_page_rows_64(f, pg, &text_count, &row_count) &&
                (tail + 64 == head || !(ring[(tail + 64) & rmask].flags & HLINE_CONT))) {
                if (have_page && page != pg) {
                    page_drop_n(h, page, run);
                    run = 0;
                }
                page = pg;
                have_page = true;
                run += text_count;
                if (n < counted) rows_take += row_count;
                tail += 64;
                n += 64;
                continue;
            }
        }
#endif
        if ((tail & 15) == 0 && n + 16 <= k && tail + 16 <= head && ((tail & rmask) + 16 <= rmask + 1)) {
            const struct hline *f = &ring[tail & rmask];

            uint32_t pg = f->off >> 16;
            if (f->off != NO_TEXT && same_page_narrow_rows(f, pg) &&
                (tail + 16 == head || !(ring[(tail + 16) & rmask].flags & HLINE_CONT))) {
                if (have_page && page != pg) {
                    page_drop_n(h, page, run);
                    run = 0;
                }
                page = pg;
                have_page = true;
                run += 16;
                uint64_t remaining = counted > n ? counted - n : 0;
                rows_take += remaining < 16 ? remaining : 16;
                tail += 16;
                n += 16;
                continue;
            }

            uint32_t t0 = *(const uint32_t *)((const char *)&f[0] + 12);
            uint32_t t1 = *(const uint32_t *)((const char *)&f[1] + 12);
            uint32_t t2 = *(const uint32_t *)((const char *)&f[2] + 12);
            uint32_t t3 = *(const uint32_t *)((const char *)&f[3] + 12);
            uint32_t t4 = *(const uint32_t *)((const char *)&f[4] + 12);
            uint32_t t5 = *(const uint32_t *)((const char *)&f[5] + 12);
            uint32_t t6 = *(const uint32_t *)((const char *)&f[6] + 12);
            uint32_t t7 = *(const uint32_t *)((const char *)&f[7] + 12);
            uint32_t t8 = *(const uint32_t *)((const char *)&f[8] + 12);
            uint32_t t9 = *(const uint32_t *)((const char *)&f[9] + 12);
            uint32_t t10 = *(const uint32_t *)((const char *)&f[10] + 12);
            uint32_t t11 = *(const uint32_t *)((const char *)&f[11] + 12);
            uint32_t t12 = *(const uint32_t *)((const char *)&f[12] + 12);
            uint32_t t13 = *(const uint32_t *)((const char *)&f[13] + 12);
            uint32_t t14 = *(const uint32_t *)((const char *)&f[14] + 12);
            uint32_t t15 = *(const uint32_t *)((const char *)&f[15] + 12);

            uint32_t or_all = t0 | t1 | t2 | t3 | t4 | t5 | t6 | t7 |
                              t8 | t9 | t10 | t11 | t12 | t13 | t14 | t15;
            if (WSAP_VTCORE_EXPECT((or_all & 0x0030ffff) == 0 &&
                (t0 >> 24) != 255 && (t1 >> 24) != 255 && (t2 >> 24) != 255 && (t3 >> 24) != 255 &&
                (t4 >> 24) != 255 && (t5 >> 24) != 255 && (t6 >> 24) != 255 && (t7 >> 24) != 255 &&
                (t8 >> 24) != 255 && (t9 >> 24) != 255 && (t10 >> 24) != 255 && (t11 >> 24) != 255 &&
                (t12 >> 24) != 255 && (t13 >> 24) != 255 && (t14 >> 24) != 255 && (t15 >> 24) != 255, 1)) {

                uint32_t target = page << 16;
                uint32_t off_diff = (f[0].off ^ target) | (f[1].off ^ target) | (f[2].off ^ target) | (f[3].off ^ target) |
                                    (f[4].off ^ target) | (f[5].off ^ target) | (f[6].off ^ target) | (f[7].off ^ target) |
                                    (f[8].off ^ target) | (f[9].off ^ target) | (f[10].off ^ target) | (f[11].off ^ target) |
                                    (f[12].off ^ target) | (f[13].off ^ target) | (f[14].off ^ target) | (f[15].off ^ target);
                if (have_page && (off_diff & 0xffff0000u) == 0) {
                    run += 16;
                } else {
                    for (int j = 0; j < 16; j++) {
                        uint32_t o = f[j].off;
                        if (o != NO_TEXT) {
                            uint32_t pg = o >> 16;
                            if (have_page && pg == page) {
                                run++;
                            } else {
                                if (have_page)
                                    page_drop_n(h, page, run);
                                page = pg;
                                run = 1;
                                have_page = true;
                            }
                        }
                    }
                }
                if (n + 16 <= counted) {
                    rows_take += (t0 >> 24) + (t1 >> 24) + (t2 >> 24) + (t3 >> 24) +
                                 (t4 >> 24) + (t5 >> 24) + (t6 >> 24) + (t7 >> 24) +
                                 (t8 >> 24) + (t9 >> 24) + (t10 >> 24) + (t11 >> 24) +
                                 (t12 >> 24) + (t13 >> 24) + (t14 >> 24) + (t15 >> 24);
                }
                tail += 16;
                n += 16;
                continue;
            }
        }

        const struct hline *f = &ring[tail & rmask];
        if (f->flags & HLINE_OPEN)
            break;
        if (tail + 1 < head && (ring[(tail + 1) & rmask].flags & HLINE_CONT))
            break;
        if (f->wrap_rows == ROWS_UNKNOWN)
            break;
        if (n < counted)
            rows_take += f->wrap_rows;
        if (f->off != NO_TEXT) {
            uint32_t pg = f->off >> 16;
            if (have_page && pg == page) {
                run++;
            } else {
                if (have_page)
                    page_drop_n(h, page, run);
                page = pg;
                run = 1;
                have_page = true;
            }
        }
        tail++;
        n++;
    }
    if (n == 0)
        return 0;
    if (have_page)
        page_drop_n(h, page, run);
    h->tail = tail;
    h->base_id += n;
    h->lines -= (uint32_t)n;
    if (counted) {
        h->counted_rows -= rows_take;
        if (n >= counted)
            h->rows_id = h->base_id;
    }
    return n;
}

static inline void evict_for(struct hist *h, uint64_t n)
{
    uint64_t have = h->lines;
    if (have + n <= (uint64_t)h->line_limit)
        return;
    uint64_t need = have + n - h->line_limit;
    while (need > 0) {
        uint64_t done = evict_simple_run(h, need);
        if (done >= need)
            break;
        need -= done;
        uint64_t old_lines = h->lines;
        evict_line(h);
        if (h->lines == old_lines)
            break;
        need--;
    }
}

static inline WSAP_VTCORE_ALWAYS_INLINE void evict_line(struct hist *h)
{
    if (h->tail == h->head)
        return;
    const struct hline *f = part(h, h->tail);
    if (WSAP_VTCORE_EXPECT(!(f->flags & HLINE_OPEN) &&
                         (h->tail + 1 == h->head || !(part(h, h->tail + 1)->flags & HLINE_CONT)), 1)) {
        if (h->base_id < h->rows_id)
            h->counted_rows -= f->wrap_rows != ROWS_UNKNOWN
                               ? f->wrap_rows : wsap_vtcore_hist_line_estimate(h, h->base_id, h->wrap_cols);
        if (f->off != NO_TEXT)
            page_drop_ref(h, f->off >> 16);
        h->tail++;
        line_gone(h);
        return;
    }
    evict_line_parts(h);
}

static WSAP_VTCORE_NOINLINE int page_next(struct hist *h)
{
    for (;;) {
        if (h->nfree) {
            uint32_t pg = h->free_pages[--h->nfree];
            h->page_used[pg] = 0;
            h->cur_page = pg;
            return (int)pg;
        }
        if (h->npages < h->max_pages) {
            uint8_t *page = wsap_vtcore_aligned_alloc(64, HIST_PAGE_SIZE);
            if (!page)
                return -1;
            uint32_t pg = h->npages++;
            h->pages[pg] = page;
            h->page_used[pg] = 0;
            h->page_parts[pg] = 0;
            h->cur_page = pg;
            return (int)pg;
        }
        if (!evict_part(h))
            return -1;
    }
}

static inline int page_for(struct hist *h, size_t need)
{
    if (need > HIST_PAGE_USABLE)
        return -1;
    uint32_t pg = h->cur_page;
    if (WSAP_VTCORE_EXPECT(pg < h->npages && h->page_used[pg] + need <= HIST_PAGE_USABLE, 1))
        return (int)pg;
    return page_next(h);
}

static inline int put_varint(uint8_t *p, uint32_t v)
{
    if (WSAP_VTCORE_EXPECT(v < 0x80, 1)) {
        p[0] = (uint8_t)v;
        return 1;
    }
    p[0] = (uint8_t)(v | 0x80);
    if (WSAP_VTCORE_EXPECT(v < 0x4000, 1)) {
        p[1] = (uint8_t)(v >> 7);
        return 2;
    }
    p[1] = (uint8_t)((v >> 7) | 0x80);
    if (v < 0x200000) {
        p[2] = (uint8_t)(v >> 14);
        return 3;
    }
    p[2] = (uint8_t)((v >> 14) | 0x80);
    if (v < 0x10000000) {
        p[3] = (uint8_t)(v >> 21);
        return 4;
    }
    p[3] = (uint8_t)((v >> 21) | 0x80);
    p[4] = (uint8_t)(v >> 28);
    return 5;
}

static inline int get_varint(const uint8_t *p, uint32_t *v)
{
    uint8_t b0 = p[0];
    if (WSAP_VTCORE_EXPECT((b0 & 0x80) == 0, 1)) {
        *v = b0;
        return 1;
    }
    uint8_t b1 = p[1];
    if (WSAP_VTCORE_EXPECT((b1 & 0x80) == 0, 1)) {
        *v = (uint32_t)(b0 & 0x7f) | ((uint32_t)b1 << 7);
        return 2;
    }
    uint8_t b2 = p[2];
    if ((b2 & 0x80) == 0) {
        *v = (uint32_t)(b0 & 0x7f) | ((uint32_t)(b1 & 0x7f) << 7) | ((uint32_t)b2 << 14);
        return 3;
    }
    uint8_t b3 = p[3];
    if ((b3 & 0x80) == 0) {
        *v = (uint32_t)(b0 & 0x7f) | ((uint32_t)(b1 & 0x7f) << 7) |
             ((uint32_t)(b2 & 0x7f) << 14) | ((uint32_t)b3 << 21);
        return 4;
    }
    *v = (uint32_t)(b0 & 0x7f) | ((uint32_t)(b1 & 0x7f) << 7) |
         ((uint32_t)(b2 & 0x7f) << 14) | ((uint32_t)(b3 & 0x7f) << 21) |
         ((uint32_t)(p[4] & 0x7f) << 28);
    return 5;
}

static inline void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void put_u16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t get_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static inline uint8_t *emit_run(uint8_t *p, const struct pen_attr *base, const struct pen_attr *pen,
                                uint32_t count)
{
    if (WSAP_VTCORE_EXPECT(count < 0x80 && pen_sl(pen) == pen_sl(base), 1)) {
        uint32_t dfg = pen->fg != base->fg, dbg = pen->bg != base->bg;
        p[0] = (uint8_t)(dfg | (dbg << 1));
        p[1] = (uint8_t)count;
        p += 2;
        put_u32(p, pen->fg);
        p += dfg * 4;
        put_u32(p, pen->bg);
        return p + dbg * 4;
    }
    uint8_t header = (pen->fg != base->fg ? RUN_FG : 0) |
                     (pen->bg != base->bg ? RUN_BG : 0) |
                     (pen->flags != base->flags ? RUN_ATTR : 0) |
                     (pen->link != base->link ? RUN_LINK : 0);
    *p++ = header;
    p += put_varint(p, count);
    if (header & RUN_FG) { put_u32(p, pen->fg); p += 4; }
    if (header & RUN_BG) { put_u32(p, pen->bg); p += 4; }
    if (header & RUN_ATTR) { put_u16(p, pen->flags); p += 2; }
    if (header & RUN_LINK) { put_u16(p, pen->link); p += 2; }
    return p;
}

static inline const uint8_t *read_run(const uint8_t *p, struct pen_attr *pen, uint32_t *count)
{
    uint8_t header = *p++;
    p += get_varint(p, count);
    if (header & RUN_FG) { pen->fg = get_u32(p); p += 4; }
    if (header & RUN_BG) { pen->bg = get_u32(p); p += 4; }
    if (header & RUN_ATTR) { pen->flags = get_u16(p); p += 2; }
    if (header & RUN_LINK) { pen->link = get_u16(p); p += 2; }
    return p;
}

static bool runs_reserve(struct hist_runs *r, size_t extra)
{
    if (WSAP_VTCORE_EXPECT(r->len + extra <= r->cap, 1))
        return true;
    size_t cap = wsap_vtcore_grow_cap(r->cap ? r->cap : 256, (size_t)r->len + extra);
    uint8_t *data = realloc(r->data, cap);
    if (!data)
        return false;
    r->data = data;
    r->cap = (uint32_t)cap;
    return true;
}

static inline void runs_reset(struct hist_runs *r)
{
    r->len = 0;
    r->base = pen_attr_default();
    r->pen = pen_attr_default();
    r->count = 0;
}

void wsap_vtcore_hist_runs_emit_slow(struct hist_runs *r)
{
    r->len = (uint32_t)(emit_run(r->data + r->len, &r->base, &r->pen, r->count) - r->data);
}

static void runs_finish(struct hist_runs *r)
{
    if (r->count && !pen_attr_is_default(&r->pen))
        hist_runs_switch(r, text_pen_from_attr(&r->pen));
    r->count = 0;
}

static inline size_t put_utf8(uint8_t *dst, uint32_t cp)
{
    if (cp < 0x80) {
        dst[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        put_u16(dst, (uint16_t)(0x80C0u | (cp >> 6) | ((cp & 0x3Fu) << 8)));
        return 2;
    }
    if (cp < 0x10000) {
        put_u32(dst, 0x008080E0u | (cp >> 12) | ((cp & 0x0FC0u) << 2) | ((cp & 0x3Fu) << 16));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        put_u32(dst, 0x808080F0u | (cp >> 18) | ((cp & 0x3F000u) >> 4) |
                     ((cp & 0x0FC0u) << 10) | ((cp & 0x3Fu) << 24));
        return 4;
    }
    put_u32(dst, 0x00BDBFEFu);
    return 3;
}

static inline int get_utf8(const uint8_t *s, const uint8_t *end, uint32_t *cp)
{
    uint8_t b = s[0];
    int n = b < 0xE0 ? (b < 0x80 ? 1 : 2) : b < 0xF0 ? 3 : 4;
    if (s + n > end) {
        *cp = 0xFFFD;
        return (int)(end - s);
    }
    switch (n) {
    case 1: *cp = b; break;
    case 2: *cp = ((uint32_t)(b & 0x1f) << 6) | (s[1] & 0x3f); break;
    case 3: *cp = ((uint32_t)(b & 0x0f) << 12) | ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f); break;
    default:
        *cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(s[1] & 0x3f) << 12) |
              ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        break;
    }
    return n;
}

struct enc {
    uint32_t bytes;
    uint32_t cells;
    uint8_t flags;
    uint16_t wide;
};

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline int narrow_prefix_avx512(const struct cell *cells, int len, uint8_t *dst,
                                       __m512i keep, __m512i target, __m512i span)
{
    const __m512i idx = _mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 0, 0, 0, 0, 0, 0, 0);
    int k = 0;
    for (; k < len; k += 8) {
        int left = len - k;
        __mmask16 m0 = left >= 4 ? 0xFFFF : (__mmask16)((1u << (left * 4)) - 1);
        __mmask16 m1 = left >= 8 ? 0xFFFF : left <= 4 ? 0 : (__mmask16)((1u << ((left - 4) * 4)) - 1);
        __m512i v0 = _mm512_maskz_loadu_epi32(m0, (const void *)(cells + k));
        __m512i v1 = _mm512_maskz_loadu_epi32(m1, (const void *)(cells + k + 4));
        __mmask16 ok0 = _mm512_mask_cmple_epu32_mask(m0, _mm512_sub_epi32(_mm512_and_si512(v0, keep), target), span);
        __mmask16 ok1 = _mm512_mask_cmple_epu32_mask(m1, _mm512_sub_epi32(_mm512_and_si512(v1, keep), target), span);
        __m512i cp = _mm512_permutex2var_epi32(v0, idx, v1);
        if (ok0 != m0 || ok1 != m1) {
            uint32_t bad = (uint32_t)(~ok0 & m0) | ((uint32_t)(~ok1 & m1) << 16);
            int good = wsap_vtcore_ctz32(bad) / 4;
            _mm_mask_storeu_epi8(dst + k, (__mmask16)((1u << good) - 1), _mm512_cvtepi32_epi8(cp));
            return k + good;
        }
        if (left >= 8)
            _mm_storel_epi64((__m128i *)(void *)(dst + k), _mm512_cvtepi32_epi8(cp));
        else
            _mm_mask_storeu_epi8(dst + k, (__mmask16)((1u << left) - 1), _mm512_cvtepi32_epi8(cp));
    }
    return len;
}

WSAP_VTCORE_TARGET_AVX512
static inline int narrow_any_pen_prefix_avx512(const struct cell *cells, int len, uint8_t *dst)
{
    const __m512i keep = _mm512_setr_epi32(-1, 0, 0, SHAPE_FLAGS, -1, 0, 0, SHAPE_FLAGS,
                                           -1, 0, 0, SHAPE_FLAGS, -1, 0, 0, SHAPE_FLAGS);
    const __m512i target = _mm512_setr_epi32(0x20, 0, 0, 0, 0x20, 0, 0, 0, 0x20, 0, 0, 0, 0x20, 0, 0, 0);
    const __m512i span = _mm512_setr_epi32(0x5f, 0, 0, 0, 0x5f, 0, 0, 0, 0x5f, 0, 0, 0, 0x5f, 0, 0, 0);
    return narrow_prefix_avx512(cells, len, dst, keep, target, span);
}

WSAP_VTCORE_TARGET_AVX512
static inline int narrow_uniform_prefix_avx512(const struct cell *cells, int len, uint8_t *dst)
{
    uint32_t sl = (uint32_t)(cells[0].flags & STYLE_FLAGS) | ((uint32_t)cells[0].pad << 16);
    const __m512i target = _mm512_setr_epi32(
        0x20, (int)cells[0].fg, (int)cells[0].bg, (int)sl, 0x20, (int)cells[0].fg, (int)cells[0].bg, (int)sl,
        0x20, (int)cells[0].fg, (int)cells[0].bg, (int)sl, 0x20, (int)cells[0].fg, (int)cells[0].bg, (int)sl);
    const __m512i keep = _mm512_setr_epi32(
        -1, -1, -1, (int)(STYLE_FLAGS | SHAPE_FLAGS | 0xFFFF0000u), -1, -1, -1, (int)(STYLE_FLAGS | SHAPE_FLAGS | 0xFFFF0000u),
        -1, -1, -1, (int)(STYLE_FLAGS | SHAPE_FLAGS | 0xFFFF0000u), -1, -1, -1, (int)(STYLE_FLAGS | SHAPE_FLAGS | 0xFFFF0000u));
    const __m512i span = _mm512_setr_epi32(0x5f, 0, 0, 0, 0x5f, 0, 0, 0, 0x5f, 0, 0, 0, 0x5f, 0, 0, 0);
    return narrow_prefix_avx512(cells, len, dst, keep, target, span);
}
#endif

static inline int narrow_any_pen_prefix(const struct cell *cells, int len, uint8_t *dst)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return narrow_any_pen_prefix_avx512(cells, len, dst);
#endif
    int k = 0;
    for (; k < len; k++) {
        if (cells[k].c - 0x20u >= 0x60u || (cells[k].flags & SHAPE_FLAGS))
            break;
        dst[k] = (uint8_t)cells[k].c;
    }
    return k;
}

static WSAP_VTCORE_NOINLINE uint32_t runs_scan(struct hist_runs *r, const struct cell *cells, int n)
{
    uint32_t links = 0;
    uint32_t pfg = r->pen.fg, pbg = r->pen.bg;
    uint32_t psl = (uint32_t)r->pen.flags | ((uint32_t)r->pen.link << 16);
    uint32_t bfg = r->base.fg, bbg = r->base.bg;
    uint32_t bsl = (uint32_t)r->base.flags | ((uint32_t)r->base.link << 16);
    uint32_t count = r->count;
    uint8_t *rp = r->data + r->len;
    for (int i = 0; i < n; i++) {
        const struct cell *c = &cells[i];
        uint32_t sl = (uint32_t)(c->flags & STYLE_FLAGS) | ((uint32_t)c->pad << 16);
        links |= c->pad;
        if (WSAP_VTCORE_EXPECT(((c->fg ^ pfg) | (c->bg ^ pbg) | (sl ^ psl)) != 0, 0)) {
            if (count) {
                uint32_t header = (pfg != bfg ? RUN_FG : 0) | (pbg != bbg ? RUN_BG : 0) |
                                  ((uint16_t)psl != (uint16_t)bsl ? RUN_ATTR : 0) |
                                  ((psl >> 16) != (bsl >> 16) ? RUN_LINK : 0);
                rp[0] = (uint8_t)header;
                if (WSAP_VTCORE_EXPECT(count < 0x80, 1)) {
                    rp[1] = (uint8_t)count;
                    rp += 2;
                } else {
                    rp += 1 + put_varint(rp + 1, count);
                }
                put_u32(rp, pfg);
                rp += (header & RUN_FG) ? 4 : 0;
                put_u32(rp, pbg);
                rp += (header & RUN_BG) ? 4 : 0;
                if (WSAP_VTCORE_EXPECT(header & (RUN_ATTR | RUN_LINK), 0)) {
                    if (header & RUN_ATTR) { put_u16(rp, (uint16_t)psl); rp += 2; }
                    if (header & RUN_LINK) { put_u16(rp, (uint16_t)(psl >> 16)); rp += 2; }
                }
                bfg = pfg;
                bbg = pbg;
                bsl = psl;
            }
            pfg = c->fg;
            pbg = c->bg;
            psl = sl;
            count = 0;
        }
        count++;
    }
    r->len = (uint32_t)(rp - r->data);
    pen_set(&r->base, bfg, bbg, bsl);
    pen_set(&r->pen, pfg, pbg, psl);
    r->count = count;
    return links;
}

static inline int narrow_uniform_prefix(const struct cell *cells, int len, uint8_t *dst)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return narrow_uniform_prefix_avx512(cells, len, dst);
#endif
    uint32_t sl = (uint32_t)(cells[0].flags & STYLE_FLAGS) | ((uint32_t)cells[0].pad << 16);
    int k = 0;
    for (; k < len; k++) {
        const struct cell *c = &cells[k];
        uint32_t csl = (uint32_t)(c->flags & (STYLE_FLAGS | SHAPE_FLAGS)) | ((uint32_t)c->pad << 16);
        if (c->c - 0x20u >= 0x60u || c->fg != cells[0].fg || c->bg != cells[0].bg || csl != sl)
            break;
        dst[k] = (uint8_t)c->c;
    }
    return k;
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static int uniform_utf8_prefix(const struct cell *cells, int len, uint8_t *dst, struct enc *o,
                               uint32_t *runcells)
{
    uint32_t sl = (uint32_t)(cells[0].flags & STYLE_FLAGS) | ((uint32_t)cells[0].pad << 16);
    const __m512i target = _mm512_setr_epi32(
        0, (int)cells[0].fg, (int)cells[0].bg, (int)sl, 0, (int)cells[0].fg, (int)cells[0].bg, (int)sl,
        0, (int)cells[0].fg, (int)cells[0].bg, (int)sl, 0, (int)cells[0].fg, (int)cells[0].bg, (int)sl);
    const __m512i keep = _mm512_setr_epi32(
        (int)0xFFFF0000u, -1, -1, (int)(STYLE_FLAGS | CELL_WRAP_PAD | CELL_EMOJI | CELL_TEXT | 0xFFFF0000u),
        (int)0xFFFF0000u, -1, -1, (int)(STYLE_FLAGS | CELL_WRAP_PAD | CELL_EMOJI | CELL_TEXT | 0xFFFF0000u),
        (int)0xFFFF0000u, -1, -1, (int)(STYLE_FLAGS | CELL_WRAP_PAD | CELL_EMOJI | CELL_TEXT | 0xFFFF0000u),
        (int)0xFFFF0000u, -1, -1, (int)(STYLE_FLAGS | CELL_WRAP_PAD | CELL_EMOJI | CELL_TEXT | 0xFFFF0000u));
    const __m512i idx_cp = _mm512_setr_epi32(0, 4, 8, 12, 16, 20, 24, 28, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m512i idx_fl = _mm512_setr_epi32(3, 7, 11, 15, 19, 23, 27, 31, 0, 0, 0, 0, 0, 0, 0, 0);
    int i = 0;
    uint32_t bytes = 0, ncells = 0, wide = 0, ascii = 0xFF, nwide = 0;
    for (; i + 8 <= len; i += 8) {
        __m512i v0 = _mm512_loadu_si512((const void *)(cells + i));
        __m512i v1 = _mm512_loadu_si512((const void *)(cells + i + 4));
        __m512i d = _mm512_or_si512(_mm512_xor_si512(_mm512_and_si512(v0, keep), target),
                                    _mm512_xor_si512(_mm512_and_si512(v1, keep), target));
        if (_mm512_test_epi32_mask(d, d))
            break;
        __m256i cp = _mm512_castsi512_si256(_mm512_permutex2var_epi32(v0, idx_cp, v1));
        __m256i fl = _mm512_castsi512_si256(_mm512_permutex2var_epi32(v0, idx_fl, v1));
        __m256i ch = _mm256_max_epu32(cp, _mm256_set1_epi32(0x20));
        __mmask8 ge80 = _mm256_cmpge_epu32_mask(ch, _mm256_set1_epi32(0x80));
        __mmask8 ge800 = _mm256_cmpge_epu32_mask(ch, _mm256_set1_epi32(0x800));
        __mmask8 m_wide = _mm256_test_epi32_mask(fl, _mm256_set1_epi32(CELL_WIDE));
        __mmask8 m_trail = _mm256_test_epi32_mask(fl, _mm256_set1_epi32(CELL_TRAIL));
        __mmask8 surrogate = _mm256_cmplt_epu32_mask(_mm256_sub_epi32(ch, _mm256_set1_epi32(0xD800)),
                                                     _mm256_set1_epi32(0x800));
        if ((m_wide & ~ge800) || (surrogate & ~m_trail))
            break;
        __m256i b1 = _mm256_and_si256(_mm256_srli_epi32(ch, 4), _mm256_set1_epi32(0x00000F00));
        __m256i b2 = _mm256_and_si256(_mm256_slli_epi32(ch, 10), _mm256_set1_epi32(0x003F0000));
        __m256i b3 = _mm256_and_si256(_mm256_slli_epi32(ch, 24), _mm256_set1_epi32(0x3F000000));
        __m256i w = _mm256_or_si256(_mm256_or_si256(b1, b2), _mm256_or_si256(b3, _mm256_set1_epi32((int)0x8080E0F5u)));
        w = _mm256_mask_or_epi32(w, (__mmask8)~ge800, w, _mm256_set1_epi32(0x00400000));
        w = _mm256_mask_mov_epi32(w, (__mmask8)~ge80, _mm256_slli_epi32(ch, 24));
        __m256i bm = _mm256_set1_epi32((int)0x80000000u);
        bm = _mm256_mask_mov_epi32(bm, ge80, _mm256_set1_epi32((int)0x80800000u));
        bm = _mm256_mask_mov_epi32(bm, ge800, _mm256_set1_epi32((int)0x80808000u));
        bm = _mm256_mask_mov_epi32(bm, m_wide, _mm256_set1_epi32((int)0x80808080u));
        bm = _mm256_maskz_mov_epi32((__mmask8)~m_trail, bm);
        __mmask32 keep_bytes = _mm256_movepi8_mask(bm);
        _mm256_storeu_si256((__m256i *)(void *)(dst + bytes), _mm256_maskz_compress_epi8(keep_bytes, w));
        bytes += (uint32_t)wsap_vtcore_popcount32(keep_bytes);
        uint32_t trails = (uint32_t)wsap_vtcore_popcount32(m_trail);
        ncells += 8u - trails + (uint32_t)wsap_vtcore_popcount32(m_wide);
        *runcells += 8u - trails;
        nwide += (uint32_t)wsap_vtcore_popcount32(m_wide);
        wide |= m_wide;
        ascii &= (uint32_t)(uint8_t)~(ge80 & ~m_trail);
    }
    o->bytes = bytes;
    o->cells = ncells;
    o->wide = (uint16_t)nwide;
    o->flags = (wide ? HLINE_HAS_WIDE : 0) | (ascii == 0xFF ? HLINE_NARROW : 0) |
               (cells[0].pad ? HLINE_HAS_LINK : 0);
    return i;
}
#endif

static WSAP_VTCORE_NOINLINE size_t encode_cluster(uint8_t *dst, const struct gstore *g,
                                                       uint32_t handle, uint16_t f)
{
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = wsap_vtcore_gstore_expand(g, handle, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    size_t b = 0;
    dst[b++] = PREFIX_CLUSTER;
    dst[b++] = (uint8_t)((n & 0x1f) | ((f & CELL_WIDE) ? 0x80 : 0) |
                         ((f & CELL_EMOJI) ? 0x40 : 0) | ((f & CELL_TEXT) ? 0x20 : 0));
    for (int k = 0; k < n; k++)
        b += put_utf8(dst + b, cps[k]);
    return b;
}

static size_t encode_shaped(uint8_t *dst, uint32_t cp, uint16_t f, const struct gstore *g,
                            uint8_t *flags)
{
    if (wsap_vtcore_grapheme_is_handle(cp)) {
        *flags |= HLINE_HAS_CLUSTER | ((f & CELL_WIDE) ? HLINE_HAS_WIDE : 0);
        return encode_cluster(dst, g, cp, f);
    }
    if (cp < 0x20)
        cp = ' ';
    uint16_t shape = f & (CELL_WIDE | CELL_EMOJI | CELL_TEXT);
    size_t b = 0;
    if (shape == 0)
        return put_utf8(dst, cp);
    if (shape == CELL_WIDE) {
        *flags |= HLINE_HAS_WIDE;
        dst[b++] = PREFIX_WIDE;
    } else if (shape == (CELL_WIDE | CELL_EMOJI)) {
        *flags |= HLINE_HAS_WIDE;
        dst[b++] = PREFIX_EMOJI;
    } else if (shape == CELL_TEXT) {
        dst[b++] = PREFIX_TEXT;
    } else {
        *flags |= HLINE_HAS_CLUSTER | ((f & CELL_WIDE) ? HLINE_HAS_WIDE : 0);
        dst[b++] = PREFIX_CLUSTER;
        dst[b++] = (uint8_t)(1 | ((f & CELL_WIDE) ? 0x80 : 0) |
                             ((f & CELL_EMOJI) ? 0x40 : 0) | ((f & CELL_TEXT) ? 0x20 : 0));
    }
    return b + put_utf8(dst + b, cp);
}

static int encode_cells(struct hist_runs *r, const struct cell *cells, int n, uint8_t *dst,
                        size_t cap, const struct gstore *g, struct enc *o)
{
    o->bytes = 0;
    o->cells = 0;
    o->flags = HLINE_NARROW;
    o->wide = 0;
    int i = 0;
    if (n <= 0)
        return 0;
    const struct cell *c0 = &cells[0];
    struct pen_attr pen0 = { c0->fg, c0->bg, (uint16_t)(c0->flags & STYLE_FLAGS), c0->pad };
    bool ascii0 = !(c0->flags & SHAPE_FLAGS) && c0->c - 0x20u < 0x60u;
    if (ascii0)
        i = narrow_uniform_prefix(cells, n, dst);
    if (i == n) {
        hist_runs_add(r, &pen0, (uint32_t)n);
        *o = (struct enc){ .bytes = (uint32_t)n, .cells = (uint32_t)n,
            .flags = (uint8_t)(HLINE_NARROW | (c0->pad ? HLINE_HAS_LINK : 0)) };
        return n;
    }
    const struct cell *ci = &cells[i];
    if (i > 0 && !(ci->flags & SHAPE_FLAGS) && ci->c - 0x20u < 0x60u) {
        i += narrow_any_pen_prefix(ci, n - i, dst + i);
        uint32_t links = runs_scan(r, cells, i);
        *o = (struct enc){ .bytes = (uint32_t)i, .cells = (uint32_t)i,
            .flags = (uint8_t)(HLINE_NARROW | (links ? HLINE_HAS_LINK : 0)) };
        if (i == n)
            return n;
    } else if (i > 0) {
        hist_runs_add(r, &pen0, (uint32_t)i);
        *o = (struct enc){ .bytes = (uint32_t)i, .cells = (uint32_t)i,
            .flags = (uint8_t)(HLINE_NARROW | (c0->pad ? HLINE_HAS_LINK : 0)) };
    }
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512() && n - i >= 8 && ((cells[i].flags & SHAPE_FLAGS) || cells[i].c >= 0x80)) {
        struct enc u;
        uint32_t runcells = 0;
        const struct cell *cu = &cells[i];
        int k = uniform_utf8_prefix(cu, n - i, dst + o->bytes, &u, &runcells);
        if (k > 0) {
            hist_runs_add_pen(r, (struct text_pen){ cu->fg, cu->bg,
                              (uint32_t)(cu->flags & STYLE_FLAGS) | ((uint32_t)cu->pad << 16) }, runcells);
            o->bytes += u.bytes;
            o->cells += u.cells;
            o->wide += u.wide;
            o->flags = (uint8_t)((o->flags & u.flags & HLINE_NARROW) |
                                 ((o->flags | u.flags) & ~HLINE_NARROW));
            i += k;
            if (i == n)
                return n;
        }
    }
#endif

    uint32_t pfg = r->pen.fg, pbg = r->pen.bg, psl = pen_sl(&r->pen);
    uint32_t bfg = r->base.fg, bbg = r->base.bg, bsl = pen_sl(&r->base);
    uint32_t count = r->count;
    uint8_t *rp = r->data + r->len;
    uint32_t bytes = o->bytes, ncells = o->cells, nwide = o->wide;
    uint8_t flags = o->flags;
    uint32_t links = 0;
    for (; i < n; i++) {
        const struct cell *c = &cells[i];
        uint16_t f = c->flags;
        if (f & (CELL_TRAIL | CELL_WRAP_PAD))
            continue;
        uint32_t cp = c->c;
        bool simple = !(f & (CELL_WIDE | CELL_EMOJI | CELL_TEXT)) && cp - 0x20u < 0x60u;
        if (!simple && wsap_vtcore_grapheme_is_handle(cp) &&
            bytes + CLUSTER_BYTES + (size_t)(n - i) * CELL_BYTES > cap)
            break;
        uint32_t sl = (uint32_t)(f & STYLE_FLAGS) | ((uint32_t)c->pad << 16);
        links |= c->pad;
        if (WSAP_VTCORE_EXPECT(((c->fg ^ pfg) | (c->bg ^ pbg) | (sl ^ psl)) != 0, 0)) {
            if (count) {
                uint32_t header = (pfg != bfg ? RUN_FG : 0) | (pbg != bbg ? RUN_BG : 0) |
                                  ((uint16_t)psl != (uint16_t)bsl ? RUN_ATTR : 0) |
                                  ((psl >> 16) != (bsl >> 16) ? RUN_LINK : 0);
                *rp++ = (uint8_t)header;
                if (WSAP_VTCORE_EXPECT(count < 0x80, 1))
                    *rp++ = (uint8_t)count;
                else
                    rp += put_varint(rp, count);
                put_u32(rp, pfg);
                rp += (header & RUN_FG) ? 4 : 0;
                put_u32(rp, pbg);
                rp += (header & RUN_BG) ? 4 : 0;
                if (WSAP_VTCORE_EXPECT(header & (RUN_ATTR | RUN_LINK), 0)) {
                    if (header & RUN_ATTR) { put_u16(rp, (uint16_t)psl); rp += 2; }
                    if (header & RUN_LINK) { put_u16(rp, (uint16_t)(psl >> 16)); rp += 2; }
                }
                bfg = pfg;
                bbg = pbg;
                bsl = psl;
            }
            pfg = c->fg;
            pbg = c->bg;
            psl = sl;
            count = 0;
        }
        count++;
        nwide += (f & CELL_WIDE) != 0;
        if (WSAP_VTCORE_EXPECT(simple, 1)) {
            dst[bytes++] = (uint8_t)cp;
            ncells++;
            continue;
        }
        flags &= (uint8_t)~HLINE_NARROW;
        uint16_t shape = f & (CELL_WIDE | CELL_EMOJI | CELL_TEXT);
        if ((shape == 0 || shape == CELL_WIDE) && cp - 0x80u < 0xD800u - 0x80u) {
            if (shape) {
                dst[bytes++] = PREFIX_WIDE;
                flags |= HLINE_HAS_WIDE;
                ncells++;
            }
            bytes += (uint32_t)put_utf8(dst + bytes, cp);
            ncells++;
            continue;
        }
        bytes += (uint32_t)encode_shaped(dst + bytes, cp, f, g, &flags);
        ncells += (f & CELL_WIDE) ? 2 : 1;
    }
    r->len = (uint32_t)(rp - r->data);
    pen_set(&r->base, bfg, bbg, bsl);
    pen_set(&r->pen, pfg, pbg, psl);
    r->count = count;
    o->bytes = bytes;
    o->cells = ncells;
    o->wide = (uint16_t)nwide;
    o->flags = flags | (links ? HLINE_HAS_LINK : 0);
    return i;
}

static inline uint8_t row_count_byte(uint64_t rows)
{
    return rows < ROWS_UNKNOWN ? (uint8_t)rows : ROWS_UNKNOWN;
}

#define HLINE_NARROW_TAIL ((uint64_t)HLINE_NARROW << 48 | 1ULL << 56)

static const struct hline HLINE_BLANK = {
    .off = NO_TEXT, .flags = HLINE_NARROW, .wrap_rows = 1,
};

static inline void part_put_narrow_rows(struct hline *p, uint32_t off, uint32_t len, uint32_t rows)
{
    uint64_t v[2] = {
        (uint64_t)off | (uint64_t)len << 32,
        (uint64_t)len | ((uint64_t)HLINE_NARROW << 48) | ((uint64_t)row_count_byte(rows) << 56),
    };
    memcpy(p, v, sizeof(v));
}

static inline void part_put_narrow(struct hline *p, uint32_t off, uint32_t len)
{
    uint64_t v[2] = {
        (uint64_t)off | (uint64_t)len << 32,
        (uint64_t)len | HLINE_NARROW_TAIL,
    };
    memcpy(p, v, sizeof(v));
}

static void walk_advance(struct hist *h);

static inline uint64_t cheap_rows(uint64_t cells, uint64_t wide, int cols)
{
    return rows_narrow(cols == 1 ? cells - wide : cells, cols);
}

static inline void count_closed(struct hist *h, struct hline *first, uint64_t rows)
{
    if (rows && h->wrap_cols && h->rows_id == hist_end_id(h) - 1) {
        first->wrap_rows = row_count_byte(rows);
        h->counted_rows += rows;
        h->rows_id++;
    }
}

static bool push_line(struct hist *h, const struct cell *cells, int len, const char *ascii,
                      const struct pen_attr *pen, const struct gstore *g)
{
    if (h->lines >= h->line_limit)
        evict_line(h);
    size_t runs_need = ascii ? RUN_MAX * 2 : (size_t)(len + 2) * RUN_MAX;
    size_t text_need = ascii ? (size_t)len : (size_t)len * CELL_BYTES;
    int pg = page_for(h, text_need + runs_need + RUN_MAX);
    if (pg < 0 || !ring_fit(h, 1) || !runs_reserve(&h->runs, runs_need + RUN_MAX))
        return false;
    uint32_t off = h->page_used[pg];
    uint8_t *dst = h->pages[pg] + off;
    runs_reset(&h->runs);
    struct enc e;
    if (ascii) {
        bytes_copy_small(dst, ascii, len);
        hist_runs_add(&h->runs, pen, (uint32_t)len);
        e = (struct enc){ .bytes = (uint32_t)len, .cells = (uint32_t)len,
            .flags = (uint8_t)(HLINE_NARROW | (pen->link ? HLINE_HAS_LINK : 0)) };
    } else {
        size_t cap = HIST_PAGE_USABLE - off - (runs_need + RUN_MAX);
        if (encode_cells(&h->runs, cells, len, dst, cap, g, &e) < len)
            return false;
    }
    runs_finish(&h->runs);
    if (h->runs.len)
        memcpy(dst + e.bytes, h->runs.data, h->runs.len);
    struct hline *p = part(h, h->head++);
    *p = (struct hline){
        .off = ((uint32_t)pg << 16) | off,
        .text_len = e.bytes,
        .cells = (uint16_t)e.cells,
        .wide = e.wide,
        .attr_len = (uint16_t)h->runs.len,
        .flags = e.flags,
        .wrap_rows = ROWS_UNKNOWN,
    };
    h->page_used[pg] = off + e.bytes + h->runs.len;
    h->page_parts[pg]++;
    h->lines++;
    h->all_blank = false;
    h->runs.len = 0;
    if (h->wrap_cols)
        count_closed(h, p, cheap_rows(e.cells, e.wide, h->wrap_cols));
    return true;
}

static inline bool push_uniform_line(struct hist *h, const struct cell *cells, int len)
{
    if (h->lines >= h->line_limit)
        evict_line(h);
    uint32_t pg = h->cur_page;
    if (pg >= h->npages || h->page_used[pg] + (uint32_t)len + RUN_MAX > HIST_PAGE_USABLE ||
        !h->ring || h->head - h->tail > h->ring_mask)
        return false;
    uint32_t off = h->page_used[pg];
    uint8_t *dst = h->pages[pg] + off;
    if (narrow_uniform_prefix(cells, len, dst) != len)
        return false;
    struct pen_attr pen = { cells[0].fg, cells[0].bg, (uint16_t)(cells[0].flags & STYLE_FLAGS),
                            cells[0].pad };
    uint32_t attr = 0;
    if (!pen_attr_is_default(&pen)) {
        struct pen_attr base = pen_attr_default();
        attr = (uint32_t)(emit_run(dst + len, &base, &pen, (uint32_t)len) - (dst + len));
    }
    struct hline *q = part(h, h->head++);
    *q = (struct hline){
        .off = (pg << 16) | off, .text_len = (uint32_t)len, .cells = (uint32_t)len,
        .attr_len = (uint16_t)attr,
        .flags = (uint8_t)(HLINE_NARROW | (pen.link ? HLINE_HAS_LINK : 0)),
        .wrap_rows = ROWS_UNKNOWN,
    };
    h->page_used[pg] = off + (uint32_t)len + attr;
    h->page_parts[pg]++;
    h->lines++;
    h->all_blank = false;
    if (h->wrap_cols)
        count_closed(h, q, cheap_rows((uint64_t)len, 0, h->wrap_cols));
    return true;
}

static struct hline *begin_line(struct hist *h, size_t need)
{
    if (h->lines >= h->line_limit)
        evict_line(h);
    int pg = page_for(h, need);
    if (pg < 0 || !ring_fit(h, 1))
        return NULL;
    struct hline *p = part(h, h->head++);
    *p = (struct hline){
        .off = ((uint32_t)pg << 16) | h->page_used[pg],
        .flags = HLINE_NARROW | HLINE_OPEN,
        .wrap_rows = ROWS_UNKNOWN,
    };
    h->page_parts[pg]++;
    h->lines++;
    h->last_id = hist_end_id(h) - 1;
    h->last_cells = h->last_wide = 0;
    h->all_blank = false;
    h->walk_on = false;
    runs_reset(&h->runs);
    return p;
}

static void close_part(struct hist *h, struct hline *p)
{
    runs_finish(&h->runs);
    uint32_t pg = p->off >> 16;
    uint32_t end = (p->off & 0xffff) + p->text_len;
    if (h->runs.len) {
        memcpy(h->pages[pg] + end, h->runs.data, h->runs.len);
        p->attr_len = (uint16_t)h->runs.len;
        end += h->runs.len;
    }
    h->page_used[pg] = end;
    p->flags &= (uint8_t)~HLINE_OPEN;
}

void wsap_vtcore_hist_close_open_line(struct hist *h)
{
    struct hline *p = open_part(h);
    if (!p)
        return;
    close_part(h, p);
    runs_reset(&h->runs);
    uint64_t id = hist_end_id(h) - 1;
    int cols = h->wrap_cols;
    if (!cols) {
        h->walk_on = false;
        return;
    }
    uint64_t first = line_first(h, id);
    uint64_t rows = first == h->head - 1
                    ? cheap_rows(p->cells, p->wide, cols)
                    : cheap_rows(wsap_vtcore_hist_line_cells(h, id), wsap_vtcore_hist_line_wide(h, id), cols);
    if (!rows && h->walk_on) {
        walk_advance(h);
        rows = h->walk_rows;
    }
    h->walk_on = false;
    count_closed(h, part(h, first), rows);
}

static struct hline *open_room(struct hist *h, struct hline *p, size_t text_need,
                               size_t runs_need, uint32_t add_cells)
{
    uint32_t start = p->off & 0xffff;
    size_t runs = h->runs.len + runs_need + RUN_MAX;
    if (WSAP_VTCORE_EXPECT(start + p->text_len + text_need + runs <= HIST_PAGE_USABLE &&
                         p->cells + add_cells <= HIST_MAX_PART_CELLS &&
                         runs <= UINT16_MAX, 1))
        return p;

    if (p->text_len + text_need + runs <= HIST_PAGE_USABLE / 4 &&
        p->cells + add_cells <= HIST_MAX_PART_CELLS && start > 0) {
        uint32_t old_pg = p->off >> 16;
        int pg = h->page_parts[old_pg] == 1 ? -1 : page_next(h);
        if (pg >= 0) {
            memcpy(h->pages[pg], h->pages[old_pg] + start, p->text_len);
            if (h->page_used[old_pg] == start + p->text_len)
                h->page_used[old_pg] = start;
            page_drop_ref(h, old_pg);
            p->off = (uint32_t)pg << 16;
            h->page_parts[pg]++;
            h->page_used[pg] = p->text_len;
            return p;
        }
        if (h->page_parts[old_pg] == 1) {
            memmove(h->pages[old_pg], h->pages[old_pg] + start, p->text_len);
            p->off = old_pg << 16;
            if (h->cur_page != old_pg && h->cur_page < h->npages && !h->page_parts[h->cur_page])
                h->free_pages[h->nfree++] = (uint16_t)h->cur_page;
            h->cur_page = old_pg;
            h->page_used[old_pg] = p->text_len;
            return p;
        }
    }

    struct pen_attr pen = h->runs.pen;
    runs_finish(&h->runs);
    uint32_t pg0 = p->off >> 16;
    uint32_t end = (p->off & 0xffff) + p->text_len;
    if (h->runs.len) {
        memcpy(h->pages[pg0] + end, h->runs.data, h->runs.len);
        p->attr_len = (uint16_t)h->runs.len;
        end += h->runs.len;
    }
    h->page_used[pg0] = end;
    int pg = page_next(h);
    if (pg < 0) {
        if (h->tail + 1 != h->head || h->page_parts[pg0] != 1)
            return NULL;
        h->page_used[pg0] = 0;
        if (h->base_cell_id != h->base_id) {
            h->base_cell_id = h->base_id;
            h->base_cell = 0;
        }
        h->base_cell += p->cells;
        h->last_cells = h->last_wide = 0;
        h->index_epoch++;
        h->cur_page = pg0;
        *p = (struct hline){
            .off = pg0 << 16,
            .flags = HLINE_OPEN | HLINE_NARROW | (p->flags & HLINE_CONT),
            .wrap_rows = ROWS_UNKNOWN,
        };
        h->walk_on = false;
        runs_reset(&h->runs);
        h->runs.pen = pen;
        return p;
    }
    if (!ring_fit(h, 1))
        return NULL;
    p = part(h, h->head - 1);
    p->flags &= (uint8_t)~HLINE_OPEN;
    struct hline *q = part(h, h->head++);
    *q = (struct hline){
        .off = ((uint32_t)pg << 16) | h->page_used[pg],
        .flags = HLINE_OPEN | HLINE_CONT | HLINE_NARROW,
        .wrap_rows = ROWS_UNKNOWN,
    };
    h->page_parts[pg]++;
    if (!cont_push(h, h->head - 1)) {
        h->page_parts[pg]--;
        h->head--;
        p->flags |= HLINE_OPEN;
        return NULL;
    }
    runs_reset(&h->runs);
    h->runs.pen = pen;
    return q;
}

static inline void part_add(struct hist *h, struct hline *p, const struct enc *e)
{
    p->text_len += e->bytes;
    p->cells += (uint16_t)e->cells;
    p->wide += e->wide;
    h->last_cells += e->cells;
    h->last_wide += e->wide;
    p->flags = (uint8_t)((p->flags & ~HLINE_NARROW) | (e->flags & ~HLINE_NARROW) |
                         (p->flags & e->flags & HLINE_NARROW));
    h->page_used[p->off >> 16] = (p->off & 0xffff) + p->text_len;
}

void wsap_vtcore_hist_push_row(struct hist *h, const struct cell *cells, int len, bool wrapped,
                   const struct gstore *g)
{
    if (!h->line_limit)
        return;
    struct hline *p = open_part(h);
    if (!p) {
        if (len <= 0 && !wrapped) {
            wsap_vtcore_hist_push_blank(h, 1);
            return;
        }
        if (!wrapped && push_uniform_line(h, cells, len))
            return;
        if (!wrapped && push_line(h, cells, len, NULL, NULL, g))
            return;
        p = begin_line(h, (size_t)len * CELL_BYTES + (size_t)(len + 2) * RUN_MAX);
        if (!p)
            return;
    }
    for (int i = 0; i < len;) {
        int n = len - i;
        size_t runs_need = (size_t)(n + 1) * RUN_MAX;
        p = open_room(h, p, (size_t)n * CELL_BYTES, runs_need, (uint32_t)n * 2);
        if (!p || !runs_reserve(&h->runs, runs_need + RUN_MAX))
            return;
        size_t used_bytes = (p->off & 0xffff) + p->text_len;
        size_t cap = HIST_PAGE_USABLE - used_bytes - (h->runs.len + runs_need + RUN_MAX);
        struct enc e;
        i += encode_cells(&h->runs, cells + i, n, part_text(h, p) + p->text_len, cap, g, &e);
        part_add(h, p, &e);
        if (i < len && !(p = open_room(h, p, HIST_PAGE_USABLE, 0, 0)))
            return;
    }
    if (!wrapped)
        wsap_vtcore_hist_close_open_line(h);
    else if (h->wrap_cols && (h->walk_on || (p->flags & (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER))))
        walk_advance(h);
}

void wsap_vtcore_hist_push_ascii(struct hist *h, const char *bytes, int n, const struct pen_attr *pen,
                     bool wrapped)
{
    if (!h->line_limit)
        return;
    struct pen_attr def = pen_attr_default();
    if (!pen)
        pen = &def;
    struct hline *p = open_part(h);
    if (!p) {
        if (n <= 0 && !wrapped) {
            wsap_vtcore_hist_push_blank(h, 1);
            return;
        }
        if (!wrapped && h->lines >= h->line_limit)
            evict_line(h);
        uint32_t pg = h->cur_page;
        if (!wrapped && pen_attr_is_default(pen) && pg < h->npages &&
            h->page_used[pg] + (uint32_t)n <= HIST_PAGE_USABLE &&
            h->ring && h->head - h->tail <= h->ring_mask) {
            uint32_t off = h->page_used[pg];
            bytes_copy_small(h->pages[pg] + off, bytes, n);
            h->page_used[pg] = off + (uint32_t)n;
            h->page_parts[pg]++;
            struct hline *q = part(h, h->head++);
            *q = (struct hline){
                .off = (pg << 16) | off, .text_len = (uint32_t)n, .cells = (uint32_t)n,
                .flags = HLINE_NARROW, .wrap_rows = ROWS_UNKNOWN,
            };
            h->lines++;
            h->all_blank = false;
            if (h->wrap_cols)
                count_closed(h, q, rows_narrow((uint64_t)n, h->wrap_cols));
            return;
        }
        if (!wrapped && n <= HIST_PAGE_USABLE / 2 && push_line(h, NULL, n, bytes, pen, NULL))
            return;
        p = begin_line(h, (size_t)(n < HIST_PAGE_USABLE / 2 ? n : 0) + RUN_MAX * 2);
        if (!p)
            return;
    }
    while (n > 0) {
        int chunk = n < HIST_PAGE_USABLE / 2 ? n : HIST_PAGE_USABLE / 2;
        p = open_room(h, p, (size_t)chunk, RUN_MAX, (uint32_t)chunk);
        if (!p || !runs_reserve(&h->runs, RUN_MAX * 2))
            return;
        memcpy(part_text(h, p) + p->text_len, bytes, (size_t)chunk);
        hist_runs_add(&h->runs, pen, (uint32_t)chunk);
        struct enc e = { .bytes = (uint32_t)chunk, .cells = (uint32_t)chunk,
            .flags = (uint8_t)(HLINE_NARROW | (pen->link ? HLINE_HAS_LINK : 0)) };
        part_add(h, p, &e);
        bytes += chunk;
        n -= chunk;
    }
    if (!wrapped)
        wsap_vtcore_hist_close_open_line(h);
    else if (h->wrap_cols && h->walk_on)
        walk_advance(h);
}

static void text_row_readd(struct hist *h, const struct text_row *row)
{
    const struct hist_runs *rr = &row->runs;
    struct pen_attr pen = row->start;
    uint32_t count;
    for (const uint8_t *q = rr->data, *end = rr->data + rr->len; q < end;) {
        q = read_run(q, &pen, &count);
        hist_runs_add(&h->runs, &pen, count);
    }
    if (rr->count)
        hist_runs_add(&h->runs, &rr->pen, rr->count);
}

void wsap_vtcore_hist_push_text(struct hist *h, const struct text_row *row, bool wrapped)
{
    if (!h->line_limit)
        return;
    const struct hist_runs *rr = &row->runs;
    struct hline *p = open_part(h);
    size_t runs_need = (size_t)rr->len * RUN_MAX / 2 + 2 * RUN_MAX;
    if (!p) {
        if (row->cells == 0 && !wrapped) {
            wsap_vtcore_hist_push_blank(h, 1);
            return;
        }
        if (!wrapped && pen_attr_is_default(&row->start)) {
            if (h->lines >= h->line_limit)
                evict_line(h);
            int pg = page_for(h, (size_t)row->text_len + rr->len + RUN_MAX);
            if (pg < 0 || !ring_fit(h, 1))
                return;
            uint32_t off = h->page_used[pg];
            uint8_t *dst = h->pages[pg] + off;
            bytes_copy_small(dst, (const char *)row->text, row->text_len);
            uint8_t *ap = dst + row->text_len;
            bytes_copy_small(ap, (const char *)rr->data, (int)rr->len);
            uint8_t *end = ap + rr->len;
            if (rr->count && !pen_attr_is_default(&rr->pen))
                end = emit_run(end, &rr->base, &rr->pen, rr->count);
            struct hline *q = part(h, h->head++);
            *q = (struct hline){
                .off = ((uint32_t)pg << 16) | off,
                .text_len = row->text_len,
                .cells = row->cells,
                .wide = row->wide,
                .attr_len = (uint16_t)(end - ap),
                .flags = row->flags,
                .wrap_rows = ROWS_UNKNOWN,
            };
            h->page_used[pg] = (uint32_t)(end - h->pages[pg]);
            h->page_parts[pg]++;
            h->lines++;
            h->all_blank = false;
            if (h->wrap_cols)
                count_closed(h, q, cheap_rows(row->cells, row->wide, h->wrap_cols));
            return;
        }
        p = begin_line(h, (size_t)row->text_len + runs_need);
        if (!p)
            return;
    }
    if (h->runs.count)
        hist_runs_switch(&h->runs, text_pen_from_attr(&h->runs.pen));
    bool copy = pen_attr_equal(&h->runs.base, &row->start);
    struct hline *q = open_room(h, p, row->text_len, copy ? rr->len + RUN_MAX : runs_need,
                                row->cells);
    if (!q || !runs_reserve(&h->runs, runs_need + RUN_MAX))
        return;
    if (q != p && !(copy = pen_attr_equal(&h->runs.base, &row->start)) &&
        !(q = open_room(h, q, row->text_len, runs_need, row->cells)))
        return;
    p = q;
    memcpy(part_text(h, p) + p->text_len, row->text, row->text_len);
    if (copy) {
        memcpy(h->runs.data + h->runs.len, rr->data, rr->len);
        h->runs.len += rr->len;
        h->runs.base = rr->base;
        h->runs.pen = rr->pen;
        h->runs.count = rr->count;
    } else {
        text_row_readd(h, row);
    }
    part_add(h, p, &(struct enc){ row->text_len, row->cells, row->flags, row->wide });
    if (!wrapped)
        wsap_vtcore_hist_close_open_line(h);
    else if (h->wrap_cols && (h->walk_on || (p->flags & (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER))))
        walk_advance(h);
}

struct hist_stream_state {
    uint64_t head;
    uint64_t rows;
    uint32_t text_count;
    uint32_t base;
    uint32_t pg;
    const char *first;
};

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static int lf_rows_avx512(struct hist *restrict h, const uint16_t *restrict ends, int i, int stop,
                          uint32_t cols, uint32_t term, struct hist_stream_state *state)
{
    struct hline *restrict ring = h->ring;
    struct hist_block *restrict blocks = h->blocks;
    uint32_t mask = h->ring_mask;
    uint64_t head = state->head, rows = state->rows;
    uint32_t text_count = state->text_count;
    uint32_t pg = state->pg, base = state->base;
    const __m512i recip = _mm512_set1_epi32((int)(UINT64_C(0x100000000) / cols + 1));
    for (; i + 16 <= stop && (head & mask) + 16 <= mask + 1; i += 16, head += 16) {
        __m512i start = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(ends + i)));
        __m512i end = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i *)(ends + i + 1)));
        __m512i lens = _mm512_sub_epi32(_mm512_sub_epi32(end, start), _mm512_set1_epi32((int)term));
        __mmask16 empty = _mm512_cmpeq_epi32_mask(lens, _mm512_setzero_si512());
        uint32_t refs = 16 - wsap_vtcore_popcount32(empty);
        text_count += refs;
        __m512i offsets = _mm512_mask_mov_epi32(_mm512_add_epi32(start, _mm512_set1_epi32((int)base)),
                                               empty, _mm512_set1_epi32(-1));
        __m512i rs = _mm512_set1_epi32(1);
        uint32_t nr = 16;
        if (cols == 1) {
            rs = _mm512_max_epu32(lens, rs);
            nr = (uint32_t)_mm512_reduce_add_epi32(rs);
        } else if (_mm512_cmpgt_epu32_mask(lens, _mm512_set1_epi32((int)cols))) {
            __m512i num = _mm512_max_epi32(_mm512_sub_epi32(lens, _mm512_set1_epi32(1)),
                                          _mm512_setzero_si512());
            __m512i even = _mm512_srli_epi64(_mm512_mul_epu32(num, recip), 32);
            __m512i odd = _mm512_mul_epu32(_mm512_srli_epi64(num, 32), recip);
            rs = _mm512_add_epi32(_mm512_mask_blend_epi32(0xaaaa, even, odd), rs);
            nr = (uint32_t)_mm512_reduce_add_epi32(rs);
        }
        rows += nr - 16;
        blocks[(head & mask) / 16] = (struct hist_block){
            .seq = head + 1, .epoch = h->index_epoch,
            .page = pg, .refs = refs, .rows = nr, .cols = h->wrap_cols,
        };
        __m512i tags = _mm512_or_si512(_mm512_slli_epi32(_mm512_min_epu32(rs, _mm512_set1_epi32(255)), 24),
                                      _mm512_set1_epi32(HLINE_NARROW << 16));
        __m512i a = _mm512_unpacklo_epi32(offsets, lens);
        __m512i b = _mm512_unpackhi_epi32(offsets, lens);
        __m512i c = _mm512_unpacklo_epi32(lens, tags);
        __m512i d = _mm512_unpackhi_epi32(lens, tags);
        __m512i v0 = _mm512_unpacklo_epi64(a, c);
        __m512i v1 = _mm512_unpackhi_epi64(a, c);
        __m512i v2 = _mm512_unpacklo_epi64(b, d);
        __m512i v3 = _mm512_unpackhi_epi64(b, d);
        a = _mm512_shuffle_i32x4(v0, v1, 0x44);
        b = _mm512_shuffle_i32x4(v2, v3, 0x44);
        c = _mm512_shuffle_i32x4(v0, v1, 0xee);
        d = _mm512_shuffle_i32x4(v2, v3, 0xee);
        struct hline *dst = ring + (head & mask);
        _mm512_storeu_si512(dst, _mm512_shuffle_i32x4(a, b, 0x88));
        _mm512_storeu_si512(dst + 4, _mm512_shuffle_i32x4(a, b, 0xdd));
        _mm512_storeu_si512(dst + 8, _mm512_shuffle_i32x4(c, d, 0x88));
        _mm512_storeu_si512(dst + 12, _mm512_shuffle_i32x4(c, d, 0xdd));
    }
    state->head = head;
    state->rows = rows;
    state->text_count = text_count;
    return i;
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static int lf_rows_avx2(struct hist *restrict h, const uint16_t *restrict ends, int i, int stop,
                        uint32_t cols, uint32_t term, struct hist_stream_state *state)
{
    struct hline *restrict ring = h->ring;
    struct hist_block *restrict blocks = h->blocks;
    uint32_t mask = h->ring_mask;
    uint64_t head = state->head, rows = state->rows;
    uint32_t text_count = state->text_count;
    uint32_t pg = state->pg, base = state->base;
    uint32_t recip = (uint32_t)(UINT64_C(0x100000000) / cols + 1);
    uint32_t block_rows = 0, block_refs = 0;
    for (; i + 4 <= stop && (head & mask) + 4 <= mask + 1; i += 4, head += 4) {
        uint32_t nr = 4, refs;
        struct hline *dst = ring + (head & mask);
        __m128i start = _mm_cvtepu16_epi32(_mm_loadl_epi64((const __m128i *)(ends + i)));
        __m128i end = _mm_cvtepu16_epi32(_mm_loadl_epi64((const __m128i *)(ends + i + 1)));
        __m128i lens = _mm_sub_epi32(_mm_sub_epi32(end, start), _mm_set1_epi32((int)term));
        __m128i empty = _mm_cmpeq_epi32(lens, _mm_setzero_si128());
        refs = 4 - wsap_vtcore_popcount32((unsigned)_mm_movemask_ps(_mm_castsi128_ps(empty)));
        __m128i offsets = _mm_or_si128(_mm_add_epi32(start, _mm_set1_epi32((int)base)), empty);
        __m128i rs = _mm_set1_epi32(1);
        if (cols == 1) rs = _mm_max_epu32(lens, rs);
        else if (_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(lens, _mm_set1_epi32((int)cols))))) {
            __m128i num = _mm_max_epi32(_mm_sub_epi32(lens, _mm_set1_epi32(1)), _mm_setzero_si128());
            __m128i even = _mm_srli_epi64(_mm_mul_epu32(num, _mm_set1_epi32((int)recip)), 32);
            __m128i odd = _mm_mul_epu32(_mm_srli_epi64(num, 32), _mm_set1_epi32((int)recip));
            rs = _mm_add_epi32(_mm_blend_epi16(even, odd, 0xcc), rs);
        }
        __m128i sum = _mm_hadd_epi32(rs, rs);
        nr = (uint32_t)_mm_cvtsi128_si32(_mm_hadd_epi32(sum, sum));
        __m128i tags = _mm_or_si128(_mm_slli_epi32(_mm_min_epu32(rs, _mm_set1_epi32(255)), 24),
                                   _mm_set1_epi32(HLINE_NARROW << 16));
        __m128i a = _mm_unpacklo_epi32(offsets, lens), b = _mm_unpackhi_epi32(offsets, lens);
        __m128i c = _mm_unpacklo_epi32(lens, tags), d = _mm_unpackhi_epi32(lens, tags);
        _mm_storeu_si128((__m128i *)(dst + 0), _mm_unpacklo_epi64(a, c));
        _mm_storeu_si128((__m128i *)(dst + 1), _mm_unpackhi_epi64(a, c));
        _mm_storeu_si128((__m128i *)(dst + 2), _mm_unpacklo_epi64(b, d));
        _mm_storeu_si128((__m128i *)(dst + 3), _mm_unpackhi_epi64(b, d));
        text_count += refs;
        rows += nr - 4;
        block_rows += nr;
        block_refs += refs;
        if ((head & 15) == 12) {
            blocks[(head & mask) / 16] = (struct hist_block){
                .seq = head - 12 + 1, .epoch = h->index_epoch,
                .page = pg, .refs = block_refs, .rows = block_rows, .cols = h->wrap_cols,
            };
            block_rows = block_refs = 0;
        }
    }
    state->head = head;
    state->rows = rows;
    state->text_count = text_count;
    return i;
}
#endif

static inline int lf_rows(struct hist *restrict h, const uint16_t *restrict ends, int i, int stop,
                          uint32_t cols, uint32_t term, struct hist_stream_state *state)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return lf_rows_avx512(h, ends, i, stop, cols, term, state);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return lf_rows_avx2(h, ends, i, stop, cols, term, state);
#endif
#if defined(__aarch64__)
    struct hline *restrict ring = h->ring;
    struct hist_block *restrict blocks = h->blocks;
    uint32_t mask = h->ring_mask;
    uint64_t head = state->head, rows = state->rows;
    uint32_t text_count = state->text_count;
    uint32_t pg = state->pg, base = state->base;
    uint32_t recip = (uint32_t)(UINT64_C(0x100000000) / cols + 1);
    uint32_t block_rows = 0, block_refs = 0;
    for (; i + 4 <= stop && (head & mask) + 4 <= mask + 1; i += 4, head += 4) {
        uint32_t nr = 4, refs;
        struct hline *dst = ring + (head & mask);
        uint32x4_t start = vmovl_u16(vld1_u16(ends + i));
        uint32x4_t end = vmovl_u16(vld1_u16(ends + i + 1));
        uint32x4_t lens = vsubq_u32(vsubq_u32(end, start), vdupq_n_u32(term));
        uint32x4_t empty = vceqq_u32(lens, vdupq_n_u32(0));
        refs = 4 - vaddvq_u32(vshrq_n_u32(empty, 31));
        uint32x4_t offsets = vorrq_u32(vaddq_u32(start, vdupq_n_u32(base)), empty);
        uint32x4_t rs = vdupq_n_u32(1);
        if (cols == 1) rs = vmaxq_u32(lens, rs);
        else if (vmaxvq_u32(lens) > cols) {
            uint32x4_t num = vqsubq_u32(lens, vdupq_n_u32(1));
            uint32x2_t lo = vshrn_n_u64(vmull_n_u32(vget_low_u32(num), recip), 32);
            uint32x2_t hi = vshrn_n_u64(vmull_n_u32(vget_high_u32(num), recip), 32);
            rs = vaddq_u32(vcombine_u32(lo, hi), rs);
        }
        nr = vaddvq_u32(rs);
        uint32x4_t tags = vorrq_u32(vshlq_n_u32(vminq_u32(rs, vdupq_n_u32(255)), 24),
                                   vdupq_n_u32(HLINE_NARROW << 16));
        uint64x2_t a = vreinterpretq_u64_u32(vzip1q_u32(offsets, lens));
        uint64x2_t b = vreinterpretq_u64_u32(vzip2q_u32(offsets, lens));
        uint64x2_t c = vreinterpretq_u64_u32(vzip1q_u32(lens, tags));
        uint64x2_t d = vreinterpretq_u64_u32(vzip2q_u32(lens, tags));
        vst1q_u64((uint64_t *)(void *)(dst + 0), vzip1q_u64(a, c));
        vst1q_u64((uint64_t *)(void *)(dst + 1), vzip2q_u64(a, c));
        vst1q_u64((uint64_t *)(void *)(dst + 2), vzip1q_u64(b, d));
        vst1q_u64((uint64_t *)(void *)(dst + 3), vzip2q_u64(b, d));
        text_count += refs;
        rows += nr - 4;
        block_rows += nr;
        block_refs += refs;
        if ((head & 15) == 12) {
            blocks[(head & mask) / 16] = (struct hist_block){
                .seq = head - 12 + 1, .epoch = h->index_epoch,
                .page = pg, .refs = block_refs, .rows = block_rows, .cols = h->wrap_cols,
            };
            block_rows = block_refs = 0;
        }
    }
    state->head = head;
    state->rows = rows;
    state->text_count = text_count;
#else
    (void)h;
    (void)ends;
    (void)term;
    (void)stop;
    (void)cols;
    (void)state;
#endif
    return i;
}

WSAP_VTCORE_NOINLINE
void wsap_vtcore_hist_push_ascii_lf(struct hist *restrict h, const char *restrict bytes,
                        const uint16_t *restrict ends, int count, int term)
{
    if (!h->line_limit) return;
    int i = 0;
    if (count && hist_is_open(h)) {
        wsap_vtcore_hist_push_ascii(h, bytes + ends[0], ends[1] - ends[0] - term, NULL, false);
        i++;
    }
    uint32_t cols = h->wrap_cols ? h->wrap_cols : 1;
    while (i < count) {
        int stop = count - i > (int)h->line_limit ? i + (int)h->line_limit : count;
        if (page_for(h, ends[i + 1] - ends[i]) < 0) return;
        uint32_t pg = h->cur_page;
        uint32_t room = HIST_PAGE_USABLE - h->page_used[pg];
        if ((uint32_t)(ends[stop] - ends[i]) > room) {
            int lo = i + 1, hi = stop;
            while (lo < hi) {
                int mid = lo + (hi - lo + 1) / 2;
                if ((uint32_t)(ends[mid] - ends[i]) <= room) lo = mid;
                else hi = mid - 1;
            }
            stop = lo;
        }
        uint32_t n = (uint32_t)(stop - i);
        evict_for(h, n);
        if (!ring_fit(h, n)) return;
        bool count_rows = h->wrap_cols && h->rows_id == hist_end_id(h);
        uint32_t off = h->page_used[pg];
        uint32_t base = (pg << 16) + off - ends[i];
        uint32_t span = ends[stop] - ends[i];
        memcpy(h->pages[pg] + off, bytes + ends[i], span);
        struct hist_stream_state state = {
            .head = h->head,
            .rows = n,
            .text_count = 0,
            .base = base,
            .pg = pg,
        };
        struct hline *restrict ring = h->ring;
        uint32_t mask = h->ring_mask;
        while (i < stop && (state.head & 15)) {
            uint32_t len = ends[i + 1] - ends[i] - (uint32_t)term;
            struct hline *dst = ring + (state.head & mask);
            if (!len) *dst = HLINE_BLANK;
            else {
                uint32_t r = rows_narrow(len, (int)cols);
                part_put_narrow_rows(dst, base + ends[i], len, r);
                state.rows += r - 1;
                state.text_count++;
            }
            i++;
            state.head++;
        }
        i = lf_rows(h, ends, i, stop, cols, (uint32_t)term, &state);
        for (; i < stop; i++, state.head++) {
            uint32_t len = ends[i + 1] - ends[i] - (uint32_t)term;
            struct hline *dst = ring + (state.head & mask);
            if (!len) *dst = HLINE_BLANK;
            else {
                uint32_t r = rows_narrow(len, (int)cols);
                part_put_narrow_rows(dst, base + ends[i], len, r);
                state.rows += r - 1;
                state.text_count++;
            }
        }
        h->head = state.head;
        h->page_used[pg] = off + (state.text_count ? span : 0);
        h->page_parts[pg] += state.text_count;
        h->lines += n;
        if (state.text_count) h->all_blank = false;
        if (count_rows) {
            h->counted_rows += state.rows;
            h->rows_id += n;
        }
    }
}

static_assert(sizeof(struct hist_ascii_line) == 16, "a line is two 64-bit lanes");
static_assert(offsetof(struct hist_ascii_line, len) == 8, "len is in the second 64-bit lane");

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static int stream_lines_avx512(struct hist *restrict h, const struct hist_ascii_line *lines,
                              int i, int stream_end, struct hist_stream_state *state)
{
    uint64_t head = state->head;
    uint32_t text_count = state->text_count;
    uint32_t base = state->base, pg = state->pg;
    const char *first = state->first;
    const __m512i even = _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14);
    const __m512i odd = _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15);
    for (; i + 16 <= stream_end &&
           (head & h->ring_mask) + 16 <= h->ring_mask + 1; i += 16, head += 16) {
        __m512i q0 = _mm512_loadu_si512(lines + i);
        __m512i q1 = _mm512_loadu_si512(lines + i + 4);
        __m512i q2 = _mm512_loadu_si512(lines + i + 8);
        __m512i q3 = _mm512_loadu_si512(lines + i + 12);
        __m512i p0 = _mm512_permutex2var_epi64(q0, even, q1);
        __m512i p1 = _mm512_permutex2var_epi64(q2, even, q3);
        __m512i origin = _mm512_set1_epi64((uintptr_t)first);
        __m256i lo = _mm512_cvtepi64_epi32(_mm512_sub_epi64(p0, origin));
        __m256i hi = _mm512_cvtepi64_epi32(_mm512_sub_epi64(p1, origin));
        __m512i offsets = _mm512_add_epi32(
            _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1),
            _mm512_set1_epi32((int)base));
        lo = _mm512_cvtepi64_epi32(_mm512_permutex2var_epi64(q0, odd, q1));
        hi = _mm512_cvtepi64_epi32(_mm512_permutex2var_epi64(q2, odd, q3));
        __m512i lens = _mm512_and_si512(
            _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1),
            _mm512_set1_epi32(UINT16_MAX));
        __mmask16 empty = _mm512_cmpeq_epi32_mask(lens, _mm512_setzero_si512());
        uint32_t refs = 16 - wsap_vtcore_popcount32(empty);
        text_count += refs;
        offsets = _mm512_mask_mov_epi32(offsets, empty, _mm512_set1_epi32(-1));
        __m512i tags = _mm512_set1_epi32((1u << 24) | (HLINE_NARROW << 16));
        __m512i a = _mm512_unpacklo_epi32(offsets, lens);
        __m512i b = _mm512_unpackhi_epi32(offsets, lens);
        __m512i c = _mm512_unpacklo_epi32(lens, tags);
        __m512i d = _mm512_unpackhi_epi32(lens, tags);
        __m512i v0 = _mm512_unpacklo_epi64(a, c);
        __m512i v1 = _mm512_unpackhi_epi64(a, c);
        __m512i v2 = _mm512_unpacklo_epi64(b, d);
        __m512i v3 = _mm512_unpackhi_epi64(b, d);
        a = _mm512_shuffle_i32x4(v0, v1, 0x44);
        b = _mm512_shuffle_i32x4(v2, v3, 0x44);
        c = _mm512_shuffle_i32x4(v0, v1, 0xee);
        d = _mm512_shuffle_i32x4(v2, v3, 0xee);
        struct hline *dst = h->ring + (head & h->ring_mask);
        _mm512_storeu_si512(dst, _mm512_shuffle_i32x4(a, b, 0x88));
        _mm512_storeu_si512(dst + 4, _mm512_shuffle_i32x4(a, b, 0xdd));
        _mm512_storeu_si512(dst + 8, _mm512_shuffle_i32x4(c, d, 0x88));
        _mm512_storeu_si512(dst + 12, _mm512_shuffle_i32x4(c, d, 0xdd));
        if (!(head & 15)) {
            h->blocks[(head & h->ring_mask) / 16] = (struct hist_block){
                .seq = head + 1, .epoch = h->index_epoch,
                .page = pg, .refs = refs, .rows = 16, .cols = h->wrap_cols,
            };
        }
    }
    state->head = head;
    state->text_count = text_count;
    return i;
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static int stream_lines_avx2(struct hist *restrict h, const struct hist_ascii_line *lines,
                            int i, int stream_end, struct hist_stream_state *state)
{
    uint64_t head = state->head;
    uint32_t text_count = state->text_count;
    uint32_t base = state->base;
    const char *first = state->first;
    for (; i + 4 <= stream_end &&
           (head & h->ring_mask) + 4 <= h->ring_mask + 1; i += 4, head += 4) {
        struct hline *dst = h->ring + (head & h->ring_mask);
        uint32_t refs;
        __m128i index = _mm_setr_epi32(0, 16, 32, 48);
        __m128i ptrs = _mm_i32gather_epi32((const int *)(lines + i), index, 1);
        __m128i lens = _mm_and_si128(
            _mm_i32gather_epi32((const int *)((const char *)(lines + i) + 8), index, 1),
            _mm_set1_epi32(UINT16_MAX));
        __m128i empty = _mm_cmpeq_epi32(lens, _mm_setzero_si128());
        refs = 4 - wsap_vtcore_popcount32((unsigned)_mm_movemask_ps(_mm_castsi128_ps(empty)));
        __m128i offsets = _mm_add_epi32(ptrs,
            _mm_set1_epi32((int)(base - (uint32_t)(uintptr_t)first)));
        offsets = _mm_or_si128(offsets, empty);
        __m128i tags = _mm_set1_epi32((1u << 24) | (HLINE_NARROW << 16));
        __m128i a = _mm_unpacklo_epi32(offsets, lens);
        __m128i b = _mm_unpackhi_epi32(offsets, lens);
        __m128i c = _mm_unpacklo_epi32(lens, tags);
        __m128i d = _mm_unpackhi_epi32(lens, tags);
        _mm256_storeu_si256((__m256i *)dst, _mm256_inserti128_si256(
            _mm256_castsi128_si256(_mm_unpacklo_epi64(a, c)), _mm_unpackhi_epi64(a, c), 1));
        _mm256_storeu_si256((__m256i *)(dst + 2), _mm256_inserti128_si256(
            _mm256_castsi128_si256(_mm_unpacklo_epi64(b, d)), _mm_unpackhi_epi64(b, d), 1));
        text_count += refs;
    }
    state->head = head;
    state->text_count = text_count;
    return i;
}
#endif

static inline int stream_lines(struct hist *restrict h, const struct hist_ascii_line *lines,
                             int i, int stream_end, struct hist_stream_state *state)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return stream_lines_avx512(h, lines, i, stream_end, state);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return stream_lines_avx2(h, lines, i, stream_end, state);
#endif
#if defined(__aarch64__)
    uint64_t head = state->head;
    uint32_t text_count = state->text_count;
    uint32_t base = state->base;
    const char *first = state->first;
    for (; i + 4 <= stream_end &&
           (head & h->ring_mask) + 4 <= h->ring_mask + 1; i += 4, head += 4) {
        struct hline *dst = h->ring + (head & h->ring_mask);
        uint32_t refs;
        uint32x4x4_t q = vld4q_u32((const uint32_t *)(lines + i));
        uint32x4_t lens = vandq_u32(q.val[2], vdupq_n_u32(UINT16_MAX));
        uint32x4_t empty = vceqq_u32(lens, vdupq_n_u32(0));
        refs = 4 - vaddvq_u32(vshrq_n_u32(empty, 31));
        uint32x4_t offsets = vorrq_u32(empty, vaddq_u32(q.val[0],
            vdupq_n_u32(base - (uint32_t)(uintptr_t)first)));
        uint32x4x4_t parts = { .val = {
            offsets, lens, lens, vdupq_n_u32((1u << 24) | (HLINE_NARROW << 16)),
        } };
        vst4q_u32((uint32_t *)dst, parts);
        text_count += refs;
    }
    state->head = head;
    state->text_count = text_count;
#else
    (void)h;
    (void)lines;
    (void)stream_end;
    (void)state;
#endif
    return i;
}

void wsap_vtcore_hist_push_ascii_lines(struct hist *h, const struct hist_ascii_line *lines, int count,
                           const struct pen_attr *pen)
{
    if (!h->line_limit)
        return;
    bool def = !pen || pen_attr_is_default(pen);
    uint16_t cols = h->wrap_cols ? h->wrap_cols : 1;
    for (int i = 0; i < count;) {
        const struct hist_ascii_line *l = &lines[i];
        if (l->wrapped) {
            int end = i + 1;
            int bytes = l->len;
            while (end < count && lines[end].wrapped &&
                   lines[end].ptr == l->ptr + bytes &&
                   lines[end].len <= INT32_MAX - bytes) {
                bytes += lines[end++].len;
            }
            wsap_vtcore_hist_push_ascii(h, l->ptr, bytes, pen, true);
            i = end;
            continue;
        }
        if (!def || hist_is_open(h)) {
            if (l->len == 0 && !l->wrapped)
                wsap_vtcore_hist_push_blank(h, 1);
            else
                wsap_vtcore_hist_push_ascii(h, l->ptr, l->len, pen, l->wrapped);
            i++;
            continue;
        }
        if (page_for(h, l->len) < 0)
            return;
        uint32_t pg = h->cur_page;
        uint32_t room = HIST_PAGE_USABLE - h->page_used[pg];
        int end = i;
        uint32_t bytes = 0;
        int stream_end = i;
        uint32_t extra_rows = 0;
        while (stream_end < count && stream_end - i < (int)h->line_limit) {
            const struct hist_ascii_line *next = &lines[stream_end];
            if (next->wrapped || next->len > HIST_PAGE_USABLE / 2 ||
                (stream_end > i &&
                 (next->ptr != lines[stream_end - 1].ptr + lines[stream_end - 1].len + 1 ||
                  next->ptr[-1] != '\n')))
                break;
            size_t span = (size_t)(next->ptr - l->ptr) + next->len;
            if (span > room)
                break;
            if (WSAP_VTCORE_EXPECT(next->len > (uint32_t)cols, 0))
                extra_rows += rows_narrow(next->len, cols) - 1;
            stream_end++;
        }
        if (stream_end - i >= 4) {
            uint32_t n = (uint32_t)(stream_end - i);
            uint64_t stream_rows = (uint64_t)n + extra_rows;
            const struct hist_ascii_line *last = &lines[stream_end - 1];
            uint32_t span = (uint32_t)(last->ptr - l->ptr) + last->len;
            evict_for(h, n);
            if (!ring_fit(h, n))
                return;
            uint32_t off = h->page_used[pg];
            memcpy(h->pages[pg] + off, l->ptr, span);
            uint32_t base = (pg << 16) | off;
            struct hist_stream_state state = {
                .head = h->head,
                .text_count = 0,
                .base = base,
                .pg = pg,
                .first = l->ptr,
            };
            if (!extra_rows)
                i = stream_lines(h, lines, i, stream_end, &state);
            for (; i < stream_end; i++) {
                uint32_t len = lines[i].len;
                struct hline *dst = &h->ring[state.head++ & h->ring_mask];
                if (len) {
                    uint32_t line_off = base + (uint32_t)(lines[i].ptr - l->ptr);
                    if (WSAP_VTCORE_EXPECT(len <= cols, 1)) {
                        part_put_narrow(dst, line_off, len);
                    } else {
                        part_put_narrow_rows(dst, line_off, len, rows_narrow(len, cols));
                    }
                    state.text_count++;
                } else {
                    *dst = HLINE_BLANK;
                }
            }
            if (h->wrap_cols && h->rows_id == hist_end_id(h)) {
                h->counted_rows += stream_rows;
                h->rows_id += n;
            }
            h->head = state.head;
            h->page_used[pg] = off + span;
            h->page_parts[pg] += state.text_count;
            h->lines += n;
            if (state.text_count)
                h->all_blank = false;
            continue;
        }
        int batch_max = (int)h->line_limit;
        while (end < count && !lines[end].wrapped &&
               bytes + lines[end].len <= room && end - i < batch_max) {
            bytes += lines[end].len;
            end++;
        }
        uint32_t n = (uint32_t)(end - i);
        evict_for(h, n);
        if (!ring_fit(h, n))
            return;
        bool count_rows = h->wrap_cols && h->rows_id == hist_end_id(h);
        uint64_t rows_add = 0;
        uint32_t n_text = 0;
        bool any_text = false;
        uint32_t off = h->page_used[pg];
        uint8_t *page = h->pages[pg];
        struct hline *ring = h->ring;
        uint32_t rmask = h->ring_mask;
        uint64_t head = h->head;
        uint32_t page_off = pg << 16;
        for (; i < end; i++) {
            uint32_t len = lines[i].len;
            if (WSAP_VTCORE_EXPECT(len == 0, 0)) {
                ring[head++ & rmask] = HLINE_BLANK;
                rows_add++;
                continue;
            }
            bytes_copy_small(page + off, lines[i].ptr, (int)len);
            n_text++;
            any_text = true;
            if (WSAP_VTCORE_EXPECT(len <= cols, 1)) {
                part_put_narrow(&ring[head++ & rmask], page_off | off, len);
                rows_add++;
            } else {
                uint32_t rows = rows_narrow(len, cols);
                part_put_narrow_rows(&ring[head++ & rmask], page_off | off, len, rows);
                rows_add += rows;
            }
            off += len;
        }
        h->head = head;
        h->page_used[pg] = off;
        h->page_parts[pg] += n_text;
        h->lines += n;
        if (any_text)
            h->all_blank = false;
        if (count_rows) {
            h->counted_rows += rows_add;
            h->rows_id += n;
        }
    }
}

void wsap_vtcore_hist_push_blank(struct hist *h, size_t n)
{
    if (!h->line_limit || n == 0)
        return;
    if (hist_is_open(h)) {
        wsap_vtcore_hist_close_open_line(h);
        n--;
    }
    if (n == 0)
        return;
    if (h->all_blank && h->lines == h->line_limit) {
        bool all = h->rows_id == hist_end_id(h);
        h->base_id += n;
        if (all || h->rows_id < h->base_id)
            h->rows_id = all ? hist_end_id(h) : h->base_id;
        h->counted_rows = h->rows_id - h->base_id;
        return;
    }
    if (n >= h->line_limit) {
        uint64_t end = hist_end_id(h) + n;
        wsap_vtcore_hist_clear(h);
        h->base_id = h->rows_id = end - h->line_limit;
        n = h->line_limit;
        h->all_blank = true;
    }
    evict_for(h, n);
    if (!ring_fit(h, n))
        return;
    bool count = h->wrap_cols && h->rows_id == hist_end_id(h);
    struct hline *ring = h->ring;
    uint32_t rmask = h->ring_mask;
    uint64_t head = h->head;
    for (size_t i = 0; i < n; i++)
        ring[head++ & rmask] = HLINE_BLANK;
    h->head = head;
    h->lines += n;
    if (count) {
        h->rows_id += n;
        h->counted_rows += n;
    }
}

struct runs_iter {
    const uint8_t *p, *end;
    bool pending;
    struct pen_attr pending_pen;
    uint32_t pending_count;
    struct pen_attr pen;
    uint32_t left;
};

static void runs_iter_runs(struct runs_iter *it, const struct hist_runs *r, const struct pen_attr *start)
{
    *it = (struct runs_iter){
        .p = r->data, .end = r->data ? r->data + r->len : NULL,
        .pending = r->count > 0, .pending_pen = r->pen, .pending_count = r->count,
        .pen = *start,
    };
}

static void runs_iter_init(struct runs_iter *it, const struct hist *h, const struct hline *p)
{
    *it = (struct runs_iter){ .pen = pen_attr_default() };
    if (p->flags & HLINE_OPEN) {
        if (h->runs.len) {
            it->p = h->runs.data;
            it->end = h->runs.data + h->runs.len;
        }
        it->pending = h->runs.count > 0;
        it->pending_pen = h->runs.pen;
        it->pending_count = h->runs.count;
    } else if (p->attr_len) {
        it->p = part_text(h, p) + p->text_len;
        it->end = it->p + p->attr_len;
    }
}

static inline uint32_t runs_take(struct runs_iter *it, uint32_t n)
{
    while (WSAP_VTCORE_EXPECT(it->left == 0, 0)) {
        if (it->p < it->end) {
            it->p = read_run(it->p, &it->pen, &it->left);
        } else if (it->pending) {
            it->pending = false;
            it->pen = it->pending_pen;
            it->left = it->pending_count;
        } else {
            it->pen = pen_attr_default();
            it->left = UINT32_MAX;
        }
    }
    if (it->left == UINT32_MAX)
        return n;
    uint32_t k = it->left < n ? it->left : n;
    it->left -= k;
    return k;
}

static inline void runs_iter_save(const struct runs_iter *it, struct hist_dec_cache *d)
{
    d->p = it->p;
    d->end = it->end;
    d->pending = it->pending;
    d->pending_pen = it->pending_pen;
    d->pending_count = it->pending_count;
    d->pen = it->pen;
    d->left = it->left;
}

static inline void runs_iter_restore(struct runs_iter *it, const struct hist_dec_cache *d)
{
    it->p = d->p;
    it->end = d->end;
    it->pending = d->pending;
    it->pending_pen = d->pending_pen;
    it->pending_count = d->pending_count;
    it->pen = d->pen;
    it->left = d->left;
}

static inline int text_width(const uint8_t *s)
{
    uint8_t b = s[0];
    if (b < PREFIX_WIDE || b == PREFIX_TEXT)
        return 1;
    if (b != PREFIX_CLUSTER)
        return 2;
    return (s[1] & 0x80) ? 2 : 1;
}

static inline const uint8_t *text_skip(const uint8_t *s, const uint8_t *end)
{
    uint8_t b = s[0];
    uint32_t cp;
    if (b < 0x80)
        return s + 1;
    if (b < PREFIX_WIDE)
        return s + get_utf8(s, end, &cp);
    if (b != PREFIX_CLUSTER)
        return s + 1 + get_utf8(s + 1, end, &cp);
    uint8_t k = s[1];
    s += 2;
    for (int i = 0; i < (k & 0x1f) && s < end; i++)
        s += get_utf8(s, end, &cp);
    return s;
}

static const uint8_t *text_cell(const uint8_t *s, const uint8_t *end, struct gstore *g,
                                uint32_t *cp, uint16_t *shape)
{
    uint8_t b = s[0];
    *shape = 0;
    if (b < 0x80) {
        *cp = b;
        return s + 1;
    }
    if (b < PREFIX_WIDE)
        return s + get_utf8(s, end, cp);
    if (b != PREFIX_CLUSTER) {
        *shape = b == PREFIX_WIDE ? CELL_WIDE : b == PREFIX_EMOJI ? (CELL_WIDE | CELL_EMOJI) : CELL_TEXT;
        return s + 1 + get_utf8(s + 1, end, cp);
    }
    uint8_t k = s[1];
    s += 2;
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = 0;
    for (int i = 0; i < (k & 0x1f) && s < end; i++) {
        uint32_t c;
        s += get_utf8(s, end, &c);
        if (n < WSAP_VTCORE_GRAPHEME_MAX_LEN)
            cps[n++] = c;
    }
    *cp = n == 0 ? ' ' : (g && n > 1) ? wsap_vtcore_gstore_intern(g, cps, n) : cps[0];
    *shape = (uint16_t)(((k & 0x80) ? CELL_WIDE : 0) | ((k & 0x40) ? CELL_EMOJI : 0) |
                        ((k & 0x20) ? CELL_TEXT : 0));
    return s;
}

uint64_t wsap_vtcore_hist_line_cells(const struct hist *h, uint64_t id)
{
    if (!line_valid(h, id))
        return 0;
    if (id == h->last_id && h->last_cells)
        return h->last_cells;
    uint64_t first = line_first(h, id), end = line_end(h, first);
    if (first + 1 == end)
        return part(h, first)->cells;
    uint64_t cells = 0;
    for (uint64_t s = first; s < end; s++)
        cells += part(h, s)->cells;
    return cells;
}

uint64_t wsap_vtcore_hist_line_wide(const struct hist *h, uint64_t id)
{
    if (!line_valid(h, id))
        return 0;
    if (id == h->last_id && h->last_cells)
        return h->last_wide;
    uint64_t first = line_first(h, id), end = line_end(h, first);
    if (first + 1 == end)
        return part(h, first)->wide;
    uint64_t wide = 0;
    for (uint64_t seq = first; seq < end; seq++)
        wide += part(h, seq)->wide;
    return wide;
}

uint8_t wsap_vtcore_hist_line_flags(const struct hist *h, uint64_t id)
{
    if (!line_valid(h, id))
        return 0;
    uint64_t first = line_first(h, id), end = line_end(h, first);
    uint8_t flags = part(h, first)->flags;
    if (first + 1 == end)
        return flags;
    for (uint64_t s = first + 1; s < end; s++) {
        uint8_t f = part(h, s)->flags;
        flags = (uint8_t)((flags | f) & ~HLINE_NARROW) | (flags & f & HLINE_NARROW);
    }
    return flags;
}

static int line_rows_walk(const struct hist *h, uint64_t first, uint64_t end, int cols)
{
    int rows = 1, col = 0;
    for (uint64_t s = first; s < end; s++) {
        const struct hline *p = part(h, s);
        if (!p->text_len)
            continue;
        const uint8_t *t = part_text(h, p), *end = t + p->text_len;
        while (t < end) {
            int w = text_width(t) == 2 && cols > 1 ? 2 : 1;
            t = text_skip(t, end);
            if (col == cols || (w == 2 && col + 2 > cols)) {
                rows++;
                col = 0;
            }
            col += w;
        }
    }
    return rows;
}

int wsap_vtcore_hist_line_rows(const struct hist *h, uint64_t id, int cols)
{
    if (cols <= 0 || !line_valid(h, id))
        return 0;
    uint64_t first = line_first(h, id);
    const struct hline *f = part(h, first);
    if (cols == h->wrap_cols) {
        if (id < h->rows_id &&
            ((f->flags & HLINE_ROWS_EXACT) || !(wsap_vtcore_hist_line_flags(h, id) & HLINE_HAS_WIDE))) {
            if (f->wrap_rows != ROWS_UNKNOWN)
                return f->wrap_rows;
            if ((f->flags & HLINE_ROWS_EXACT) && h->exact_rows)
                return (int)h->exact_rows[first & h->ring_mask];
        }
        if (h->walk_on && id + 1 == hist_end_id(h) && h->walk_seq + 1 == h->head &&
            h->walk_off == part(h, h->walk_seq)->text_len)
            return (int)h->walk_rows;
    }
    uint64_t cells = wsap_vtcore_hist_line_cells(h, id);
    if (cells <= (uint64_t)cols ||
        !(wsap_vtcore_hist_line_flags(h, id) & (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER)))
        return (int)rows_narrow(cells, cols);
    return line_rows_walk(h, first, line_end(h, first), cols);
}

static void walk_advance(struct hist *h)
{
    int cols = h->wrap_cols;
    if (!h->walk_on) {
        h->walk_seq = line_first(h, hist_end_id(h) - 1);
        h->walk_off = 0;
        h->walk_col = 0;
        h->walk_rows = 1;
        h->walk_on = true;
    }
    for (;;) {
        const struct hline *p = part(h, h->walk_seq);
        if (p->text_len) {
            const uint8_t *t0 = part_text(h, p), *t = t0 + h->walk_off, *end = t0 + p->text_len;
            uint32_t col = h->walk_col, rows = h->walk_rows;
            while (t < end) {
                uint32_t w = text_width(t) == 2 && cols > 1 ? 2 : 1;
                t = text_skip(t, end);
                if (col == (uint32_t)cols || (w == 2 && col + 2 > (uint32_t)cols)) {
                    rows++;
                    col = 0;
                }
                col += w;
            }
            h->walk_col = col;
            h->walk_rows = rows;
            h->walk_off = (uint32_t)(t - t0);
        }
        if (h->walk_seq + 1 >= h->head)
            break;
        h->walk_seq++;
        h->walk_off = 0;
    }
}

static inline void put_cell(struct cell *out, uint32_t cp, uint16_t shape, const struct pen_attr *pen)
{
    *out = (struct cell){ cp, pen->fg, pen->bg, (uint16_t)(shape | pen->flags), pen->link };
}

static bool line_narrow(const struct hist *h, uint64_t first, uint64_t end)
{
    for (uint64_t s = first; s < end; s++)
        if (!(part(h, s)->flags & HLINE_NARROW))
            return false;
    return true;
}

int wsap_vtcore_hist_decode_segments(const struct hist *h, uint64_t id, int start_seg, int count, int cols,
                         struct cell *out, int *lens, struct gstore *g)
{
    if (count <= 0 || start_seg < 0 || cols <= 0 || !line_valid(h, id))
        return 0;
    uint64_t first = line_first(h, id), end = line_end(h, first);

    if (line_narrow(h, first, end)) {
        uint64_t skip = (uint64_t)start_seg * (uint64_t)cols;
        int seg_idx = 0;
        int n = 0;
        for (uint64_t s = first; s < end; s++) {
            const struct hline *p = part(h, s);
            if (skip >= p->cells) {
                skip -= p->cells;
                continue;
            }
            uint64_t avail = p->cells - skip;
            struct runs_iter it;
            runs_iter_init(&it, h, p);
            for (uint64_t k = skip; k > 0;)
                k -= runs_take(&it, (uint32_t)k);
            const char *src = (const char *)part_text(h, p) + skip;
            while (avail > 0 && seg_idx < count) {
                int need = cols - n;
                int take = avail > (uint64_t)need ? need : (int)avail;
                struct cell *row_out = out + (size_t)seg_idx * (size_t)cols;
                for (int x = 0; x < take;) {
                    int k = (int)runs_take(&it, (uint32_t)(take - x));
                    put_ascii_chunk(row_out + n + x, src + x, k, it.pen.fg, it.pen.bg,
                                    (uint32_t)it.pen.flags | ((uint32_t)it.pen.link << 16));
                    x += k;
                }
                n += take;
                src += take;
                avail -= (uint64_t)take;
                if (n == cols) {
                    if (lens)
                        lens[seg_idx] = cols;
                    seg_idx++;
                    n = 0;
                }
            }
            skip = 0;
            if (seg_idx == count)
                return count;
        }
        if (seg_idx < count) {
            if (lens)
                lens[seg_idx] = n;
            seg_idx++;
        }
        return seg_idx;
    }

    if (g)
        wsap_vtcore_gstore_pin(g);
    int row = 0, col = 0;
    int end_seg = start_seg + count;
    uint64_t s = first;
    struct runs_iter it;
    struct hist_dec_cache *dec = &((struct hist *)(uintptr_t)h)->dec;
    bool resumed = false;

    if (dec->valid && dec->epoch == h->index_epoch && dec->id == id &&
        dec->cols == cols && dec->seg == (uint32_t)start_seg &&
        dec->seq >= first && dec->seq < end &&
        dec->off <= part(h, dec->seq)->text_len) {
        s = dec->seq;
        row = start_seg;
        col = (int)dec->col;
        runs_iter_restore(&it, dec);
        resumed = true;
    }

    for (; s < end; s++) {
        const struct hline *p = part(h, s);
        if (!p->text_len)
            continue;
        const uint8_t *t = part_text(h, p), *tend = t + p->text_len;
        if (resumed && s == dec->seq) {
            t += dec->off;
            resumed = false;
        } else {
            uint32_t skipped_cells = 0;
            if (row < start_seg) {
                while (t < tend && row < start_seg) {
                    int w = text_width(t) == 2 && cols > 1 ? 2 : 1;
                    if (col == cols || (w == 2 && col + 2 > cols)) {
                        row++;
                        col = 0;
                        if (row == start_seg)
                            break;
                    }
                    skipped_cells++;
                    col += w;
                    t = text_skip(t, tend);
                }
                if (row < start_seg)
                    continue;
            }

            runs_iter_init(&it, h, p);
            for (uint32_t k = skipped_cells; k > 0;)
                k -= runs_take(&it, k);
        }

        while (t < tend && row < end_seg) {
            int w = text_width(t) == 2 && cols > 1 ? 2 : 1;
            if (col == cols || (w == 2 && col + 2 > cols)) {
                int seg_idx = row - start_seg;
                struct cell *row_out = out + (size_t)seg_idx * (size_t)cols;
                if (col < cols)
                    row_out[col] = (struct cell){ ' ', WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, CELL_WRAP_PAD, 0 };
                if (lens)
                    lens[seg_idx] = col < cols ? cols : col;
                row++;
                col = 0;
                if (row == end_seg) {
                    if (g)
                        wsap_vtcore_gstore_unpin(g);
                    dec->epoch = h->index_epoch;
                    dec->id = id;
                    dec->cols = (uint16_t)cols;
                    dec->seq = s;
                    dec->seg = (uint32_t)end_seg;
                    dec->off = (uint32_t)(t - part_text(h, p));
                    dec->col = 0;
                    runs_iter_save(&it, dec);
                    dec->valid = true;
                    return count;
                }
            }
            int seg_idx = row - start_seg;
            struct cell *row_out = out + (size_t)seg_idx * (size_t)cols;
            runs_take(&it, 1);
            uint32_t cp;
            uint16_t shape;
            t = text_cell(t, tend, g, &cp, &shape);
            put_cell(&row_out[col], cp, shape, &it.pen);
            if (w == 2)
                put_cell(&row_out[col + 1], 0, (uint16_t)((shape & ~CELL_WIDE) | CELL_TRAIL), &it.pen);
            col += w;
        }
        if (row == end_seg) {
            if (g)
                wsap_vtcore_gstore_unpin(g);
            dec->epoch = h->index_epoch;
            dec->id = id;
            dec->cols = (uint16_t)cols;
            dec->seq = s;
            dec->seg = (uint32_t)end_seg;
            dec->off = (uint32_t)(t - part_text(h, p));
            dec->col = (uint32_t)col;
            runs_iter_save(&it, dec);
            dec->valid = true;
            return count;
        }
    }
    int decoded = row - start_seg;
    if (row >= start_seg && row < end_seg) {
        if (lens)
            lens[decoded] = col;
        decoded++;
    }
    dec->valid = false;
    if (g)
        wsap_vtcore_gstore_unpin(g);
    return decoded;
}

int wsap_vtcore_hist_decode_segment(const struct hist *h, uint64_t id, int seg, int cols,
                        struct cell *out, struct gstore *g)
{
    int len = 0;
    int got = wsap_vtcore_hist_decode_segments(h, id, seg, 1, cols, out, &len, g);
    return got > 0 ? len : 0;
}

static int decode_line(const struct hist *h, uint64_t first, uint64_t end,
                       struct cell *out, int cap, struct gstore *g)
{
    int n = 0;
    if (g)
        wsap_vtcore_gstore_pin(g);
    for (uint64_t s = first; s < end; s++) {
        const struct hline *p = part(h, s);
        if (!p->text_len)
            continue;
        struct runs_iter it;
        runs_iter_init(&it, h, p);
        const uint8_t *t = part_text(h, p), *tend = t + p->text_len;
        while (t < tend) {
            uint32_t cp;
            uint16_t shape;
            runs_take(&it, 1);
            t = text_cell(t, tend, g, &cp, &shape);
            int w = (shape & CELL_WIDE) ? 2 : 1;
            if (n + w <= cap) {
                put_cell(&out[n], cp, shape, &it.pen);
                if (w == 2)
                    put_cell(&out[n + 1], 0, (uint16_t)((shape & ~CELL_WIDE) | CELL_TRAIL), &it.pen);
            }
            n += w;
        }
    }
    if (g)
        wsap_vtcore_gstore_unpin(g);
    return n;
}

int wsap_vtcore_hist_text_decode(const struct text_row *row, struct cell *out, int cap, struct gstore *g)
{
    struct runs_iter it;
    runs_iter_runs(&it, &row->runs, &row->start);
    if (row->flags & HLINE_NARROW) {
        int take = row->text_len < cap ? row->text_len : cap;
        for (int x = 0; x < take;) {
            int k = (int)runs_take(&it, (uint32_t)(take - x));
            put_ascii_chunk(out + x, (const char *)row->text + x, k, it.pen.fg, it.pen.bg,
                            (uint32_t)it.pen.flags | ((uint32_t)it.pen.link << 16));
            x += k;
        }
        return take;
    }
    const uint8_t *t = row->text, *tend = row->text + row->text_len;
    int n = 0;
    while (t < tend) {
        uint32_t cp;
        uint16_t shape;
        runs_take(&it, 1);
        t = text_cell(t, tend, g, &cp, &shape);
        int w = (shape & CELL_WIDE) ? 2 : 1;
        if (n + w <= cap) {
            put_cell(&out[n], cp, shape, &it.pen);
            if (w == 2)
                put_cell(&out[n + 1], 0, (uint16_t)((shape & ~CELL_WIDE) | CELL_TRAIL), &it.pen);
        } else if (n < cap) {
            put_cell(&out[n], cp, shape, &it.pen);
        }
        n += w;
    }
    return n < cap ? n : cap;
}

void wsap_vtcore_hist_text_mark_links(const struct text_row *row, void (*mark)(void *ud, uint16_t link), void *ud)
{
    if (!(row->flags & HLINE_HAS_LINK) && !row->start.link)
        return;
    struct runs_iter it;
    runs_iter_runs(&it, &row->runs, &row->start);
    struct pen_attr pen = row->start;
    uint32_t count;
    if (pen.link)
        mark(ud, pen.link);
    while (it.p < it.end) {
        it.p = read_run(it.p, &pen, &count);
        if (pen.link)
            mark(ud, pen.link);
    }
    if (it.pending && it.pending_pen.link)
        mark(ud, it.pending_pen.link);
}

void wsap_vtcore_hist_mark_links(const struct hist *h, void (*mark)(void *ud, uint16_t link), void *ud)
{
    for (uint64_t s = h->tail; s < h->head; s++) {
        const struct hline *p = part(h, s);
        if (!(p->flags & HLINE_HAS_LINK))
            continue;
        struct runs_iter it;
        runs_iter_init(&it, h, p);
        struct pen_attr pen = pen_attr_default();
        uint32_t count;
        while (it.p < it.end) {
            it.p = read_run(it.p, &pen, &count);
            if (pen.link)
                mark(ud, pen.link);
        }
        if (it.pending && it.pending_pen.link)
            mark(ud, it.pending_pen.link);
    }
}

static void drop_head_part(struct hist *h)
{
    struct hline *p = part(h, --h->head);
    if (p->off != NO_TEXT) {
        uint32_t pg = p->off >> 16;
        uint32_t start = p->off & 0xffff;
        uint32_t stop = start + p->text_len + ((p->flags & HLINE_OPEN) ? 0 : p->attr_len);
        if (pg == h->cur_page && h->page_used[pg] == stop)
            h->page_used[pg] = start;
        page_drop_ref(h, pg);
    }
    cont_trim(h);
}

int wsap_vtcore_hist_peek_last_line(const struct hist *h, struct cell *out, int cap, struct gstore *g)
{
    if (h->lines == 0)
        return 0;
    uint64_t id = hist_end_id(h) - 1;
    return decode_line(h, line_first(h, id), h->head, out, cap, g);
}

int wsap_vtcore_hist_pop_line(struct hist *h, struct cell *out, int cap, struct gstore *g)
{
    if (h->lines == 0)
        return 0;
    uint64_t id = hist_end_id(h) - 1;
    uint64_t first = line_first(h, id);
    int n = out ? decode_line(h, first, h->head, out, cap, g) : (int)wsap_vtcore_hist_line_cells(h, id);
    if (id < h->rows_id) {
        rows_uncount(h, id);
        h->rows_id = id;
    }
    while (h->head > first)
        drop_head_part(h);
    h->lines--;
    h->index_epoch++;
    if (h->lines) {
        uint64_t last = hist_end_id(h) - 1;
        h->last_cells = wsap_vtcore_hist_line_cells(h, last);
        h->last_wide = wsap_vtcore_hist_line_wide(h, last);
        h->last_id = last;
    }
    runs_reset(&h->runs);
    h->walk_on = false;
    if (h->lines == 0 && h->head == h->tail) {
        h->all_blank = true;
        h->base_cell = 0;
        h->last_cells = h->last_wide = 0;
    }
    return n;
}

int wsap_vtcore_hist_pop_tail_cells(struct hist *h, int n, struct cell *out, struct gstore *g)
{
    if (h->lines == 0 || n <= 0)
        return 0;
    uint64_t total = wsap_vtcore_hist_line_cells(h, hist_end_id(h) - 1);
    if (total == 0) {
        wsap_vtcore_hist_pop_line(h, NULL, 0, g);
        return 0;
    }
    struct cell *all = malloc((size_t)total * sizeof(*all));
    if (!all)
        return 0;
    if (g)
        wsap_vtcore_gstore_pin(g);
    int got = wsap_vtcore_hist_pop_line(h, all, (int)total, g);
    int take = n < got ? n : got;
    int keep = got - take;
    if (keep > 0 && (all[keep].flags & CELL_TRAIL)) {
        keep++;
        take--;
    }
    if (out && take > 0)
        memcpy(out, all + keep, (size_t)take * sizeof(*out));
    if (keep > 0) {
        uint16_t cols = h->wrap_cols ? h->wrap_cols : 1;
        for (int i = 0; i < keep; i += cols)
            wsap_vtcore_hist_push_row(h, all + i, keep - i < cols ? keep - i : cols, true, g);
    }
    if (g)
        wsap_vtcore_gstore_unpin(g);
    free(all);
    return take;
}

uint64_t wsap_vtcore_hist_total_cells(const struct hist *h)
{
    if (!h || h->tail == h->head || !h->ring)
        return 0;
    uint64_t total = 0;
    uint32_t mask = h->ring_mask;
    const struct hline *ring = h->ring;
    for (uint64_t s = h->tail; s < h->head; s++)
        total += ring[s & mask].cells;
    return total;
}

void wsap_vtcore_hist_set_width(struct hist *h, int cols)
{
    if (h->wrap_cols == cols)
        return;
    h->wrap_cols = (uint16_t)cols;
    h->rows_id = h->base_id;
    h->counted_rows = 0;
    h->walk_on = false;
}

uint64_t wsap_vtcore_hist_line_estimate(const struct hist *h, uint64_t id, int cols)
{
    if (!line_valid(h, id))
        return 0;
    if (cols == h->wrap_cols && id < h->rows_id) {
        const struct hline *first = part(h, line_first(h, id));
        if (first->flags & HLINE_ROWS_EXACT)
            return first->wrap_rows != ROWS_UNKNOWN ? first->wrap_rows :
                   h->exact_rows[line_first(h, id) & h->ring_mask];
    }
    uint64_t cells = wsap_vtcore_hist_line_cells(h, id);
    if (cols == 1)
        cells -= wsap_vtcore_hist_line_wide(h, id);
    return rows_narrow(cells, cols);
}

void wsap_vtcore_hist_refine_rows(struct hist *h, uint64_t id, int cols, int rows)
{
    if (cols != h->wrap_cols || id >= h->rows_id || id < h->base_id)
        return;
    uint64_t seq = line_first(h, id);
    uint64_t old = wsap_vtcore_hist_line_estimate(h, id, cols);
    if (rows >= ROWS_UNKNOWN) {
        if (!h->exact_rows) {
            h->exact_rows = calloc((size_t)h->ring_mask + 1, sizeof(*h->exact_rows));
            if (!h->exact_rows)
                return;
        }
        h->exact_rows[seq & h->ring_mask] = (uint32_t)rows;
    }
    struct hline *first = part(h, seq);
    first->flags |= HLINE_ROWS_EXACT;
    first->wrap_rows = row_count_byte((uint64_t)rows);
    h->counted_rows = h->counted_rows - old + (uint64_t)rows;
}

void wsap_vtcore_hist_prepare_rows(struct hist *h, int cols)
{
    if (cols != h->wrap_cols)
        wsap_vtcore_hist_set_width(h, cols);
    uint64_t stop = hist_end_id(h) - (hist_is_open(h) ? 1 : 0);
    for (; h->rows_id < stop; h->rows_id++) {
        uint64_t rows = wsap_vtcore_hist_line_estimate(h, h->rows_id, cols);
        struct hline *first = part(h, line_first(h, h->rows_id));
        first->wrap_rows = row_count_byte(rows);
        first->flags &= (uint8_t)~HLINE_ROWS_EXACT;
        if (rows < ROWS_UNKNOWN &&
            (!(wsap_vtcore_hist_line_flags(h, h->rows_id) & HLINE_HAS_WIDE) || cols == 1))
            first->flags |= HLINE_ROWS_EXACT;
        h->counted_rows += rows;
    }
}

uint64_t wsap_vtcore_hist_estimated_rows(const struct hist *h, int cols)
{
    if (!h || cols <= 0 || !h->lines)
        return 0;
    uint64_t total = cols == h->wrap_cols ? h->counted_rows : 0;
    uint64_t first = cols == h->wrap_cols ? h->rows_id : h->base_id;
    for (uint64_t id = first; id < hist_end_id(h); id++)
        total += wsap_vtcore_hist_line_estimate(h, id, cols);
    return total;
}

uint64_t wsap_vtcore_hist_rows(const struct hist *h, int cols)
{
    if (!h || cols <= 0)
        return 0;
    uint64_t total = 0;
    for (uint64_t id = hist_first_id(h); id < hist_end_id(h); id++)
        total += (uint64_t)wsap_vtcore_hist_line_rows(h, id, cols);
    return total;
}

uint64_t wsap_vtcore_hist_segment_cell(const struct hist *h, uint64_t id, int seg, int cols)
{
    if (!line_valid(h, id))
        return 0;
    uint64_t base = id == h->base_cell_id ? h->base_cell : 0;
    if (seg <= 0)
        return base;
    if (!(wsap_vtcore_hist_line_flags(h, id) & (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER)))
        return base + (uint64_t)seg * cols;
    uint64_t first = line_first(h, id), end = line_end(h, first), cells = 0;
    int row = 0, col = 0;
    for (uint64_t seq = first; seq < end; seq++) {
        const struct hline *p = part(h, seq);
        if (!p->text_len)
            continue;
        const uint8_t *t = part_text(h, p), *stop = t + p->text_len;
        while (t < stop) {
            int width = text_width(t) == 2 && cols > 1 ? 2 : 1;
            if (col == cols || col + width > cols) {
                if (++row == seg)
                    return base + cells;
                col = 0;
            }
            cells += width;
            col += width;
            t = text_skip(t, stop);
        }
    }
    return base + cells;
}

int wsap_vtcore_hist_cell_segment(const struct hist *h, uint64_t id, uint64_t cell, int cols)
{
    if (!line_valid(h, id) || cols <= 0)
        return 0;
    uint64_t base = id == h->base_cell_id ? h->base_cell : 0;
    cell = cell > base ? cell - base : 0;
    if (!(wsap_vtcore_hist_line_flags(h, id) & (HLINE_HAS_WIDE | HLINE_HAS_CLUSTER))) {
        uint64_t cells = wsap_vtcore_hist_line_cells(h, id);
        if (cells && cell >= cells)
            cell = cells - 1;
        return (int)(cell / cols);
    }
    uint64_t first = line_first(h, id), end = line_end(h, first), cells = 0;
    int row = 0, col = 0;
    for (uint64_t seq = first; seq < end; seq++) {
        const struct hline *p = part(h, seq);
        if (!p->text_len)
            continue;
        const uint8_t *t = part_text(h, p), *stop = t + p->text_len;
        while (t < stop) {
            int width = text_width(t) == 2 && cols > 1 ? 2 : 1;
            if (col == cols || col + width > cols) {
                row++;
                col = 0;
            }
            if (cells + (uint64_t)width > cell)
                return row;
            cells += width;
            col += width;
            t = text_skip(t, stop);
        }
    }
    return row;
}

bool wsap_vtcore_hist_init(struct hist *h, int line_limit, size_t byte_cap)
{
    memset(h, 0, sizeof(*h));
    h->all_blank = true;
    runs_reset(&h->runs);
    if (line_limit <= 0)
        return true;
    if (byte_cap == 0) {
        size_t scaled = (size_t)line_limit * 800;
        byte_cap = scaled > 8u * 1024 * 1024 ? scaled : 8u * 1024 * 1024;
    }
    h->line_limit = (uint32_t)line_limit;
    h->max_pages = (uint32_t)(byte_cap / HIST_PAGE_SIZE);
    if (h->max_pages == 0)
        h->max_pages = 1;
    if (h->max_pages > 65535)
        h->max_pages = 65535;
    h->pages = calloc(h->max_pages, sizeof(*h->pages));
    h->page_used = calloc(h->max_pages, sizeof(*h->page_used));
    h->page_parts = calloc(h->max_pages, sizeof(*h->page_parts));
    h->free_pages = malloc(h->max_pages * sizeof(*h->free_pages));
    if (!h->pages || !h->page_used || !h->page_parts || !h->free_pages || !ring_fit(h, 1)) {
        wsap_vtcore_hist_free(h);
        return false;
    }
    return true;
}

void wsap_vtcore_hist_free(struct hist *h)
{
    if (h->pages)
        for (uint32_t i = 0; i < h->npages; i++)
            wsap_vtcore_aligned_free(h->pages[i]);
    free(h->pages);
    free(h->page_used);
    free(h->page_parts);
    free(h->free_pages);
    wsap_vtcore_aligned_free(h->ring);
    free(h->blocks);
    free(h->exact_rows);
    free(h->cont);
    free(h->runs.data);
    memset(h, 0, sizeof(*h));
}

void wsap_vtcore_hist_clear(struct hist *h)
{
    h->index_epoch++;
    uint64_t end = hist_end_id(h);
    h->tail = h->head = 0;
    h->base_id = end;
    h->base_cell = 0;
    h->last_cells = h->last_wide = 0;
    h->lines = 0;
    h->cont_count = 0;
    h->nfree = 0;
    for (uint32_t pg = 0; pg < h->npages; pg++) {
        h->page_used[pg] = 0;
        h->page_parts[pg] = 0;
        if (pg != h->cur_page)
            h->free_pages[h->nfree++] = (uint16_t)pg;
    }
    h->all_blank = true;
    h->rows_id = end;
    h->counted_rows = 0;
    h->walk_on = false;
    runs_reset(&h->runs);
}
