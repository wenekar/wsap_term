#ifndef WSAP_VTCORE_GRAPHICS_H
#define WSAP_VTCORE_GRAPHICS_H

#include "wsap_vtcore.h"

#define WSAP_VTCORE_APC_MAX (4096 + 256)
#define WSAP_VTCORE_GRAPHICS_DIM_MAX 4096
#define WSAP_VTCORE_GRAPHICS_BYTES_MAX (32u << 20)

void wsap_vtcore_kitty_dispatch(struct wsap_vtcore *t, const char *buf, size_t len);
void wsap_vtcore_kitty_abort(struct wsap_vtcore *t);
void wsap_vtcore_graphics_reset(struct wsap_vtcore *t);
void wsap_vtcore_graphics_history_free(struct wsap_vtcore *t);
void wsap_vtcore_graphics_history_add(struct wsap_vtcore *t, uint64_t line, uint64_t cell,
                                      uint64_t uid);
uint64_t wsap_vtcore_graphics_history_row(const struct wsap_vtcore *t, uint64_t line,
                                          int segment, int cols);
void wsap_vtcore_graphics_history_cut(struct wsap_vtcore *t, uint64_t line, uint64_t cell);
void wsap_vtcore_graphics_discard_screen(struct wsap_vtcore *t);
void wsap_vtcore_graphics_erase(struct wsap_vtcore *t, int x, int y, int width, int height);
void wsap_vtcore_graphics_erase_sixel(struct wsap_vtcore *t, int x, int y, int width, int height);
bool wsap_vtcore_sixel_header(const char *buf, size_t len);
bool wsap_vtcore_sixel_dispatch(struct wsap_vtcore *t, const char *buf, size_t len);
enum wsap_vtcore_graphics_result wsap_vtcore_graphics_emit(struct wsap_vtcore *t,
                                                         struct wsap_vtcore_graphics *cmd);

#endif
