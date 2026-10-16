#include <assert.h>
#include <stdalign.h>
#include "text_run.h"
#include "compat.h"
#include "kernels.h"
#include "simd.h"
#include "unicode/utf8.h"

static uint8_t unicode_fast_class[65536];
static int8_t unicode_width_bmp[65536];

/* Runs of code points with one fast class, for the AVX-512 width cache. */
#define WSAP_VTCORE_WIDTH_RUNS_MAX 1024
static uint16_t width_run_lo[WSAP_VTCORE_WIDTH_RUNS_MAX];
static uint16_t width_run_hi[WSAP_VTCORE_WIDTH_RUNS_MAX];
static uint8_t width_run_class[WSAP_VTCORE_WIDTH_RUNS_MAX];
static int width_run_count;

void wsap_vtcore_text_tables_init(void)
{
    for (uint32_t cp = 0; cp < 65536; cp++) {
        int w = wsap_vtcore_unicode_width(cp);
        unicode_fast_class[cp] = (w == 1 || w == 2) && !cluster_contextual(cp)
            ? (uint8_t)w : 0;
        unicode_width_bmp[cp] = (int8_t)w;
    }
    for (uint32_t cp = 0; cp < 65536; ) {
        uint8_t k = unicode_fast_class[cp];
        uint32_t lo = cp;
        while (cp < 65536 && unicode_fast_class[cp] == k) cp++;
        if (width_run_count == WSAP_VTCORE_WIDTH_RUNS_MAX) {
            width_run_count = 0;
            break;
        }
        width_run_lo[width_run_count] = (uint16_t)lo;
        width_run_hi[width_run_count] = (uint16_t)(cp - 1);
        width_run_class[width_run_count] = k;
        width_run_count++;
    }
}

static WSAP_VTCORE_MAYBE_UNUSED int width_run_find(uint32_t cp)
{
    int lo = 0, hi = width_run_count - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (width_run_lo[mid] > cp) hi = mid - 1;
        else lo = mid;
    }
    return width_run_count > 0 ? lo : -1;
}

static WSAP_VTCORE_MAYBE_UNUSED uint16_t width_cache_covers(const struct width_cache *wc,
                                                     const uint32_t *cp)
{
    uint16_t covered = 0;
    for (int l = 0; l < 16; l++) {
        bool hit = false;
        for (int k = 0; k < WSAP_VTCORE_WIDTH_SLOTS; k++)
            hit |= cp[l] - wc->lo[k] <= wc->span[k];
        covered |= (uint16_t)(hit << l);
    }
    return covered;
}

static WSAP_VTCORE_MAYBE_UNUSED bool width_cache_extend(struct width_cache *wc, const uint32_t *cp,
                                                 uint16_t covered)
{
    unsigned miss = (unsigned)~covered & 0xFFFFu;
    int r = width_run_find(cp[miss ? wsap_vtcore_ctz32(miss) : 0]);
    if (r < 0) return false;
    uint8_t k = width_run_class[r];
    if (k != 1 && k != 2) return false;
    if (k != wc->klass) {
        memset(wc->lo, 0xFF, sizeof(wc->lo));
        memset(wc->span, 0, sizeof(wc->span));
        wc->klass = k;
        wc->next = 0;
    }
    for (int added = 0; added < WSAP_VTCORE_WIDTH_SLOTS; added++) {
        wc->lo[wc->next] = width_run_lo[r];
        wc->span[wc->next] = (uint32_t)(width_run_hi[r] - width_run_lo[r]);
        wc->next = (uint8_t)((wc->next + 1) % WSAP_VTCORE_WIDTH_SLOTS);
        covered = width_cache_covers(wc, cp);
        if (covered == 0xFFFF) return true;
        r = width_run_find(cp[wsap_vtcore_ctz32((unsigned)~covered & 0xFFFFu)]);
        if (r < 0 || width_run_class[r] != k) return false;
    }
    return false;
}

int wsap_vtcore_text_class(uint32_t cp)
{
    assert(wsap_vtcore_tables_ready());
    return WSAP_VTCORE_EXPECT(cp < 65536, 1) ? (int)unicode_width_bmp[cp] : wsap_vtcore_unicode_width(cp);
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)

static const uint8_t utf8_3byte_shuffle[64] = {
     2,  1,  0, 48,   5,  4,  3, 48,   8,  7,  6, 48,  11, 10,  9, 48,
    14, 13, 12, 48,  17, 16, 15, 48,  20, 19, 18, 48,  23, 22, 21, 48,
    26, 25, 24, 48,  29, 28, 27, 48,  32, 31, 30, 48,  35, 34, 33, 48,
    38, 37, 36, 48,  41, 40, 39, 48,  44, 43, 42, 48,  47, 46, 45, 48,
};

static const uint8_t utf8_3byte_bits[64] = {
    0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0,
    0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0,
    0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0,
    0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0,
    0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0, 0xF0, 0xC0, 0xC0,
    0xF0, 0xC0, 0xC0, 0xF0,
};
static const uint8_t utf8_3byte_want[64] = {
    0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80,
    0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80,
    0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80,
    0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80,
    0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80, 0xE0, 0x80, 0x80,
    0xE0, 0x80, 0x80, 0xE0,
};
#define UTF8_3BYTE_SPAN 0x0000FFFFFFFFFFFFull

WSAP_VTCORE_TARGET_AVX512
static inline __m512i utf8_3byte_decode(__m512i packed)
{
    __m512i b2 = _mm512_and_si512(packed, _mm512_set1_epi32(0x003F));
    __m512i b1 = _mm512_and_si512(_mm512_srli_epi32(packed, 2), _mm512_set1_epi32(0x0FC0));
    __m512i b0 = _mm512_and_si512(_mm512_srli_epi32(packed, 4), _mm512_set1_epi32(0xF000));
    return _mm512_ternarylogic_epi32(b2, b1, b0, 0xFE);
}

struct width_ranges {
    __m512i lo[WSAP_VTCORE_WIDTH_SLOTS];
    __m512i span[WSAP_VTCORE_WIDTH_SLOTS];
};

WSAP_VTCORE_TARGET_AVX512
static inline struct width_ranges width_ranges_load(const struct width_cache *wc)
{
    struct width_ranges r;
    for (int k = 0; k < WSAP_VTCORE_WIDTH_SLOTS; k++) {
        r.lo[k] = _mm512_set1_epi32((int)wc->lo[k]);
        r.span[k] = _mm512_set1_epi32((int)wc->span[k]);
    }
    return r;
}

WSAP_VTCORE_TARGET_AVX512
static inline __mmask16 width_ranges_cover(const struct width_ranges *r, __m512i cps)
{
    __mmask16 covered = 0;
    for (int k = 0; k < WSAP_VTCORE_WIDTH_SLOTS; k++)
        covered |= _mm512_cmple_epu32_mask(_mm512_sub_epi32(cps, r->lo[k]), r->span[k]);
    return covered;
}

WSAP_VTCORE_TARGET_AVX512
static inline void store_wide_cells(uint64_t * restrict dst, __m512i base, __m512i cps)
{
#define WSAP_VTCORE_WIDE_PAIR(k) _mm512_storeu_si512((void *)(dst + 8 * (k)), \
    _mm512_mask_permutexvar_epi32(base, 0x0101, \
        _mm512_setr_epi32((k) * 2, 0, 0, 0, 0, 0, 0, 0, \
                          (k) * 2 + 1, 0, 0, 0, 0, 0, 0, 0), cps))
    WSAP_VTCORE_WIDE_PAIR(0); WSAP_VTCORE_WIDE_PAIR(1); WSAP_VTCORE_WIDE_PAIR(2); WSAP_VTCORE_WIDE_PAIR(3);
    WSAP_VTCORE_WIDE_PAIR(4); WSAP_VTCORE_WIDE_PAIR(5); WSAP_VTCORE_WIDE_PAIR(6); WSAP_VTCORE_WIDE_PAIR(7);
#undef WSAP_VTCORE_WIDE_PAIR
}

WSAP_VTCORE_TARGET_AVX512
static inline void store_narrow_cells(uint64_t * restrict dst, __m512i base, __m512i cps)
{
#define WSAP_VTCORE_NARROW_QUAD(k) _mm512_storeu_si512((void *)(dst + 8 * (k)), \
    _mm512_mask_permutexvar_epi32(base, 0x1111, \
        _mm512_setr_epi32((k) * 4, 0, 0, 0, (k) * 4 + 1, 0, 0, 0, \
                          (k) * 4 + 2, 0, 0, 0, (k) * 4 + 3, 0, 0, 0), cps))
    WSAP_VTCORE_NARROW_QUAD(0); WSAP_VTCORE_NARROW_QUAD(1); WSAP_VTCORE_NARROW_QUAD(2); WSAP_VTCORE_NARROW_QUAD(3);
#undef WSAP_VTCORE_NARROW_QUAD
}

/* Both cell and history writers validate and classify the same UTF-8 block. */
WSAP_VTCORE_TARGET_AVX512
static inline bool classify_3byte_block(__m512i in, struct width_ranges *ranges,
                                       struct width_cache *wc, __m512i *cps)
{
    if (WSAP_VTCORE_EXPECT(_mm512_test_epi8_mask(
            _mm512_xor_si512(in, _mm512_loadu_si512(utf8_3byte_want)),
            _mm512_loadu_si512(utf8_3byte_bits)) & UTF8_3BYTE_SPAN, 0))
        return false;
    *cps = utf8_3byte_decode(_mm512_permutexvar_epi8(_mm512_loadu_si512(utf8_3byte_shuffle), in));
    if (_mm512_cmplt_epu32_mask(*cps, _mm512_set1_epi32((int)WSAP_VTCORE_UTF8_3BYTE_MIN)) ||
        _mm512_cmple_epu32_mask(
            _mm512_sub_epi32(*cps, _mm512_set1_epi32((int)WSAP_VTCORE_UTF8_SURROGATE_LO)),
            _mm512_set1_epi32((int)(WSAP_VTCORE_UTF8_SURROGATE_HI - WSAP_VTCORE_UTF8_SURROGATE_LO))))
        return false;
    __mmask16 covered = width_ranges_cover(ranges, *cps);
    if (WSAP_VTCORE_EXPECT(covered != 0xFFFF || wc->klass == 0, 0)) {
        uint32_t cp[16];
        _mm512_storeu_si512(cp, *cps);
        if (!width_cache_extend(wc, cp, covered)) return false;
        *ranges = width_ranges_load(wc);
    }
    return true;
}

WSAP_VTCORE_TARGET_AVX512
static WSAP_VTCORE_NOINLINE int
decode_3byte_blocks(uint64_t * restrict dst, const char * restrict buf, int len, int cols_left,
                   __m512i base_wide_pair, __m512i base_narrow_quad,
                   struct width_cache * restrict wc,
                   int * restrict out_cols, uint32_t * restrict last_cp)
{
    struct width_ranges ranges = width_ranges_load(wc);
    uint32_t last = *last_cp;
    int i = 0, cx = 0;

    while (i + 48 <= len && cx + 32 <= cols_left &&
           ((unsigned char)buf[i + 45] & UTF8_LEAD3_MASK) == UTF8_LEAD3_TAG) {
        __m512i in = _mm512_maskz_loadu_epi8(UTF8_3BYTE_SPAN, buf + i);
        __m512i cps;
        if (!classify_3byte_block(in, &ranges, wc, &cps)) break;

        if (wc->klass == 2) {
            store_wide_cells(dst, base_wide_pair, cps);
            dst += 64;
            cx += 32;
        } else {
            store_narrow_cells(dst, base_narrow_quad, cps);
            dst += 32;
            cx += 16;
        }
        last = (uint32_t)_mm_extract_epi32(_mm512_extracti32x4_epi32(cps, 3), 3);
        i += 48;
    }

    *out_cols = cx;
    *last_cp = last;
    return i;
}

/* decode_3byte_blocks() that writes the history format: a narrow block
 * copies its 48 bytes, a wide block puts 0xF5 before each character. */
WSAP_VTCORE_TARGET_AVX512
static WSAP_VTCORE_NOINLINE int
text_3byte_blocks(uint8_t * restrict dst, const char * restrict buf, int len, int cols_left,
                  struct width_cache * restrict wc, int * restrict out_cols,
                  int * restrict out_bytes, bool * restrict wide, uint32_t * restrict last_cp)
{
    static const uint8_t spread[64] = {
         0,  0,  1,  2,   0,  3,  4,  5,   0,  6,  7,  8,   0,  9, 10, 11,
         0, 12, 13, 14,   0, 15, 16, 17,   0, 18, 19, 20,   0, 21, 22, 23,
         0, 24, 25, 26,   0, 27, 28, 29,   0, 30, 31, 32,   0, 33, 34, 35,
         0, 36, 37, 38,   0, 39, 40, 41,   0, 42, 43, 44,   0, 45, 46, 47,
    };
    struct width_ranges ranges = width_ranges_load(wc);
    __m512i idx = _mm512_loadu_si512(spread);
    __m512i prefix = _mm512_set1_epi8((char)0xF5);
    uint32_t last = *last_cp;
    int i = 0, cx = 0, o = 0;

    while (i + 48 <= len && cx + 32 <= cols_left &&
           ((unsigned char)buf[i + 45] & UTF8_LEAD3_MASK) == UTF8_LEAD3_TAG) {
        __m512i in = _mm512_maskz_loadu_epi8(UTF8_3BYTE_SPAN, buf + i);
        __m512i cps;
        if (!classify_3byte_block(in, &ranges, wc, &cps)) break;
        if (wc->klass == 2) {
            __m512i out = _mm512_mask_mov_epi8(_mm512_permutexvar_epi8(idx, in),
                                               0x1111111111111111ull, prefix);
            _mm512_storeu_si512((void *)(dst + o), out);
            o += 64;
            cx += 32;
            *wide = true;
        } else {
            _mm512_mask_storeu_epi8(dst + o, UTF8_3BYTE_SPAN, in);
            o += 48;
            cx += 16;
        }
        last = (uint32_t)_mm_extract_epi32(_mm512_extracti32x4_epi32(cps, 3), 3);
        i += 48;
    }
    *out_cols = cx;
    *out_bytes = o;
    *last_cp = last;
    return i;
}
#endif

struct text_run_bytes wsap_vtcore_text_run_bytes(uint8_t *restrict dst, const char *restrict buf, int len,
                                          int max_cols, uint32_t last, struct width_cache *restrict wc)
{
    if (WSAP_VTCORE_EXPECT(last == 0x200D || wsap_vtcore_grapheme_is_handle(last), 0))
        return (struct text_run_bytes){ .last = last, .narrow = true };
    /* Unlike wsap_vtcore_text_run(), this fills up to the edge and up to the end of
     * buf: the cells path writes those characters one by one with the same
     * result, and a text row should not settle for them. */
    int i = 0, cx = 0, o = 0, chars = 0;
    bool wide = false, narrow = true;
    (void)wc;
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    bool blocks = wsap_vtcore_use_avx512();
#endif
    while (i < len && cx < max_cols) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 0x20 && c < 0x7f) {
            int room = max_cols - cx < len - i ? max_cols - cx : len - i;
            int n = (int)scan_printable_span(buf + i, (size_t)room, 0x7f);
            if (!n) n = 1;
            bytes_copy_small(dst + o, buf + i, n);
            i += n;
            o += n;
            cx += n;
            chars += n;
            last = (unsigned char)buf[i - 1];
            continue;
        }
        if (c < 0x20)
            break;
#if defined(WSAP_VTCORE_KERNEL_AVX512)
        if ((c & UTF8_LEAD3_MASK) == UTF8_LEAD3_TAG && len - i >= 48 && max_cols - cx >= 32 &&
            ((unsigned char)buf[i + 21] & UTF8_LEAD3_MASK) == UTF8_LEAD3_TAG && blocks) {
            int cols, bytes;
            bool block_wide = false;
            int used = text_3byte_blocks(dst + o, buf + i, len - i, max_cols - cx, wc,
                                         &cols, &bytes, &block_wide, &last);
            if (used) {
                i += used;
                cx += cols;
                o += bytes;
                chars += used / 3;
                wide |= block_wide;
                narrow = false;
                continue;
            }
        }
#endif
        uint32_t cp;
        int bytes = wsap_vtcore_utf8_decode_one(buf + i, (size_t)(len - i), &cp);
        if (!bytes)
            break;
        int w = bytes < 4 ? unicode_fast_class[cp] : wsap_vtcore_text_class(cp);
        if (w <= 0 || (bytes == 4 && can_extend_cluster(last, cp, w)) || cx + w > max_cols)
            break;
        if (w == 2) {
            dst[o++] = 0xF5;
            wide = true;
        }
        bytes_copy_small(dst + o, buf + i, bytes);
        o += bytes;
        cx += w;
        chars++;
        narrow = false;
        last = cp;
        i += bytes;
    }
    return (struct text_run_bytes){ i, cx, o, chars, last, wide, narrow };
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline __m512i wide_pair_base(uint32_t fg, uint32_t bg, uint32_t flags)
{
    return _mm512_setr_epi32(
        0, fg, bg, flags | CELL_WIDE,
        0, fg, bg, (flags & ~CELL_WIDE) | CELL_TRAIL,
        0, fg, bg, flags | CELL_WIDE,
        0, fg, bg, (flags & ~CELL_WIDE) | CELL_TRAIL);
}

WSAP_VTCORE_TARGET_AVX512
static inline int try_3byte_blocks(uint64_t * restrict dst, const char * restrict buf,
                                  int len, int cols_left, uint32_t fg, uint32_t bg,
                                  uint32_t flags, struct width_cache * restrict wc,
                                  int * restrict out_cols, uint32_t * restrict last_cp)
{
    *out_cols = 0;
    if (len < 48 || cols_left < 32) return 0;
    if (((unsigned char)buf[0] & UTF8_LEAD3_MASK) != UTF8_LEAD3_TAG) return 0;
    if (((unsigned char)buf[21] & UTF8_LEAD3_MASK) != UTF8_LEAD3_TAG) return 0;
    if (((unsigned char)buf[45] & UTF8_LEAD3_MASK) != UTF8_LEAD3_TAG) return 0;
    __m512i base_narrow_quad = _mm512_broadcast_i32x4(_mm_set_epi32((int)flags, (int)bg, (int)fg, 0));
    return decode_3byte_blocks(dst, buf, len, cols_left, wide_pair_base(fg, bg, flags),
                              base_narrow_quad, wc, out_cols, last_cp);
}

/* Two wide cells side by side: cp and next_cp. */
WSAP_VTCORE_TARGET_AVX512
static inline void store_wide_pair(uint64_t *dst, uint32_t fg, uint32_t bg, uint32_t flags,
                                   uint32_t cp, uint32_t next_cp)
{
    __m512i cells = _mm512_mask_set1_epi32(wide_pair_base(fg, bg, flags), 0x0001, (int)cp);
    cells = _mm512_mask_set1_epi32(cells, 0x0100, (int)next_cp);
    _mm512_storeu_si512((void *)dst, cells);
}
#endif

struct cell_bases {
#if defined(__x86_64__) || defined(_M_X64)
    __m128i cell, wide, trail;
#elif defined(__aarch64__)
    uint32x4_t cell, wide, trail;
#else
    uint64_t fg_shifted, high, high_wide, high_trail;
#endif
};

static inline struct cell_bases cell_bases_make(uint32_t fg, uint32_t bg, uint32_t flags)
{
    struct cell_bases b;
#if defined(__x86_64__) || defined(_M_X64)
    b.cell = _mm_set_epi32(flags, bg, fg, 0);
    b.wide = _mm_set_epi32(flags | CELL_WIDE, bg, fg, 0);
    b.trail = _mm_set_epi32((flags & ~CELL_WIDE) | CELL_TRAIL, bg, fg, 0);
#elif defined(__aarch64__)
    b.cell = (uint32x4_t){ 0, fg, bg, (uint32_t)flags };
    b.wide = (uint32x4_t){ 0, fg, bg, (uint32_t)(flags | CELL_WIDE) };
    b.trail = (uint32x4_t){ 0, fg, bg,
        (uint32_t)((flags & ~CELL_WIDE) | CELL_TRAIL) };
#else
    b.fg_shifted = (uint64_t)fg << 32;
    b.high = ((uint64_t)flags << 32) | bg;
    b.high_wide = ((uint64_t)(flags | CELL_WIDE) << 32) | bg;
    b.high_trail = ((uint64_t)((flags & ~CELL_WIDE) | CELL_TRAIL) << 32) | bg;
#endif
    return b;
}

static inline uint64_t *store_cell(uint64_t * restrict dst,
                                   const struct cell_bases *b, uint32_t cp)
{
#if defined(__x86_64__) || defined(_M_X64)
    _mm_storeu_si128((__m128i *)dst, _mm_or_si128(b->cell, _mm_cvtsi32_si128(cp)));
#elif defined(__aarch64__)
    vst1q_u32((uint32_t *)(void *)dst, vsetq_lane_u32(cp, b->cell, 0));
#else
    dst[0] = b->fg_shifted | cp;
    dst[1] = b->high;
#endif
    return dst + 2;
}

static inline uint64_t *store_cell_wide(uint64_t * restrict dst,
                                        const struct cell_bases *b, uint32_t cp)
{
#if defined(__x86_64__) || defined(_M_X64)
    _mm_storeu_si128((__m128i *)dst, _mm_or_si128(b->wide, _mm_cvtsi32_si128(cp)));
    _mm_storeu_si128((__m128i *)(dst + 2), b->trail);
#elif defined(__aarch64__)
    vst1q_u32((uint32_t *)(void *)dst, vsetq_lane_u32(cp, b->wide, 0));
    vst1q_u32((uint32_t *)(void *)(dst + 2), b->trail);
#else
    dst[0] = b->fg_shifted | cp;
    dst[1] = b->high_wide;
    dst[2] = b->fg_shifted;
    dst[3] = b->high_trail;
#endif
    return dst + 4;
}

#if defined(WSAP_VTCORE_KERNEL_AVX2)
/*
 * Decode runs of 3-byte UTF-8 four code points at a time. A quad stops the
 * run when a byte is not a 3-byte sequence, a code point is overlong or a
 * surrogate, or a code point has no fast width class. The scalar loop then
 * handles that code point, so both paths store the same cells.
 */
WSAP_VTCORE_TARGET_AVX2
static inline int decode_3byte_quads(uint64_t **dst_io, const char *buf, int len,
                                     int cols_left, const struct cell_bases *b,
                                     int *out_cols, uint32_t *last)
{
    const __m128i want = _mm_setr_epi8((char)0xE0, (char)0x80, (char)0x80,
                                       (char)0xE0, (char)0x80, (char)0x80,
                                       (char)0xE0, (char)0x80, (char)0x80,
                                       (char)0xE0, (char)0x80, (char)0x80, 0, 0, 0, 0);
    const __m128i bits = _mm_setr_epi8((char)0xF0, (char)0xC0, (char)0xC0,
                                       (char)0xF0, (char)0xC0, (char)0xC0,
                                       (char)0xF0, (char)0xC0, (char)0xC0,
                                       (char)0xF0, (char)0xC0, (char)0xC0, 0, 0, 0, 0);
    const __m128i shuffle = _mm_setr_epi8(2, 1, 0, -1, 5, 4, 3, -1,
                                          8, 7, 6, -1, 11, 10, 9, -1);
    uint64_t *dst = *dst_io;
    int i = 0, cx = 0;
    while (i + 16 <= len && cx + 8 <= cols_left) {
        __m128i in = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i ok = _mm_cmpeq_epi8(_mm_and_si128(_mm_xor_si128(in, want), bits),
                                    _mm_setzero_si128());
        if (_mm_movemask_epi8(ok) != 0xFFFF)
            break;
        __m128i packed = _mm_shuffle_epi8(in, shuffle);
        __m128i cps = _mm_or_si128(
            _mm_or_si128(_mm_and_si128(packed, _mm_set1_epi32(0x003F)),
                         _mm_and_si128(_mm_srli_epi32(packed, 2), _mm_set1_epi32(0x0FC0))),
            _mm_and_si128(_mm_srli_epi32(packed, 4), _mm_set1_epi32(0xF000)));
        __m128i bad = _mm_or_si128(
            _mm_cmplt_epi32(cps, _mm_set1_epi32((int)WSAP_VTCORE_UTF8_3BYTE_MIN)),
            _mm_cmpeq_epi32(_mm_and_si128(cps, _mm_set1_epi32(0xF800)),
                            _mm_set1_epi32((int)WSAP_VTCORE_UTF8_SURROGATE_LO)));
        if (_mm_movemask_epi8(bad))
            break;
        alignas(16) uint32_t cp[4];
        _mm_store_si128((__m128i *)cp, cps);
        uint8_t k0 = unicode_fast_class[cp[0]], k1 = unicode_fast_class[cp[1]];
        uint8_t k2 = unicode_fast_class[cp[2]], k3 = unicode_fast_class[cp[3]];
        if (!k0 || !k1 || !k2 || !k3)
            break;
        if (k0 == 1) { dst = store_cell(dst, b, cp[0]); cx++; }
        else { dst = store_cell_wide(dst, b, cp[0]); cx += 2; }
        if (k1 == 1) { dst = store_cell(dst, b, cp[1]); cx++; }
        else { dst = store_cell_wide(dst, b, cp[1]); cx += 2; }
        if (k2 == 1) { dst = store_cell(dst, b, cp[2]); cx++; }
        else { dst = store_cell_wide(dst, b, cp[2]); cx += 2; }
        if (k3 == 1) { dst = store_cell(dst, b, cp[3]); cx++; }
        else { dst = store_cell_wide(dst, b, cp[3]); cx += 2; }
        *last = cp[3];
        i += 12;
    }
    *dst_io = dst;
    *out_cols = cx;
    return i;
}
#endif

/* One body for every level. isa is a constant in each copy below, so each
 * copy keeps only its own kernels. */
static inline WSAP_VTCORE_ALWAYS_INLINE struct text_run_result
text_run_body(struct cell *restrict row, const char *restrict buf, int len, int max_cols,
              uint32_t fg, uint32_t bg, uint32_t flags, uint32_t last,
              struct width_cache *restrict wc, int isa)
{
    int i = 0;
    int cx = 0;
    if (WSAP_VTCORE_EXPECT(last == 0x200D || wsap_vtcore_grapheme_is_handle(last), 0))
        return (struct text_run_result){0, 0, last};

    const struct cell_bases bases = cell_bases_make(fg, bg, flags);
    uint64_t *dst = (uint64_t *)row;
    (void)wc;
    (void)isa;

#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (isa == WSAP_VTCORE_ISA_AVX512) {
        int cols;
        i += try_3byte_blocks(dst, buf, len, max_cols, fg, bg, flags, wc, &cols, &last);
        cx += cols;
        dst += 2 * cols;
    }
#endif

    while (i + 4 <= len && cx + 2 <= max_cols) {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 0x20 && c < 0x7f) {
            int ascii_at = cx;
            dst = store_cell(dst, &bases, c);
            last = c;
            cx++;
            i++;
            while (i + 4 <= len && cx + 2 <= max_cols) {
                unsigned char next_c = (unsigned char)buf[i];
                if (next_c < 0x20 || next_c >= 0x7f) break;
                dst = store_cell(dst, &bases, next_c);
                last = next_c;
                cx++;
                i++;
            }
#if defined(WSAP_VTCORE_KERNEL_AVX512)
            if (isa == WSAP_VTCORE_ISA_AVX512 && ascii_at == 0) {
                int cols;
                i += try_3byte_blocks(dst, buf + i, len - i, max_cols - cx,
                                     fg, bg, flags, wc, &cols, &last);
                cx += cols;
                dst += 2 * cols;
            }
#endif
            (void)ascii_at;
            continue;
        }
        if (c < 0x20) {
            break;
        }

#if defined(WSAP_VTCORE_KERNEL_AVX2)
        if (isa == WSAP_VTCORE_ISA_AVX2 &&
            WSAP_VTCORE_EXPECT((c & UTF8_LEAD3_MASK) == UTF8_LEAD3_TAG && i + 16 <= len &&
                             ((unsigned char)buf[i + 9] & UTF8_LEAD3_MASK) == UTF8_LEAD3_TAG, 0)) {
            int cols;
            int used = decode_3byte_quads(&dst, buf + i, len - i, max_cols - cx,
                                          &bases, &cols, &last);
            i += used;
            cx += cols;
            if (used)
                continue;
        }
#endif
        uint32_t cp;
        int bytes = wsap_vtcore_utf8_decode_one(buf + i, (size_t)(len - i), &cp);
        if (!bytes) break;
        int w = bytes < 4 ? unicode_fast_class[cp] : wsap_vtcore_text_class(cp);
        if (w <= 0 || (bytes == 4 && can_extend_cluster(last, cp, w))) break;
#if defined(WSAP_VTCORE_KERNEL_AVX512)
        if (isa == WSAP_VTCORE_ISA_AVX512 && bytes == 3 && w == 2 && i + 6 <= len && cx + 4 <= max_cols) {
            uint32_t next_cp;
            if (wsap_vtcore_utf8_decode_one(buf + i + 3, (size_t)(len - i - 3), &next_cp) == 3 &&
                unicode_fast_class[next_cp] == 2) {
                store_wide_pair(dst, fg, bg, flags, cp, next_cp);
                dst += 8;
                last = next_cp;
                cx += 4;
                i += 6;
                continue;
            }
        }
#endif
        if (w == 1) {
            dst = store_cell(dst, &bases, cp);
            cx++;
        } else {
            dst = store_cell_wide(dst, &bases, cp);
            cx += 2;
        }
        last = cp;
        i += bytes;
    }

    return (struct text_run_result){i, cx, last};
}

#define TEXT_RUN_PARAMS struct cell *restrict row, const char *restrict buf, int len, \
    int max_cols, uint32_t fg, uint32_t bg, uint32_t flags, uint32_t last, \
    struct width_cache *restrict wc
#define TEXT_RUN_ARGS row, buf, len, max_cols, fg, bg, flags, last, wc

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static WSAP_VTCORE_NOINLINE struct text_run_result text_run_avx512(TEXT_RUN_PARAMS)
{
    return text_run_body(TEXT_RUN_ARGS, WSAP_VTCORE_ISA_AVX512);
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static WSAP_VTCORE_NOINLINE struct text_run_result text_run_avx2(TEXT_RUN_PARAMS)
{
    return text_run_body(TEXT_RUN_ARGS, WSAP_VTCORE_ISA_AVX2);
}
#endif

static WSAP_VTCORE_NOINLINE struct text_run_result text_run_base(TEXT_RUN_PARAMS)
{
    return text_run_body(TEXT_RUN_ARGS, WSAP_VTCORE_ISA_BASE);
}

struct text_run_result wsap_vtcore_text_run(TEXT_RUN_PARAMS)
{
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return text_run_avx512(TEXT_RUN_ARGS);
#endif
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return text_run_avx2(TEXT_RUN_ARGS);
#endif
    return text_run_base(TEXT_RUN_ARGS);
}
