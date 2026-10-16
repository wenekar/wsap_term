#include "font/font.h"

#include <string.h>

static bool family_differs(const char *a, const char *b)
{
    return (a || b) && (!a || !b || strcmp(a, b) != 0);
}

void font_apply_settings(const struct font_settings *prev,
                         const struct font_settings *next)
{
    if (!prev) {
        font_set_family(next->family);
        font_set_configured_size(next->size_pt);
        font_set_hint_style(next->hinting);
        font_set_aa_mode(next->antialias);
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
        font_set_smoothing(next->smoothing);
#endif
        return;
    }
    if (family_differs(prev->family, next->family))
        font_set_family(next->family);
    if (prev->size_pt != next->size_pt)
        font_set_configured_size(next->size_pt);
    if (prev->hinting != next->hinting)
        font_set_hint_style(next->hinting);
    if (prev->antialias != next->antialias)
        font_set_aa_mode(next->antialias);
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    if (prev->smoothing != next->smoothing)
        font_set_smoothing(next->smoothing);
#endif
}
