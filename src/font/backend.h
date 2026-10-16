#ifndef FONT_BACKEND_H
#define FONT_BACKEND_H

#include "font/font.h"

#include <hb.h>

/* An opaque font handle that the backend owns. The FreeType and CoreText
 * backends make it. */
typedef struct font_handle font_handle_t;

/* Code points to shape, and the cells of them whose glyphs the caller
 * wants: [core, core + core_len). The other code points are context. */
struct font_window {
    const uint32_t *cps;
    int len;
    int core, core_len;
    uint16_t style;
    /* An hb_script_t, or 0 to let the shaper guess. */
    uint32_t script;
};

struct font_backend {
    const char *name;

    /* Subsystem initialization and teardown */
    bool (*init)(void);
    void (*destroy)(void);
    /* Prepares the per-thread state. NULL when the backend has none to
     * load ahead of use. */
    bool (*thread_attach)(void);
    /* Frees the per-thread state of the calling thread. */
    void (*thread_detach)(void);

    /* Explicit font face lifecycle for one size. Metrics, glyphs, and
     * shaped runs are in device pixels at font_size_px(size). */
    font_handle_t *(*open)(const char *family_list, struct font_size size);
    void (*close)(font_handle_t *face);
    bool (*measure)(font_handle_t *face, struct font_metrics *out);

    /* Glyph rasterization with explicit metrics. The handle is a per-size
     * context. */
    bool (*rasterize_glyph)(font_handle_t *face, uint32_t codepoint, uint16_t style,
                            const struct font_metrics *m, struct glyph *out);

    /* Hashes style and cps into *hash when hash is not NULL. Sets
     * *dead_at to the first code point that no face covers, or to -1.
     * *hash is not valid when *dead_at is not -1. The return value is
     * backend data; common code ignores it. NULL: the caller hashes the
     * run and treats it as covered. */
    uint64_t (*scan_run)(const uint32_t *cps, int len, uint16_t style,
                         uint64_t *hash, int *dead_at);

    /* Shapes w and fits the glyphs of its core into w->core_len cells.
     * UNCHANGED means shaping leaves the core glyphs as they are. */
    enum font_run_result (*shape_run)(font_handle_t *face, const struct font_window *w,
                                      const struct font_metrics *m, struct glyph *cells);

    /* The face that shapes style, for the layout analysis. A new
     * reference, or NULL. */
    hb_face_t *(*layout_face)(font_handle_t *face, uint16_t style);
};

extern const struct font_backend font_backend_freetype;
#if defined(__APPLE__)
extern const struct font_backend font_backend_coretext;
#endif

#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
#define FONT_BACKEND (&font_backend_coretext)
#else
#define FONT_BACKEND (&font_backend_freetype)
#endif

#endif
