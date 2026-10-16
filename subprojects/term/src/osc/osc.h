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

const char *wsap_vtcore_osc8_url_at(const struct wsap_vtcore *t, int col, int row);
void wsap_vtcore_osc8_cleanup(struct wsap_vtcore *t);

#endif
