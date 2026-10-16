#ifndef FONT_BACKEND_H
#define FONT_BACKEND_H

#include "font/font.h"

#include <hb.h>

typedef struct font_handle font_handle_t;

struct font_window {
    const uint32_t *cps;
    int len;
    int core, core_len;
    uint16_t style;
    uint32_t script;
};

struct font_backend {
    const char *name;

    bool (*init)(void);
    void (*destroy)(void);
    bool (*thread_attach)(void);
    void (*thread_detach)(void);

    font_handle_t *(*open)(const char *family_list, struct font_size size);
    void (*close)(font_handle_t *face);
    bool (*measure)(font_handle_t *face, struct font_metrics *out);

    bool (*rasterize_glyph)(font_handle_t *face, uint32_t codepoint, uint16_t style,
                            const struct font_metrics *m, struct glyph *out);

    uint64_t (*scan_run)(const uint32_t *cps, int len, uint16_t style,
                         uint64_t *hash, int *dead_at);

    enum font_run_result (*shape_run)(font_handle_t *face, const struct font_window *w,
                                      const struct font_metrics *m, struct glyph *cells);

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
