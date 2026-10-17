#define _GNU_SOURCE
#include "font/backend.h"
#include "font/backends/freetype/internal.h"
#include "font/glyph/intern.h"

#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include FT_MODULE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FT_LAST_RESORT_FAMILY
#define FT_LAST_RESORT_FAMILY "JetBrainsMono Nerd Font,JetBrainsMono,JetBrains Mono"
#endif

struct font_handle {
    int pixel_size;
};

char **font_list_families(size_t *count)
{
    *count = 0;
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FAMILY, FC_SPACING, NULL);
    FcFontSet *fonts = pattern && objects ? FcFontList(NULL, pattern, objects) : NULL;
    char **names = fonts ? calloc((size_t)fonts->nfont + 1, sizeof(*names)) : NULL;
    bool ok = names != NULL;
    for (int i = 0; ok && i < fonts->nfont; i++) {
        int spacing = 0;
        FcChar8 *family = NULL;
        if (FcPatternGetInteger(fonts->fonts[i], FC_SPACING, 0, &spacing) != FcResultMatch ||
            spacing < FC_MONO ||
            FcPatternGetString(fonts->fonts[i], FC_FAMILY, 0, &family) != FcResultMatch)
            continue;
        bool duplicate = false;
        for (size_t j = 0; j < *count; j++)
            duplicate |= !strcmp(names[j], (char *)family);
        if (duplicate) continue;
        names[*count] = strdup((char *)family);
        if (!names[*count]) ok = false;
        else (*count)++;
    }
    if (fonts) FcFontSetDestroy(fonts);
    if (objects) FcObjectSetDestroy(objects);
    if (pattern) FcPatternDestroy(pattern);
    if (!ok) {
        for (size_t i = 0; i < *count; i++) free(names[i]);
        free(names);
        names = NULL;
        *count = 0;
    }
    return names;
}

static __thread unsigned synced_epoch = 0;

static const char *env_first(const char *a, const char *b)
{
    const char *v = getenv(a);
    return v ? v : getenv(b);
}

static bool ft_fit_coverable(const uint32_t *cps, int len, uint16_t flags,
                             bool emoji, struct glyph *g)
{
    if (!run_coverable(cps, len))
        return false;
    struct font_face_query q = FONT_FACE_QUERY(cps[0], emoji);
    struct shaped_run run;
    int i;
    while ((i = font_next_face_covering(&q, cps, len)) >= 0)
        if (font_shape(i, cps, len, flags, 0, &run) != FONT_SHAPE_UNAVAILABLE &&
            font_fit_single(&run, g))
            return true;
    return false;
}

static bool ft_new_library(void)
{
    if (FT_Init_FreeType(&ft_lib) != 0)
        return false;
    FT_Bool darken = 0;
    FT_Property_Set(ft_lib, "cff", "no-stem-darkening", &darken);
    FT_Property_Set(ft_lib, "autofit", "no-stem-darkening", &darken);
    font_hinting_init_library(ft_lib);
    return true;
}

static bool ft_init(void)
{
    if (ft_lib)
        return true;
    if (!ft_new_library())
        return false;
    if (!FcInit()) {
        FT_Done_FreeType(ft_lib);
        ft_lib = NULL;
        return false;
    }
#ifdef WSAP_DEV_FONTS_DIR
    FcConfigAppFontAddDir(NULL, (const FcChar8 *)WSAP_DEV_FONTS_DIR);
#endif
#ifdef WSAP_DATADIR
    FcConfigAppFontAddDir(NULL, (const FcChar8 *)(WSAP_DATADIR "/wsap_term/fonts"));
#endif
    fc_ready = true;
    if (font_hinting_has_auto_axis())
        ft_apply_hinting();

    ft_applied_px = FONT_PT_TO_PX(font_configured_size());
    ft_reload_family();
    return true;
}

void ft_reload_family(void)
{
    if (!ft_lib)
        return;
    shape_thread_reset();
    fallback_destroy();
    release_all_faces();
    configured_faces = MAX_FACES;
    fallback_coverage_reset();
    load_named_face(font_get_family());
    if (!num_faces) load_named_face(FT_LAST_RESORT_FAMILY);
    if (!num_faces) load_named_face("monospace");
    if (num_faces) {
        struct font_metrics m = ft_measure_cell(faces[0].styles[0]);
        font_thread_metrics_set(&m);
    }

    const char *unicode_env = env_first("WSAP_ALL_UNICODE", "WSAP_ALL_UNICODE_FONT");
    load_named_face(unicode_env);
    defer_named_face("Noto Color Emoji");
    face_snapshot_configured();
    build_cfg_usable();
    synced_epoch = atomic_fetch_add_explicit(&ft_sync_epoch, 1,
                                             memory_order_release) + 1;
}

static void ft_destroy(void)
{
    if (!ft_lib && !shape_buffer_live() && !num_faces && !fallback_ready())
        return;
    shape_thread_reset();
    face_stamp = 0;
    fallback_destroy();
    fallback_coverage_reset();
    release_all_faces();
    face_cfg_specs_destroy();
    configured_faces = 0;

    if (ft_lib) {
        FT_Done_FreeType(ft_lib);
        ft_lib = NULL;
    }
    FcFini();
    fc_ready = false;
    synced_epoch = 0;
    atomic_fetch_add_explicit(&ft_sync_epoch, 1, memory_order_relaxed);
}

static bool ft_thread_attach(void)
{
    unsigned e = atomic_load_explicit(&ft_sync_epoch, memory_order_acquire);
    if (ft_lib && synced_epoch == e)
        return true;
    release_all_faces();
    shape_thread_reset();
    fallback_thread_reset();
    if (ft_lib)
        FT_Done_FreeType(ft_lib);
    ft_lib = NULL;
    if (!ft_new_library())
        return false;
    face_reload_configured();
    if (!num_faces)
        return false;
    synced_epoch = e;
    return true;
}

static void ft_thread_detach(void)
{
    release_all_faces();
    shape_thread_reset();
    face_stamp = 0;
    fallback_thread_reset();
    if (ft_lib) {
        FT_Done_FreeType(ft_lib);
        ft_lib = NULL;
    }
    synced_epoch = 0;
}

static bool ft_thread_ready(void)
{
    unsigned e = atomic_load_explicit(&ft_sync_epoch, memory_order_acquire);
    if (ft_lib && synced_epoch == e)
        return true;
    return ft_thread_attach();
}

static font_handle_t *ft_face_open(const char *family_list, struct font_size size)
{
    (void)family_list;
    if (!ft_thread_ready())
        return NULL;
    struct font_handle *face = calloc(1, sizeof(*face));
    if (face)
        face->pixel_size = font_size_px(size);
    return face;
}

static void ft_face_close(font_handle_t *face)
{
    free(face);
}

static bool ft_face_metrics(font_handle_t *face, struct font_metrics *out)
{
    if (!face || !out)
        return false;
    if (!ft_thread_ready())
        return false;
    if (!num_faces)
        return false;
    ensure_primary_face(0);
    if (!faces[0].styles[0])
        return false;

    ft_size_faces(face->pixel_size);
    struct font_metrics m = ft_measure_cell(faces[0].styles[0]);
    m.pixel_size = face->pixel_size;
    font_thread_metrics_set(&m);
    *out = m;
    return true;
}

static bool ft_rasterize_glyph(font_handle_t *face, uint32_t codepoint, uint16_t style,
                               const struct font_metrics *m, struct glyph *out)
{
    (void)face;
    if (!ft_thread_ready())
        return false;
    face_activate(m);

    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int len = glyph_intern_expand(codepoint, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    if (len > 1) {
        bool emoji = font_prefers_emoji(cps[0], style) ||
                     font_prefers_emoji(cps[len - 1], style);
        if (ft_fit_coverable(cps, len, style, emoji, out))
            return true;
    }

    return ft_fit_coverable(&codepoint, 1, style, font_prefers_emoji(codepoint, style), out);
}

static bool core_unchanged(const struct shaped_run *run, const struct font_window *w)
{
    FT_Face ft = faces[run->slot].styles[run->variant];
    int seen = 0;
    for (int k = 0; k < run->count; k++) {
        const struct shaped_glyph *g = &run->glyphs[k];
        int c = g->cluster - w->core;
        if (c < 0 || c >= w->core_len)
            continue;
        if (g->x_off || g->y_off || !ft ||
            g->gid != FT_Get_Char_Index(ft, w->cps[g->cluster]))
            return false;
        seen++;
    }
    return seen == w->core_len;
}

static void keep_core(struct shaped_run *run, const struct font_window *w)
{
    int n = 0;
    for (int k = 0; k < run->count; k++) {
        struct shaped_glyph g = run->glyphs[k];
        int c = g.cluster - w->core;
        if (c < 0 || c >= w->core_len)
            continue;
        g.cluster = (uint16_t)c;
        run->glyphs[n++] = g;
    }
    run->count = n;
}

static enum font_run_result ft_shape_run(font_handle_t *face, const struct font_window *w,
                                         const struct font_metrics *m, struct glyph *cells)
{
    (void)face;
    if (!ft_thread_ready())
        return FONT_RUN_UNAVAILABLE;
    face_activate(m);
    const uint32_t *cps = w->cps;
    int len = w->len;
    uint16_t style = w->style;

    int dead_at;
    uint64_t cfg_and =
        ft_scan_run(cps, len, style & FONT_STYLE_VARIANT_MASK,
                    NULL, &dead_at);
    if (dead_at >= 0)
        return FONT_RUN_UNAVAILABLE;

    struct font_face_query q = FONT_FACE_QUERY(cps[0], false);
    q.cfg_mask = cfg_and;
    if (!cfg_and && !fallback_run_covered(cps, len))
        return FONT_RUN_UNAVAILABLE;

    bool retry_suffix = false;
    struct shaped_run run;
    int i;
    while ((i = font_next_face_for_run(&q, cps, len, style)) >= 0) {
        if (faces[i].is_emoji)
            continue;
        if (q.vfail)
            retry_suffix = true;
        enum font_shape_result r = font_shape(i, cps, len, style, w->script, &run);
        if (r == FONT_SHAPE_UNAVAILABLE) {
            retry_suffix = true;
            continue;
        }
        if (r == FONT_SHAPE_NO_SUBST || core_unchanged(&run, w))
            return retry_suffix ? FONT_RUN_UNAVAILABLE : FONT_RUN_UNCHANGED;
        keep_core(&run, w);
        if (run.count && font_fit_cells(&run, w->core_len, cells))
            return FONT_RUN_SHAPED;
        retry_suffix = true;
    }
    return FONT_RUN_UNAVAILABLE;
}

static hb_face_t *ft_layout_face(font_handle_t *face, uint16_t style)
{
    (void)face;
    if (!ft_thread_ready() || !num_faces)
        return NULL;
    ensure_primary_face(0);
    enum font_variant want = font_variant_of(style);
    if (want)
        ensure_styled_face(0, want);
    FT_Face ft = faces[0].styles[want] ? faces[0].styles[want] : faces[0].styles[0];
    return ft ? hb_ft_face_create_referenced(ft) : NULL;
}

const struct font_backend font_backend_freetype = {
    .name = "freetype",
    .init = ft_init,
    .destroy = ft_destroy,
    .thread_attach = ft_thread_attach,
    .thread_detach = ft_thread_detach,
    .open = ft_face_open,
    .close = ft_face_close,
    .measure = ft_face_metrics,
    .rasterize_glyph = ft_rasterize_glyph,
    .scan_run = ft_scan_run,
    .shape_run = ft_shape_run,
    .layout_face = ft_layout_face,
};
