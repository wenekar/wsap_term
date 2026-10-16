#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>

#include "font/font.h"
#include "paint/blend.h"

struct font_metrics scene_paint_test_font = { .cell_w = 8, .cell_h = 16 };
const struct glyph *(*scene_paint_glyph_hook)(uint32_t cp, uint16_t flags);

const struct glyph *glyph_cache_fetch(struct font_instance *fi, uint32_t c, uint16_t flags)
{
    (void)fi;
    return scene_paint_glyph_hook ? scene_paint_glyph_hook(c, flags)
                                  : (const struct glyph *)0;
}

void glyph_cache_refresh(struct font_instance *fi)
{
    (void)fi;
}

#include <stdalign.h>

uint16_t render_srgb_to_linear[258];
alignas(64) uint8_t render_linear_to_srgb[65540];
_Atomic bool render_srgb_luts_ready;

void render_build_srgb_luts(void)
{
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        render_srgb_to_linear[i] = (uint16_t)(l * 65535.0 + 0.5);
    }
    for (int i = 0; i < 65536; i++) {
        double l = i / 65535.0;
        double c = l <= 0.0031308 ? l * 12.92
                                  : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        render_linear_to_srgb[i] = (uint8_t)(c * 255.0 + 0.5);
    }
    render_srgb_to_linear[256] = render_srgb_to_linear[255];
    render_srgb_to_linear[257] = render_srgb_to_linear[255];
    for (int i = 65536; i < 65540; i++)
        render_linear_to_srgb[i] = render_linear_to_srgb[65535];
    atomic_store_explicit(&render_srgb_luts_ready, true, memory_order_release);
}
