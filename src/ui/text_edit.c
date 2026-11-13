#include "ui/text_edit.h"
#include "ui/text_break.h"

struct word_move_ctx {
    size_t pos;
    size_t result;
    enum ui_edit_unit unit;
    bool backward;
};

static bool word_move_cb(size_t start, size_t end, bool is_word, void *userdata)
{
    struct word_move_ctx *ctx = userdata;
    if (!is_word)
        return true;
    if (ctx->backward) {
        if (start < ctx->pos) {
            ctx->result = start;
            return true;
        }
        return false;
    }
    size_t target = ctx->unit == UI_EDIT_WORD_END ? end : start;
    if (target > ctx->pos) {
        ctx->result = target;
        return false;
    }
    return true;
}

int ui_text_edit_move(const char *text, int len, int pos,
                       enum ui_edit_unit unit, bool backward)
{
    if (!text || len <= 0)
        return 0;
    if (pos < 0)
        pos = 0;
    if (pos > len)
        pos = len;
    if (unit == UI_EDIT_LINE)
        return backward ? 0 : len;
    if (unit == UI_EDIT_CHARACTER) {
        size_t res = backward
            ? ui_text_break_grapheme_prev(text, (size_t)len, (size_t)pos)
            : ui_text_break_grapheme_next(text, (size_t)len, (size_t)pos);
        return (int)res;
    }
    struct word_move_ctx ctx = {
        .pos = (size_t)pos,
        .result = backward ? 0 : (size_t)len,
        .unit = unit,
        .backward = backward,
    };
    ui_text_each_word(text, (size_t)len, word_move_cb, &ctx);
    return (int)ctx.result;
}

size_t ui_text_edit_clamp_offset(const char *text, size_t len, size_t offset)
{
    if (offset >= len)
        return len;
    while (offset > 0 && ((unsigned char)text[offset] & 0xc0) == 0x80)
        offset--;
    return offset;
}
