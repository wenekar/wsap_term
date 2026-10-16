#ifndef WSAP_VTCORE_TEXT_H
#define WSAP_VTCORE_TEXT_H
#include "internal.h"

void wsap_vtcore_text_ground(struct wsap_vtcore *t, unsigned char ch);
/* End an unfinished UTF-8 sequence: it prints U+FFFD. */
void wsap_vtcore_text_utf8_interrupt(struct wsap_vtcore *t);
void wsap_vtcore_text_repeat(struct wsap_vtcore *t, int count);

#endif
