#ifndef WSAP_VTCORE_PALETTE_H
#define WSAP_VTCORE_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#include "wsap_vtcore.h"

struct palette {
    uint32_t colors[WSAP_VTCORE_PALETTE_SIZE];
    uint32_t base[WSAP_VTCORE_PALETTE_SIZE];
    struct wsap_vtcore_theme theme;
    uint64_t version;
};

void wsap_vtcore_palette_init(struct palette *p, const struct wsap_vtcore_theme *theme);
void wsap_vtcore_palette_set(struct palette *p, int slot, uint32_t argb);
void wsap_vtcore_palette_reset(struct palette *p, int slot);
void wsap_vtcore_palette_reset_all(struct palette *p);

#endif
