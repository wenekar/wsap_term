#import <Foundation/Foundation.h>
#include "font/font.h"
#include "font/glyph_cache.h"
#include "font/backend.h"
#include "font/glyph/intern.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static uint64_t glyph_pixels(const struct glyph *g, int rows)
{
    uint64_t hash = 14695981039346656037ULL;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < g->width; x++) {
            uint32_t pixel = g->has_color ? g->color[(size_t)y * g->width + x]
                                         : g->alpha[y][x];
            hash = (hash ^ pixel) * 1099511628211ULL;
        }
    }
    return hash;
}

static bool glyph_has_ink(const struct glyph *g, int rows)
{
    for (int y = 0; y < rows; y++)
        if (glyph_row_inked(g, y)) return true;
    return false;
}

static bool glyph_has_partial_coverage(const struct glyph *g, int rows)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < g->width; x++)
            if (g->alpha[y][x] > 0 && g->alpha[y][x] < 255) return true;
    return false;
}

static void test_coretext_metrics(void)
{
    struct font_instance *fi = font_instance_default();
    assert(fi != NULL);

    const struct font_metrics *m = font_instance_metrics(fi);
    assert(m->cell_w > 0);
    assert(m->cell_h > 0);
    assert(m->baseline > 0);
    assert(m->baseline <= m->cell_h);
    assert(m->pixel_size > 0);

    font_bind_thread(fi);
}

static void test_coretext_glyph_rendering(void)
{
    struct font_instance *fi = font_instance_default();
    struct glyph g = {0};

    /* Basic ASCII rendering */
    assert(font_instance_render_glyph(fi, 'A', 0, &g));
    assert(g.width > 0);
    assert(!g.has_color);
    bool has_ink = false;
    for (int y = 0; y < font_instance_metrics(font_instance_default())->cell_h; y++) {
        if (glyph_row_inked(&g, y)) {
            has_ink = true;
            break;
        }
    }
    assert(has_ink);
    glyph_release(&g);

    /* CJK wide glyph rendering */
    memset(&g, 0, sizeof(g));
    assert(font_instance_render_glyph(fi, 0x4E2D, FONT_STYLE_WIDE, &g));
    assert(g.width >= font_instance_metrics(font_instance_default())->cell_w * 2);
    has_ink = false;
    for (int y = 0; y < font_instance_metrics(font_instance_default())->cell_h; y++) {
        if (glyph_row_inked(&g, y)) {
            has_ink = true;
            break;
        }
    }
    assert(has_ink);
    glyph_release(&g);

    /* Apple Color Emoji rendering */
    memset(&g, 0, sizeof(g));
    assert(font_instance_render_glyph(fi, 0x1F600, FONT_STYLE_WIDE | FONT_STYLE_EMOJI, &g));
    assert(g.has_color);
    assert(g.color != NULL);
    has_ink = false;
    for (int y = 0; y < font_instance_metrics(font_instance_default())->cell_h; y++) {
        if (glyph_row_inked(&g, y)) {
            has_ink = true;
            break;
        }
    }
    assert(has_ink);
    glyph_release(&g);

    /* Procedural box drawing */
    memset(&g, 0, sizeof(g));
    assert(font_instance_render_glyph(fi, 0x2500, 0, &g));
    assert(!g.has_color);
    has_ink = false;
    for (int y = 0; y < font_instance_metrics(font_instance_default())->cell_h; y++) {
        if (glyph_row_inked(&g, y)) {
            has_ink = true;
            break;
        }
    }
    assert(has_ink);
    glyph_release(&g);
}

static void test_coretext_scaling(void)
{
    struct font_instance *base = font_instance_default();
    const struct font_metrics *base_m = font_instance_metrics(base);

    struct font_instance *retina = font_instance_acquire(font_instance_size_pt(base), 240);
    assert(retina != NULL);
    const struct font_metrics *retina_m = font_instance_metrics(retina);

    assert(retina_m->cell_w >= base_m->cell_w * 2 - 2);
    assert(retina_m->cell_h >= base_m->cell_h * 2 - 2);

    font_instance_unref(retina);
    font_bind_thread(base);
}

/* F and the question mark have wider ink at the top. */
static bool upper_ink_wider(const struct glyph *g, int rows)
{
    int top = rows, bottom = -1;
    for (int y = 0; y < rows; y++) {
        if (glyph_row_inked(g, y)) {
            if (top == rows) top = y;
            bottom = y;
        }
    }
    if (bottom <= top)
        return false;
    unsigned upper = 0, lower = 0;
    for (int y = top; y <= bottom; y++) {
        unsigned ink = 0;
        for (int x = 0; x < g->width; x++)
            ink += g->has_color ? g->color[(size_t)y * g->width + x] >> 24
                                : g->alpha[y][x];
        if (2 * y < top + bottom && ink > upper)
            upper = ink;
        else if (2 * y > top + bottom && ink > lower)
            lower = ink;
    }
    return upper > lower;
}

/* What each cell of a segment shows: the shaped glyph of a span, or else
 * the cell's own glyph. A span is valid only during the callback, so the
 * collector keeps what the checks read. */
struct shown_cells {
    int rows, shaped;
    bool span[FONT_RUN_MAX];
    bool ink[FONT_RUN_MAX], partial[FONT_RUN_MAX], upper_wider[FONT_RUN_MAX];
    uint64_t pixels[FONT_RUN_MAX];
};

static void show_cell(struct shown_cells *s, int i, const struct glyph *g)
{
    s->ink[i] = glyph_has_ink(g, s->rows);
    s->partial[i] = glyph_has_partial_coverage(g, s->rows);
    s->upper_wider[i] = upper_ink_wider(g, s->rows);
    s->pixels[i] = glyph_pixels(g, s->rows);
}

static void collect_span(void *ctx, const struct glyph_span *span)
{
    struct shown_cells *s = ctx;
    for (int i = 0; i < span->count; i++) {
        s->span[span->start + i] = true;
        show_cell(s, span->start + i, &span->cells[i]);
    }
    s->shaped += span->count;
}

static struct shown_cells show_segment(struct font_instance *fi, const uint32_t *cps, int n)
{
    struct shown_cells s = { .rows = font_instance_metrics(fi)->cell_h };
    glyph_cache_shape_segment(fi, cps, n, 0, collect_span, &s);
    for (int i = 0; i < n; i++) {
        const struct glyph *g = s.span[i] ? NULL : glyph_cache_fetch(fi, cps[i], 0);
        if (g)
            show_cell(&s, i, g);
    }
    return s;
}

static void test_coretext_glyph_orientation(void)
{
    font_set_family("Menlo");
    for (uint32_t scale = 120; scale <= 240; scale += 120) {
        struct font_instance *fi = font_instance_acquire(20, scale);
        assert(fi);
        font_bind_thread(fi);
        const struct font_metrics *m = font_instance_metrics(fi);
        for (uint16_t style = 0; style <= FONT_STYLE_BOLD; style += FONT_STYLE_BOLD) {
            struct glyph g = {0};
            assert(font_instance_render_glyph(fi, 'F', style, &g));
            assert(upper_ink_wider(&g, m->cell_h));
            glyph_release(&g);
        }

        /* A cell of a segment stands upright, shaped or not. */
        const uint32_t cps[] = { 'F', 0x0301 };
        assert(show_segment(fi, cps, 2).upper_wider[0]);

        struct glyph emoji = {0};
        assert(font_instance_render_glyph(fi, 0x2753, FONT_STYLE_WIDE | FONT_STYLE_EMOJI, &emoji));
        assert(emoji.has_color);
        assert(upper_ink_wider(&emoji, m->cell_h));
        glyph_release(&emoji);
        font_instance_unref(fi);
    }
    font_set_family(FONT_DEFAULT_FAMILY);
    font_bind_thread(font_instance_default());
}

static void test_coretext_hotswap(void)
{
    struct font_instance *fi = font_instance_default();
    uint32_t gen0 = (uint32_t)font_instance_version(fi);

    font_set_family("Courier");
    uint32_t gen1 = (uint32_t)font_generation();
    assert(gen1 > gen0);

    struct font_instance *fi_new = font_instance_default();
    assert(fi_new != NULL);
    assert(strcmp(font_get_family(), "Courier") == 0);

    struct glyph g = {0};
    assert(font_instance_render_glyph(fi_new, 'M', 0, &g));
    assert(g.width > 0);
    glyph_release(&g);

    font_set_family(FONT_DEFAULT_FAMILY);
}

static void test_coretext_clusters(void)
{
    font_set_family("Menlo");
    const uint32_t accent[] = { 'e', 0x0301 };
    const uint32_t tone[] = { 0x1F44D, 0x1F3FF };
    const uint32_t family[] = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 };
    const uint32_t flag[] = { 0x1F1F9, 0x1F1F7 };
    const uint32_t keycap[] = { '1', 0xFE0F, 0x20E3 };
    const uint32_t *sequences[] = { accent, tone, family, flag, keycap };
    const int lengths[] = { 2, 2, 5, 2, 3 };
    for (uint32_t scale = 120; scale <= 240; scale += 120) {
        struct font_instance *fi = font_instance_acquire(20, scale);
        assert(fi);
        font_bind_thread(fi);
        int rows = font_instance_metrics(fi)->cell_h;
        for (int i = 0; i < 5; i++) {
            struct glyph g = {0}, base = {0};
            uint16_t style = i ? FONT_STYLE_WIDE | FONT_STYLE_EMOJI : 0;
            assert(font_instance_render_glyph(fi, glyph_intern(sequences[i], lengths[i]),
                                              style, &g));
            assert(font_instance_render_glyph(fi, sequences[i][0], style, &base));
            assert(glyph_has_ink(&g, rows));
            assert(g.has_color == (i != 0));
            assert(glyph_pixels(&g, rows) != glyph_pixels(&base, rows));
            glyph_release(&g);
            glyph_release(&base);
        }
        font_instance_unref(fi);
    }
    font_set_family(FONT_DEFAULT_FAMILY);
    font_bind_thread(font_instance_default());
}

static void test_coretext_fallback_shaping(void)
{
    font_set_family("Menlo");
    struct font_instance *fi = font_instance_default();
    font_bind_thread(fi);
    int rows = font_instance_metrics(fi)->cell_h;
    const uint32_t cps[] = { 'A', 0x633, 0x644, 0x627, 0x645, 'B' };
    struct shown_cells run = show_segment(fi, cps, 6);
    assert(run.shaped > 0);
    assert(run.ink[0]);
    assert(run.ink[5]);
    bool joined = false;
    for (int i = 1; i < 5; i++) {
        struct glyph isolated = {0};
        assert(font_instance_render_glyph(fi, cps[i], 0, &isolated));
        joined |= run.ink[i] && run.pixels[i] != glyph_pixels(&isolated, rows);
        glyph_release(&isolated);
    }
    assert(joined);
    font_set_family(FONT_DEFAULT_FAMILY);
    font_bind_thread(font_instance_default());
}

static void test_coretext_antialias(void)
{
    font_set_family("Menlo");
    const uint32_t cps[] = { 'F', 0x0301 };
    for (int mode = 0; mode < 3; mode++) {
        font_set_aa_mode(mode == 1 ? FONT_AA_MONO : FONT_AA_GRAYSCALE);
        font_set_hint_style(FONT_HINT_NONE);
        struct font_instance *fi = font_instance_default();
        font_bind_thread(fi);
        int rows = font_instance_metrics(fi)->cell_h;
        const struct glyph *g = glyph_cache_fetch(fi, 'S', 0);
        assert(g && glyph_has_ink(g, rows));
        assert(glyph_has_partial_coverage(g, rows) == (mode != 1));
        struct shown_cells run = show_segment(fi, cps, 2);
        assert(run.ink[0]);
        assert(run.partial[0] == (mode != 1));
        const uint32_t tall[] = { 'F', 0x0301, 0x0302, 0x0303, 0x0304, 0x0305 };
        struct shown_cells tall_run = show_segment(fi, tall, 6);
        bool ink = false, partial = false;
        for (int i = 0; i < 6; i++) {
            ink |= tall_run.ink[i];
            partial |= tall_run.partial[i];
        }
        assert(ink);
        assert(partial == (mode != 1));
    }
    font_set_aa_mode(FONT_AA_AUTO);
    font_set_hint_style(FONT_HINT_AUTO);
    font_set_family(FONT_DEFAULT_FAMILY);
    font_bind_thread(font_instance_default());
}

static void *render_worker(void *arg)
{
    struct font_instance *fi = arg;
    @autoreleasepool {
        font_bind_thread(fi);
        const uint32_t cps[] = { 'F', 0x0301 };
        uint64_t expected = 0;
        for (int pass = 0; pass < 3; pass++) {
            struct shown_cells run = show_segment(fi, cps, 2);
            uint64_t pixels = run.pixels[0];
            assert(run.ink[0]);
            if (pass) assert(pixels == expected);
            expected = pixels;
            font_thread_detach();
            font_thread_detach();
        }
        glyph_cache_thread_detach();
    }
    return NULL;
}

static void test_coretext_worker_lifecycle(void)
{
    struct font_instance *fi = font_instance_default();
    for (int pass = 0; pass < 4; pass++) {
        pthread_t threads[3];
        /* The workers share one instance, so the cache must be in parallel
         * mode. In serial mode a run cache CAS loser frees its cells before
         * emit reads them. */
        glyph_cache_set_parallel(true);
        for (int i = 0; i < 3; i++) assert(!pthread_create(&threads[i], NULL, render_worker, fi));
        for (int i = 0; i < 3; i++) assert(!pthread_join(threads[i], NULL));
        /* Every worker has joined, so no reader is left. */
        glyph_cache_set_parallel(false);
        glyph_cache_flush_frame();
    }
}

static void test_coretext_family_listing(void)
{
    size_t count = 0;
    char **families = font_list_families(&count);
    assert(families != NULL);
    assert(count > 0);
    for (size_t i = 0; i < count; i++) {
        assert(families[i] != NULL);
        for (size_t j = i + 1; j < count; j++)
            assert(strcmp(families[i], families[j]) != 0);
        free(families[i]);
    }
    free(families);
}

static void test_coretext_styles(void)
{
    struct font_instance *fi = font_instance_default();
    font_bind_thread(fi);
    int rows = font_instance_metrics(fi)->cell_h;

    struct glyph g = {0};
    assert(font_instance_render_glyph(fi, 'A', FONT_STYLE_BOLD | FONT_STYLE_ITALIC, &g));
    assert(g.width > 0);
    assert(glyph_has_ink(&g, rows));
    glyph_release(&g);

    memset(&g, 0, sizeof(g));
    assert(font_instance_render_glyph(fi, 'g', FONT_STYLE_BOLD, &g));
    assert(g.width > 0);
    assert(glyph_has_ink(&g, rows));
    glyph_release(&g);
}

int main(void)
{
    @autoreleasepool {
        assert(font_system_init());
        test_coretext_metrics();
        test_coretext_glyph_rendering();
        test_coretext_family_listing();
        test_coretext_styles();
        test_coretext_scaling();
        test_coretext_glyph_orientation();
        test_coretext_hotswap();
        test_coretext_clusters();
        test_coretext_fallback_shaping();
        test_coretext_antialias();
        test_coretext_worker_lifecycle();
        font_system_shutdown();
        puts("PASS: CoreText native macOS font backend test");
        return 0;
    }
}
