#ifndef WSAP_TERM_MODEL_H
#define WSAP_TERM_MODEL_H

/*
 * A slow reference model of the terminal. It defines correct behavior for
 * the input that tests/fuzz_gen.h makes. model.c names its sources:
 * xterm and ECMA-48 for controls, Unicode for UTF-8, and this terminal's
 * own design where xterm has no rule (reflow, history, clusters).
 *
 * The model reads nothing from the terminal. Tests compare the two through
 * the public API only.
 */

#include "wsap_vtcore/wsap_vtcore.h"
#include "wsap_vtcore/unicode/grapheme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct model_cell {
    uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    uint8_t n;          /* code points; 0 for the right half of a wide cell */
    uint8_t width;      /* 1, 2 for a wide cell, 0 for its right half */
    uint16_t attrs;     /* WSAP_VTCORE_ATTR_* */
    wsap_vtcore_color fg, bg;
};

struct model;

/* history is the history limit, as in wsap_vtcore_options.scrollback. name is
 * as in wsap_vtcore_options.name. */
struct model *model_new(struct wsap_vtcore_vector2i size, int history, const char *name);
void model_free(struct model *m);
void model_feed(struct model *m, const char *s, size_t len, unsigned flags);
bool model_resize(struct model *m, struct wsap_vtcore_vector2i size);

struct wsap_vtcore_vector2i model_size(const struct model *m);
/* History rows. The alternate screen shows no history, so it reports 0. */
int model_history(const struct model *m);
/* Row 0 is the oldest history row. Row model_history() is the top screen
 * row. Return NULL for an invalid row. Different rows can be read together.
 * A feed or resize ends the lifetime of returned row data. */
const struct model_cell *model_row(const struct model *m, int row);
struct wsap_vtcore_cursor model_cursor(const struct model *m);
struct wsap_vtcore_modes model_modes(const struct model *m);
/* Replies since the last model_take_replies(). */
const char *model_replies(const struct model *m, size_t *len);
void model_take_replies(struct model *m);

#endif
