#include "ui/text_edit.h"
#include "wsap_vtcore/unicode/utf8.h"

#include <unicode/ubrk.h>
#include <unicode/utext.h>

static UBreakIterator *iterator(UText *utext, const char *text, int len, UBreakIteratorType type)
{
    UErrorCode error = U_ZERO_ERROR;
    utext_openUTF8(utext, text, len, &error);
    UBreakIterator *it = ubrk_open(type, NULL, NULL, 0, &error);
    if (U_SUCCESS(error))
        ubrk_setUText(it, utext, &error);
    if (U_FAILURE(error)) {
        ubrk_close(it);
        return NULL;
    }
    return it;
}

static int character_fallback(const char *text, int len, int pos, bool backward)
{
    if (backward) {
        if (pos > 0) pos--;
        while (pos > 0 && ((unsigned char)text[pos] & 0xc0) == 0x80) pos--;
    } else if (pos < len) {
        uint32_t cp;
        int n = wsap_vtcore_utf8_decode_one(text + pos, (size_t)(len - pos), &cp);
        pos += n > 0 ? n : 1;
    }
    return pos;
}

int ui_text_edit_move(const char *text, int len, int pos,
                       enum ui_edit_unit unit, bool backward)
{
    if (unit == UI_EDIT_LINE)
        return backward ? 0 : len;
    UText utext = UTEXT_INITIALIZER;
    UBreakIterator *it = iterator(&utext, text, len,
        unit == UI_EDIT_CHARACTER ? UBRK_CHARACTER : UBRK_WORD);
    int result = backward ? 0 : len;
    if (!it) {
        result = character_fallback(text, len, pos, backward);
    } else if (unit == UI_EDIT_CHARACTER) {
        result = backward ? ubrk_preceding(it, pos) : ubrk_following(it, pos);
        if (result == UBRK_DONE) result = backward ? 0 : len;
    } else {
        int start = ubrk_first(it), end;
        while ((end = ubrk_next(it)) != UBRK_DONE) {
            if (ubrk_getRuleStatus(it) != UBRK_WORD_NONE) {
                if (backward && start < pos)
                    result = start;
                if (!backward &&
                    (unit == UI_EDIT_WORD_END ? end > pos : start > pos)) {
                    result = unit == UI_EDIT_WORD_END ? end : start;
                    break;
                }
            }
            start = end;
        }
    }
    ubrk_close(it);
    utext_close(&utext);
    return result;
}
