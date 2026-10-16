#ifndef WSAP_TERM_GSTORE_H
#define WSAP_TERM_GSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gstore;
struct wsap_vtcore;

/* Create a grapheme store for a terminal. */
struct gstore *wsap_vtcore_gstore_new(struct wsap_vtcore *t);

/* Free the grapheme store and its memory. */
void wsap_vtcore_gstore_free(struct gstore *g);

/* Store a cluster of code points. Return its handle. */
uint32_t wsap_vtcore_gstore_intern(struct gstore *g, const uint32_t *cps, int n);

/* Expand a handle to its code points. Return the count. */
int wsap_vtcore_gstore_expand(const struct gstore *g, uint32_t handle, uint32_t *cps, int cap);

/* Mark reachable clusters and free unused ones. */
void wsap_vtcore_gstore_sweep(struct gstore *g, struct wsap_vtcore *t);

/* Mark one cluster handle as reachable. */
void wsap_vtcore_gstore_mark(struct gstore *g, uint32_t handle);

/* Return the count of active clusters in the store. */
uint32_t wsap_vtcore_gstore_count(const struct gstore *g);

/* Suppress automatic GC sweeps while temporary cell buffers hold handles. */
void wsap_vtcore_gstore_pin(struct gstore *g);
void wsap_vtcore_gstore_unpin(struct gstore *g);

#endif
