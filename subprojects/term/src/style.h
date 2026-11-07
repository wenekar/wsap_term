#ifndef WSAP_VTCORE_STYLE_H
#define WSAP_VTCORE_STYLE_H

#include "cell.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The style table holds the parts of a cell style that do not fit in a
 * cell: the link and the underline color. A cell keeps a style id in the
 * low bits of its pad field (CELL_EXT_ID). The table interns each pair of
 * link and underline color once. Id zero means no link and no underline
 * color, so a plain cell needs no table lookup.
 *
 * Cells, text rows, and history runs copy the pad field without a change.
 * Thus history and reflow keep the style. A full table collects the ids
 * that no cell uses. The link table collects its links the same way. */

struct wsap_vtcore;

struct style_entry {
    uint32_t ul;
    uint16_t link;
    uint16_t live;
};

struct wsap_vtcore_styles {
    struct style_entry *entry;
    uint16_t *slot;
    uint16_t *free_ids;
    uint32_t slot_cap;
    uint16_t cap, next, free_count;
};

/* Return the entry of the style id in pad. Return NULL for id zero. */
static inline const struct style_entry *wsap_vtcore_style_entry(const struct wsap_vtcore_styles *s,
                                                                uint16_t pad)
{
    uint16_t id = pad & CELL_EXT_ID;
    if (!s || !id || id >= s->next)
        return NULL;
    return &s->entry[id];
}

/* Set cur_pad and base_pad from cur_link, cur_ul, and the flags in cur_pad.
 * When the table cannot take a new style, the pen loses its underline color
 * first, and then its link. */
void wsap_vtcore_style_update(struct wsap_vtcore *t);

/* Release the styles that no cell, text row, history line, or pen uses.
 * When link_mark is not NULL, also set the bit of each link that a kept
 * style uses. Bit id - 1 stands for link id; link_cap limits the ids. */
void wsap_vtcore_style_gc(struct wsap_vtcore *t, uint64_t *link_mark, uint16_t link_cap);

void wsap_vtcore_style_free(struct wsap_vtcore *t);

#endif
