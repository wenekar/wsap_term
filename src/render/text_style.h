#ifndef RENDER_TEXT_STYLE_H
#define RENDER_TEXT_STYLE_H

#include <stdint.h>

#include "font/font.h"
#include "paint/scene.h"

_Static_assert((unsigned)UI_TEXT_BOLD == (unsigned)FONT_STYLE_BOLD &&
               (unsigned)UI_TEXT_ITALIC == (unsigned)FONT_STYLE_ITALIC,
               "UI text bits must match font style bits");

static inline uint16_t render_text_style(uint32_t ui_flags)
{
    return (uint16_t)(ui_flags & (UI_TEXT_BOLD | UI_TEXT_ITALIC));
}

#endif
