#ifndef WSAP_TERM_MODEL_H
#define WSAP_TERM_MODEL_H

#include "wsap_vtcore/wsap_vtcore.h"
#include "wsap_vtcore/unicode/grapheme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct model_cell {
    uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    uint8_t n;
    uint8_t width;
    uint16_t attrs;
    wsap_vtcore_color fg, bg;
};

struct model;

struct model *model_new(struct wsap_vtcore_vector2i size, int history, const char *name);
void model_free(struct model *m);
void model_feed(struct model *m, const char *s, size_t len, unsigned flags);
bool model_resize(struct model *m, struct wsap_vtcore_vector2i size);

struct wsap_vtcore_vector2i model_size(const struct model *m);
int model_history(const struct model *m);
const struct model_cell *model_row(const struct model *m, int row);
struct wsap_vtcore_cursor model_cursor(const struct model *m);
struct wsap_vtcore_modes model_modes(const struct model *m);
const char *model_replies(const struct model *m, size_t *len);
void model_take_replies(struct model *m);

#endif
