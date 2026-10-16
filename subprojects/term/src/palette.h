#ifndef WSAP_VTCORE_PALETTE_H
#define WSAP_VTCORE_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

#include "wsap_vtcore.h"

/*
 * The colors of one terminal. base holds the colors that the theme gives;
 * colors holds base plus the entries that the program changed with OSC 4,
 * 10, 11, or 12. version changes whenever colors changes.
 */
struct palette {
    uint32_t colors[WSAP_VTCORE_PALETTE_SIZE];
    uint32_t base[WSAP_VTCORE_PALETTE_SIZE];
    struct wsap_vtcore_theme theme;
    uint64_t version;
};

/* Set every entry from theme. An all-zero theme selects the built-in theme. */
void wsap_vtcore_palette_init(struct palette *p, const struct wsap_vtcore_theme *theme);
/* Change one entry (0-255, WSAP_VTCORE_PALETTE_FG, _BG, or _CURSOR). */
void wsap_vtcore_palette_set(struct palette *p, int slot, uint32_t argb);
/* Restore one entry, or all entries, to the theme color. */
void wsap_vtcore_palette_reset(struct palette *p, int slot);
void wsap_vtcore_palette_reset_all(struct palette *p);

#endif
