#ifndef WSAP_TERM_GSTORE_H
#define WSAP_TERM_GSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gstore;
struct wsap_vtcore;

struct gstore *wsap_vtcore_gstore_new(struct wsap_vtcore *t);

void wsap_vtcore_gstore_free(struct gstore *g);

uint32_t wsap_vtcore_gstore_intern(struct gstore *g, const uint32_t *cps, int n);

int wsap_vtcore_gstore_expand(const struct gstore *g, uint32_t handle, uint32_t *cps, int cap);

void wsap_vtcore_gstore_sweep(struct gstore *g, struct wsap_vtcore *t);

void wsap_vtcore_gstore_mark(struct gstore *g, uint32_t handle);

uint32_t wsap_vtcore_gstore_count(const struct gstore *g);

void wsap_vtcore_gstore_pin(struct gstore *g);
void wsap_vtcore_gstore_unpin(struct gstore *g);

#endif
