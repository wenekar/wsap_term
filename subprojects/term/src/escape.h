#ifndef WSAP_VTCORE_ESCAPE_H
#define WSAP_VTCORE_ESCAPE_H
#include "internal.h"

void wsap_vtcore_escape_begin(struct wsap_vtcore *t);
void wsap_vtcore_escape_byte(struct wsap_vtcore *t, unsigned char ch);
size_t wsap_vtcore_escape_str(struct wsap_vtcore *t, const char *buf, size_t len);
size_t wsap_vtcore_escape_csi(struct wsap_vtcore *t, const char *buf, size_t len);

#endif
