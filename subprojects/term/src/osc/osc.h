#ifndef WSAP_VTCORE_OSC_H
#define WSAP_VTCORE_OSC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wsap_vtcore;

#define WSAP_VTCORE_OSC_MAX (16u << 20)

size_t wsap_vtcore_osc_room(const struct wsap_vtcore *t);
char *wsap_vtcore_osc_tail(struct wsap_vtcore *t);
bool wsap_vtcore_osc_grow(struct wsap_vtcore *t);

void wsap_vtcore_osc_finish(struct wsap_vtcore *t);

void wsap_vtcore_osc_cleanup(struct wsap_vtcore *t);

void wsap_vtcore_osc8(struct wsap_vtcore *t, const char *params);
void wsap_vtcore_osc52(struct wsap_vtcore *t, const char *params);
void wsap_vtcore_osc_misc_dispatch(struct wsap_vtcore *t, long code, const char *params);

/* Decode n bytes of base64. out needs room for (n / 4 + 1) * 3 bytes. */
bool wsap_vtcore_b64_decode(const char *s, size_t n, char *out, size_t *out_len);

/* Shell integration (shell.c). params follows the code and its ';'. */
void wsap_vtcore_osc133(struct wsap_vtcore *t, const char *params);
/* OSC 9 forms of ConEmu: 4 (progress) and 9 (current directory). Return
 * false for other OSC 9 text, which is a notification. */
bool wsap_vtcore_osc9_conemu(struct wsap_vtcore *t, const char *params);
void wsap_vtcore_osc1337(struct wsap_vtcore *t, const char *params);

/* Titles from OSC 0, 1, and 2. The core keeps one title for all three. */
void wsap_vtcore_title_set(struct wsap_vtcore *t, const char *title);
void wsap_vtcore_title_push(struct wsap_vtcore *t);
void wsap_vtcore_title_pop(struct wsap_vtcore *t);
void wsap_vtcore_title_stack_clear(struct wsap_vtcore *t);

const char *wsap_vtcore_osc8_url_at(const struct wsap_vtcore *t, int col, int row);
void wsap_vtcore_osc8_cleanup(struct wsap_vtcore *t);

#endif
