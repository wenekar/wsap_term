#ifndef UI_TEXT_EDIT_H
#define UI_TEXT_EDIT_H

#include <stdbool.h>
#include "ui/keys.h"

int ui_text_edit_move(const char *text, int len, int pos,
                       enum ui_edit_unit unit, bool backward);

#endif
