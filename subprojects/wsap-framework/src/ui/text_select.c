#include "ui/text_select.h"

#include "ui/text_break.h"

#include <stdlib.h>

static size_t clamp_offset(const char *text, size_t len, size_t offset)
{
    return text ? ui_text_clamp_offset(text, len, offset) : 0;
}

/* Selects the word or the line under `at`. A press of one click does
 * nothing. */
static void select_unit(struct ui_text_select *sel, const char *text,
                        size_t len, size_t at, unsigned clicks)
{
    if (clicks < 2)
        return;
    ui_text_unit_range(text, len, at,
                       clicks >= 3 ? UI_EDIT_LINE : UI_EDIT_WORD,
                       &sel->gesture_start, &sel->gesture_end);
    sel->anchor = sel->gesture_start;
    sel->caret = sel->gesture_end;
}

void ui_text_select_clear(struct ui_text_select *sel)
{
    if (!sel)
        return;
    *sel = (struct ui_text_select){0};
}

void ui_text_select_end(struct ui_text_select *sel)
{
    if (!sel)
        return;
    sel->pressed = false;
    sel->dragged = false;
    sel->clicks = 0;
}

void ui_text_select_to(struct ui_text_select *sel, size_t offset, bool extend)
{
    if (!sel)
        return;
    sel->caret = offset;
    if (!extend)
        sel->anchor = offset;
}

void ui_text_select_move(struct ui_text_select *sel, const char *text,
                         size_t len, enum ui_edit_unit unit, bool backward,
                         bool extend)
{
    if (!sel)
        return;
    size_t lo, hi;
    if (!extend && unit == UI_EDIT_CHARACTER && ui_text_select_range(sel, &lo, &hi)) {
        ui_text_select_to(sel, backward ? lo : hi, false);
        return;
    }
    ui_text_select_to(sel, ui_text_move(text, len, sel->caret, unit, backward),
                      extend);
}

void ui_text_select_all(struct ui_text_select *sel, size_t len)
{
    if (!sel)
        return;
    sel->anchor = 0;
    sel->caret = len;
}

void ui_text_select_apply(struct ui_text_select *sel, const char *text,
                          size_t len, size_t start, size_t end, size_t caret)
{
    if (!sel)
        return;
    if (start == end) {
        sel->caret = clamp_offset(text, len, caret);
        sel->anchor = sel->caret;
        return;
    }
    sel->anchor = clamp_offset(text, len, start);
    sel->caret = clamp_offset(text, len, end);
}

void ui_text_select_press(struct ui_text_select *sel, const char *text,
                          size_t len, const struct ui_text_select_click *click)
{
    if (!sel || !click)
        return;
    struct vector2i delta = vector2i_sub(click->pos, sel->click_pos);
    bool repeated = sel->clicks && click->interval_ms &&
        click->time_ms >= sel->click_at &&
        click->time_ms - sel->click_at <= click->interval_ms &&
        llabs((long long)delta.x) <= click->threshold_px &&
        llabs((long long)delta.y) <= click->threshold_px;
    sel->clicks = repeated ? sel->clicks % 3 + 1 : 1;
    sel->click_at = click->time_ms;
    sel->click_pos = sel->press_pos = click->pos;
    sel->pressed = true;
    sel->dragged = false;
    sel->selects = click->selects;
    size_t at = click->offset <= len ? click->offset : len;
    if (click->selects && sel->clicks > 1) {
        select_unit(sel, text, len, at, sel->clicks);
        return;
    }
    if (!click->selects) {
        sel->caret = sel->anchor = at;
        return;
    }
    ui_text_select_to(sel, at, (click->mods & UI_MOD_SHIFT) != 0);
}

bool ui_text_select_drag(struct ui_text_select *sel, const char *text,
                        size_t len, size_t offset, struct vector2i pos,
                        int threshold_px)
{
    if (!sel || !sel->pressed)
        return false;
    struct vector2i delta = vector2i_sub(pos, sel->press_pos);
    if (llabs((long long)delta.x) > threshold_px ||
        llabs((long long)delta.y) > threshold_px)
        sel->dragged = true;
    if (!sel->dragged)
        return false;
    if (!sel->selects)
        return false;
    size_t at = offset <= len ? offset : len;
    if (sel->clicks > 1) {
        size_t lo, hi;
        ui_text_unit_range(text, len, at,
                           sel->clicks >= 3 ? UI_EDIT_LINE : UI_EDIT_WORD,
                           &lo, &hi);
        sel->anchor = lo < sel->gesture_start ? sel->gesture_end
                                             : sel->gesture_start;
        sel->caret = lo < sel->gesture_start ? lo
                   : hi > sel->gesture_end ? hi : sel->gesture_end;
    } else {
        sel->caret = at;
    }
    return true;
}

void ui_text_select_release(struct ui_text_select *sel)
{
    if (!sel)
        return;
    sel->pressed = false;
}

bool ui_text_select_range(const struct ui_text_select *sel, size_t *start,
                          size_t *end)
{
    if (!sel || sel->anchor == sel->caret)
        return false;
    if (start)
        *start = sel->anchor < sel->caret ? sel->anchor : sel->caret;
    if (end)
        *end = sel->anchor > sel->caret ? sel->anchor : sel->caret;
    return true;
}
