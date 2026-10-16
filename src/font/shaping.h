#ifndef WSAP_FONT_SHAPING_H
#define WSAP_FONT_SHAPING_H

#include "font/font.h"
#include <hb.h>

/* Keep separator lines separate. Short operators can still form ligatures. */
static inline void font_shape_text(hb_font_t *font, hb_buffer_t *buffer,
                                   const uint32_t *cps, int len)
{
    static const hb_tag_t tags[] = {
        HB_TAG('c', 'a', 'l', 't'), HB_TAG('l', 'i', 'g', 'a'),
        HB_TAG('c', 'l', 'i', 'g'), HB_TAG('d', 'l', 'i', 'g'),
        HB_TAG('c', 'c', 'm', 'p'),
    };
    hb_feature_t features[FONT_RUN_MAX / 4 * 5];
    unsigned count = 0;
    for (int i = 0; i < len;) {
        if (cps[i] != '=') {
            i++;
            continue;
        }
        int start = i;
        while (i < len && cps[i] == '=')
            i++;
        if (i - start < 4)
            continue;
        for (unsigned k = 0; k < sizeof(tags) / sizeof(tags[0]); k++)
            features[count++] = (hb_feature_t){ tags[k], 0, (unsigned)start, (unsigned)i };
    }
    hb_shape(font, buffer, features, count);
}

#endif
