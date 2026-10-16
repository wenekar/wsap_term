#include "ui/widgets/label.h"

#include "ui/debug.h"
#include "ui/paint.h"
#include "ui/surface.h"
#include "paint/text_run.h"
#include "ui/ui_util.h"
#include "ui/widget_impl.h"

#include <stdlib.h>
#include <string.h>

static struct ui_label *label_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct ui_label, widget);
}

static struct {
    ui_part_id selection;
    ui_part_id link;
    ui_part_id focus;
} parts;

static void label_parts_init(void)
{
    if (parts.selection)
        return;
    parts.selection = ui_part_intern("label-selection");
    parts.link = ui_part_intern("label-link");
    parts.focus = ui_part_intern("label-focus");
}

static struct css_style label_part(const struct ui_label *label, ui_part_id part)
{
    return ui_widget_part(&label->widget, part, label->widget.impl.style_pseudo);
}

struct label_line {
    size_t start, len, next;
    int cells;
    bool ellipsis;
};

struct label_iter {
    const struct ui_label *label;
    size_t off, end, next;
    int cap;
    bool segment, done;
};

static size_t next_offset(const char *text, size_t len, size_t off, int *cells)
{
    if (off >= len) {
        *cells = 0;
        return len;
    }
    uint32_t cp;
    off += (size_t)ui_text_run_next(text + off, len - off, &cp, cells);
    while (off < len && text[off] != '\n' && text[off] != '\r') {
        int width;
        int n = ui_text_run_next(text + off, len - off, &cp, &width);
        if (width != 0)
            break;
        off += (size_t)n;
    }
    return off;
}

static size_t previous_offset(const struct ui_label *label, size_t off)
{
    const char *text = ui_label_text(label);
    while (off > 0) {
        size_t prev = off - 1;
        while (prev > 0 && ((unsigned char)text[prev] & 0xC0) == 0x80)
            prev--;
        uint32_t cp;
        int cells;
        ui_text_run_next(text + prev, label->impl.len - prev, &cp, &cells);
        off = prev;
        if (cells != 0)
            break;
    }
    return off;
}

static bool next_line(struct label_iter *it, struct label_line *line)
{
    if (it->done)
        return false;
    const char *text = ui_label_text(it->label);
    size_t len = it->label->impl.len;
    if (!it->segment) {
        const char *nl = memchr(text + it->off, '\n', len - it->off);
        it->end = nl ? (size_t)(nl - text) : len;
        it->next = nl ? it->end + 1 : len;
        if (nl && it->end > it->off && text[it->end - 1] == '\r')
            it->end--;
        it->segment = true;
    }
    *line = (struct label_line){ .start = it->off };
    size_t pos = it->off, word_end = pos;
    int used = 0, word_cells = 0;
    while (pos < it->end) {
        int cells;
        size_t next = next_offset(text, it->end, pos, &cells);
        if (cells > it->cap - used)
            break;
        used += cells;
        if (text[pos] == ' ' || text[pos] == '-' || text[pos] == '\t') {
            word_end = next;
            word_cells = used;
        }
        pos = next;
    }
    enum ui_label_wrap wrap = it->label->impl.wrap;
    if (pos < it->end && wrap == UI_LABEL_WRAP_WORD && word_end > it->off) {
        pos = word_end;
        used = word_cells;
    }
    if (pos < it->end && wrap == UI_LABEL_ELLIPSIS && it->cap > 0) {
        pos = it->off;
        used = 0;
        while (pos < it->end) {
            int cells;
            size_t next = next_offset(text, it->end, pos, &cells);
            if (cells > it->cap - 1 - used)
                break;
            used += cells;
            pos = next;
        }
        line->ellipsis = true;
    } else if (pos == it->off && pos < it->end && wrap <= UI_LABEL_WRAP_CHAR) {
        pos = next_offset(text, it->end, pos, &used);
    }
    line->len = pos - it->off;
    line->cells = used;
    if (pos == it->end || wrap >= UI_LABEL_CLIP) {
        line->next = it->next;
        it->done = it->next == len && (it->end == len || text[len - 1] != '\n');
        it->off = it->next;
        it->segment = false;
    } else {
        line->next = pos;
        it->off = pos;
    }
    return true;
}

static struct rect2i content_rect(const struct ui_label *label, struct vector2i size)
{
    return rect2i_apply_insets((struct rect2i){ .size = size },
        ui_ctx_insets(&label->widget.style_scale, label->widget.style.padding, 0));
}

static struct label_iter line_iter(const struct ui_label *label, int width)
{
    struct ui_text_style style = ui_widget_text_style(&label->widget.style_scale);
    return (struct label_iter){
        .label = label,
        .cap = style.advance > 0 && width > 0 ? width / style.advance : 0,
    };
}

static struct vector2i label_measure(const struct ui_widget *widget,
                                     struct ui_constraints constraints)
{
    const struct ui_label *label = label_of(widget);
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    struct vector2i pad = insets2i_extent(ui_ctx_insets(
        &widget->style_scale, widget->style.padding, 0));
    int width = constraints.min.x == constraints.max.x ? constraints.max.x :
        ui_css_resolve_int(widget->style.width, (float)constraints.max.x,
            (float)constraints.max.x, &widget->style_scale, true);
    if (widget->impl.fixed_size.x)
        width = widget->impl.fixed_size.x;
    int content_w = geom_sat_i((int64_t)width - pad.x);
    struct label_iter it = line_iter(label, content_w);
    struct label_line line;
    int widest = 0;
    int64_t lines = 0;
    while (next_line(&it, &line)) {
        int cells = line.cells + (line.ellipsis ? 1 : 0);
        if (cells > widest) widest = cells;
        lines++;
    }
    return ui_constraints_clamp(constraints, (struct vector2i){
        .x = geom_sat_i((int64_t)widest * style.advance + pad.x),
        .y = geom_sat_i(lines * style.cell_h + pad.y),
    });
}

static struct rect2i line_rect(const struct ui_label *label, struct rect2i content,
                                const struct label_line *line, int row)
{
    struct ui_text_style style = ui_widget_text_style(&label->widget.style_scale);
    int width = geom_sat_i((int64_t)(line->cells + (line->ellipsis ? 1 : 0)) * style.advance);
    int free = content.width > width ? content.width - width : 0;
    int dx = label->impl.align == UI_LABEL_ALIGN_CENTER ? free / 2
           : label->impl.align == UI_LABEL_ALIGN_RIGHT ? free : 0;
    int dy = 0;
    struct label_iter it = line_iter(label, content.width);
    struct label_line first, second;
    if (row == 0 && next_line(&it, &first) && !next_line(&it, &second))
        dy = geom_sat_i(geom_half_up((int64_t)content.height - style.cell_h));
    return (struct rect2i){
        .pos = { .x = content.x + dx,
                 .y = geom_sat_i((int64_t)content.y + (int64_t)row * style.cell_h + dy) },
        .size = { .x = width, .y = style.cell_h },
    };
}

bool ui_label_selection(const struct ui_label *label, size_t *start, size_t *end)
{
    if (!label || label->impl.anchor == label->impl.caret)
        return false;
    if (start) *start = label->impl.anchor < label->impl.caret ? label->impl.anchor : label->impl.caret;
    if (end) *end = label->impl.anchor > label->impl.caret ? label->impl.anchor : label->impl.caret;
    return true;
}

static void paint_span(const struct ui_label *label, struct ui_paint_context *paint,
                         struct rect2i rect, size_t start, size_t end, uint32_t color)
{
    if (start >= end)
        return;
    struct ui_text_style style = ui_widget_text_style(&label->widget.style_scale);
    rect.width = geom_sat_i((int64_t)ui_text_run_cells(ui_label_text(label) + start,
                                                   end - start) * style.advance);
    ui_paint_text(paint, rect, style, color, ui_label_text(label) + start, end - start);
}

static void paint_line(const struct ui_label *label, struct ui_paint_context *paint,
                         struct rect2i rect, const struct label_line *line, uint32_t color)
{
    const char *text = ui_label_text(label);
    struct ui_text_style style = ui_widget_text_style(&label->widget.style_scale);
    size_t lo = 0, hi = 0, end = line->start + line->len;
    bool selected = ui_label_selection(label, &lo, &hi) && hi > line->start && lo < end;
    if (selected) {
        if (lo < line->start) lo = line->start;
        if (hi > end) hi = end;
        int before = geom_sat_i((int64_t)ui_text_run_cells(text + line->start, lo - line->start) * style.advance);
        int width = geom_sat_i((int64_t)ui_text_run_cells(text + lo, hi - lo) * style.advance);
        struct rect2i selection = rect;
        selection.x += before;
        selection.width = width;
        struct css_style sel = label_part(label, parts.selection);
        uint32_t bg = sel.has_bg_color ? sel.bg_color : 0xFF45475A;
        uint32_t fg = sel.has_text_color ? sel.text_color : color;
        ui_paint_rect(paint, selection, bg);
        paint_span(label, paint, rect, line->start, lo, color);
        paint_span(label, paint, selection, lo, hi, fg);
        rect.x += before + width;
        paint_span(label, paint, rect, hi, end, color);
        rect.x -= before + width;
    } else {
        paint_span(label, paint, rect, line->start, end, color);
    }
    if (line->ellipsis) {
        struct rect2i marker = rect;
        marker.x += geom_sat_i((int64_t)line->cells * style.advance);
        marker.width = style.advance;
        ui_paint_text(paint, marker, style, color, "\xE2\x80\xA6", 3);
    }
    if (label->impl.link && rect.width > 0) {
        struct css_style link = label_part(label, parts.link);
        int thickness = ui_css_resolve_int(link.border_width,
            0, 1, &label->widget.style_scale, true);
        thickness = geom_clamp_i(thickness, 0, rect.height);
        struct rect2i underline = rect;
        underline.y += rect.height - thickness;
        underline.height = thickness;
        if (thickness > 0)
            ui_paint_rect(paint, underline, color);
    }
}

static void label_paint(const struct ui_widget *widget, struct ui_paint_context *paint)
{
    const struct ui_label *label = label_of(widget);
    struct rect2i bounds = ui_paint_bounds(paint);
    if (widget->style.has_bg_color) {
        int radius = ui_css_resolve_int(widget->style.border_radius, 0, 0,
                                        &widget->style_scale, true);
        ui_paint_rounded_rect(paint, bounds, radius, widget->style.bg_color);
    }
    struct rect2i content = content_rect(label, bounds.size);
    struct ui_text_style style = ui_widget_text_style(&widget->style_scale);
    if (content.width <= 0 || content.height < style.cell_h || !label->impl.len)
        return;
    uint32_t color = widget->style.has_text_color ? widget->style.text_color : 0xFFFFFFFF;
    if (label->impl.link) {
        struct css_style link = label_part(label, parts.link);
        if (link.has_text_color)
            color = link.text_color;
    }
    if (!ui_paint_push_clip(paint, content))
        return;
    struct label_iter it = line_iter(label, content.width);
    struct label_line line;
    int row = 0;
    while (next_line(&it, &line)) {
        struct rect2i rect = line_rect(label, content, &line, row++);
        if ((int64_t)rect.y + rect.height > (int64_t)content.y + content.height)
            break;
        paint_line(label, paint, rect, &line, color);
    }
    ui_paint_pop_clip(paint);
    if (ui_widget_has_focus(widget) && !label->impl.disabled) {
        struct css_style fs = label_part(label, parts.focus);
        uint32_t focus = fs.has_border_color ? fs.border_color : 0xFF89B4FA;
        int edge = ui_css_resolve_int(fs.border_width, 0, 1,
                                      &widget->style_scale, true);
        edge = geom_clamp_i(edge, 0, bounds.height / 2);
        if (edge > 0) {
            ui_paint_rect(paint, (struct rect2i){ .pos = bounds.pos,
                .size = { .x = bounds.width, .y = edge } }, focus);
            ui_paint_rect(paint, (struct rect2i){
                .pos = { .x = bounds.x, .y = bounds.y + bounds.height - edge },
                .size = { .x = bounds.width, .y = edge } }, focus);
            ui_paint_rect(paint, (struct rect2i){ .pos = bounds.pos,
                .size = { .x = edge, .y = bounds.height } }, focus);
            ui_paint_rect(paint, (struct rect2i){
                .pos = { .x = bounds.x + bounds.width - edge, .y = bounds.y },
                .size = { .x = edge, .y = bounds.height } }, focus);
        }
    }
}

static void label_parts(const struct ui_widget *widget, struct ui_debug_parts *parts)
{
    const struct ui_label *label = label_of(widget);
    struct rect2i content = content_rect(label, widget->bounds.size);
    ui_debug_part(parts, "content", -1, ui_widget_rect_to_surface(widget, content));
    if (label->impl.selectable)
        ui_debug_part(parts, "selection-area", -1, widget->bounds);
    if (content.width <= 0 || content.height < ui_widget_text_style(&widget->style_scale).cell_h)
        return;
    struct label_iter it = line_iter(label, content.width);
    struct label_line line;
    int row = 0;
    while (next_line(&it, &line)) {
        struct rect2i rect = line_rect(label, content, &line, row);
        if ((int64_t)rect.y + rect.height > (int64_t)content.y + content.height)
            break;
        ui_debug_part(parts, "text-line", row++, ui_widget_rect_to_surface(widget,
            rect2i_intersect(rect, content)));
    }
}

static size_t offset_at(const struct ui_label *label, struct vector2i pos)
{
    struct rect2i content = content_rect(label, label->widget.bounds.size);
    struct ui_text_style style = ui_widget_text_style(&label->widget.style_scale);
    struct label_iter it = line_iter(label, content.width);
    struct label_line line;
    int row = 0;
    while (next_line(&it, &line)) {
        struct rect2i rect = line_rect(label, content, &line, row++);
        if (pos.y >= (int64_t)rect.y + rect.height)
            continue;
        size_t off = line.start, end = off + line.len;
        int64_t x = rect.x;
        while (off < end) {
            int cells;
            size_t next = next_offset(ui_label_text(label), end, off, &cells);
            int64_t width = (int64_t)cells * style.advance;
            if ((int64_t)pos.x < x + (width + 1) / 2)
                break;
            x += width;
            off = next;
        }
        return off;
    }
    return label->impl.len;
}

static bool text_hit(const struct ui_label *label, struct vector2i surface)
{
    struct vector2i pos = ui_widget_to_local(&label->widget, surface);
    struct rect2i content = content_rect(label, label->widget.bounds.size);
    struct label_iter it = line_iter(label, content.width);
    struct label_line line;
    int row = 0;
    while (next_line(&it, &line)) {
        struct rect2i rect = line_rect(label, content, &line, row++);
        if ((int64_t)rect.y + rect.height > (int64_t)content.y + content.height)
            break;
        if (rect2i_contains(rect2i_intersect(rect, content), pos))
            return true;
    }
    return false;
}

static bool label_hit_test(const struct ui_widget *widget, struct vector2i pos)
{
    const struct ui_label *label = label_of(widget);
    return label->impl.link && !label->impl.selectable
        ? text_hit(label, pos) : rect2i_contains(widget->bounds, pos);
}

static int word_class(const char *text, size_t len, size_t off)
{
    if (off >= len) return -1;
    uint32_t cp;
    int cells;
    ui_text_run_next(text + off, len - off, &cp, &cells);
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return 0;
    return cp >= 0x80 || (cp >= 'a' && cp <= 'z') ||
           (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_' ? 1 : 2;
}

static void selection_unit(const struct ui_label *label, size_t at, unsigned clicks,
                             size_t *lo, size_t *hi)
{
    const char *text = ui_label_text(label);
    size_t len = label->impl.len;
    if (at == len && at > 0)
        at = previous_offset(label, at);
    *lo = at;
    int cells;
    *hi = next_offset(text, len, at, &cells);
    if (clicks == 3) {
        while (*lo > 0 && text[*lo - 1] != '\n') (*lo)--;
        while (*hi < len && text[*hi] != '\n') (*hi)++;
        if (*hi < len) (*hi)++;
    } else {
        int kind = word_class(text, len, at);
        while (*lo > 0) {
            size_t prev = previous_offset(label, *lo);
            if (word_class(text, len, prev) != kind) break;
            *lo = prev;
        }
        while (*hi < len && word_class(text, len, *hi) == kind)
            *hi = next_offset(text, len, *hi, &cells);
    }
}

static void update_pseudo(struct ui_label *label)
{
    unsigned pseudo = label->widget.impl.style_pseudo & CSS_PSEUDO_HOVER;
    if (label->impl.pressed) pseudo |= CSS_PSEUDO_ACTIVE;
    if (label->impl.disabled) pseudo |= CSS_PSEUDO_DISABLED;
    if (ui_widget_has_focus(&label->widget)) pseudo |= CSS_PSEUDO_FOCUS;
    ui_widget_set_style_pseudo(&label->widget, pseudo);
}

static void activate(struct ui_label *label)
{
    bool handled = ui_widget_notify(&label->widget, (struct ui_action){
        .kind = UI_ACTION_ACTIVATE, .id = label->impl.action_id, .index = -1,
    });
    if (!handled && !label->widget.impl.destroyed && label->impl.link)
        ui_surface_open_url(ui_widget_surface(&label->widget), label->impl.link);
}

static bool label_accessibility(const struct ui_widget *widget,
                                struct ui_accessibility_node_spec *out)
{
    const struct ui_label *label = label_of(widget);
    const char *text = ui_label_text(label);
    out->role = label->impl.link ? UI_ACCESSIBILITY_ROLE_LINK
                                 : UI_ACCESSIBILITY_ROLE_LABEL;
    out->name = text[0] ? text : NULL;
    if (label->impl.disabled)
        out->states |= UI_ACCESSIBILITY_STATE_DISABLED;
    if (!label->impl.disabled && label->impl.link)
        out->actions |= UI_ACCESSIBILITY_ACTION_INVOKE;
    if (!label->impl.disabled && label->impl.selectable) {
        out->value = text;
        out->actions |= UI_ACCESSIBILITY_ACTION_SET_SELECTION;
        size_t start, end;
        if (ui_label_selection(label, &start, &end)) {
            out->selection_start = start;
            out->selection_end = end;
            out->caret_offset = label->impl.caret;
        } else {
            out->caret_offset = label->impl.caret;
        }
    }
    return true;
}

static size_t label_clamp_offset(const struct ui_label *label, size_t offset)
{
    if (offset > label->impl.len)
        offset = label->impl.len;
    const char *text = ui_label_text(label);
    while (offset > 0 && offset < label->impl.len &&
           ((unsigned char)text[offset] & 0xC0) == 0x80)
        offset--;
    return offset;
}

static bool label_accessibility_action(
    struct ui_widget *widget, uint64_t node_id,
    const struct ui_accessibility_action_request *request)
{
    struct ui_label *label = label_of(widget);
    if (node_id != ui_widget_accessibility_id(widget) ||
        label->impl.disabled)
        return false;
    if (request->action == UI_ACCESSIBILITY_ACTION_INVOKE &&
        label->impl.link) {
        activate(label);
        return true;
    }
    if (request->action == UI_ACCESSIBILITY_ACTION_SET_SELECTION &&
        label->impl.selectable) {
        if (request->selection_start == request->selection_end) {
            label->impl.caret = label_clamp_offset(label,
                                                    request->caret_offset);
            label->impl.anchor = label->impl.caret;
        } else {
            label->impl.anchor = label_clamp_offset(
                label, request->selection_start);
            label->impl.caret = label_clamp_offset(
                label, request->selection_end);
        }
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
        return true;
    }
    return false;
}

static bool label_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct ui_label *label = label_of(widget);
    if (event->kind == UI_EVENT_CAPTURE_LOST || event->kind == UI_EVENT_FOCUS_OUT) {
        label->impl.pressed = false;
        label->impl.clicks = 0;
        ui_widget_release_pointer(widget);
        if (event->kind == UI_EVENT_FOCUS_OUT)
            ui_label_clear_selection(label);
        update_pseudo(label);
        return false;
    }
    if (event->kind == UI_EVENT_FOCUS_IN) {
        update_pseudo(label);
        return true;
    }
    if (label->impl.disabled)
        return false;
    if (event->kind == UI_EVENT_SHORTCUT) {
        switch (event->data.shortcut.edit) {
        case UI_EDIT_COPY:
            ui_label_copy_selection(label);
            return true;
        case UI_EDIT_SELECT_ALL:
            ui_label_select_all(label);
            return true;
        default:
            return false;
        }
    }
    if (event->kind == UI_EVENT_KEY) {
        const struct ui_key_event *key = &event->data.key;
        bool ctrl = (key->mods & (UI_MOD_CTRL | UI_MOD_SUPER)) != 0;
        bool shift = (key->mods & UI_MOD_SHIFT) != 0;
        if (label->impl.selectable) {
            if (ctrl && key->key == UI_KEY_CHAR && (key->text == 'a' || key->text == 'A')) {
                ui_label_select_all(label);
                return true;
            }
            if (ctrl && key->key == UI_KEY_CHAR && (key->text == 'c' || key->text == 'C'))
                return ui_label_copy_selection(label);
            size_t at = label->impl.caret;
            int cells;
            switch (key->key) {
            case UI_KEY_LEFT:
            case UI_KEY_RIGHT: {
                size_t lo, hi;
                if (!shift && ui_label_selection(label, &lo, &hi)) {
                    at = key->key == UI_KEY_LEFT ? lo : hi;
                } else if (key->key == UI_KEY_LEFT) {
                    at = previous_offset(label, at);
                } else {
                    at = next_offset(ui_label_text(label), label->impl.len, at, &cells);
                }
                break;
            }
            case UI_KEY_HOME: at = 0; break;
            case UI_KEY_END: at = label->impl.len; break;
            case UI_KEY_ESCAPE: ui_label_clear_selection(label); return true;
            default: goto link_key;
            }
            label->impl.caret = at;
            if (!shift) label->impl.anchor = at;
            ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
            return true;
        }
link_key:
        if (label->impl.link && !ctrl && !(key->mods & UI_MOD_ALT) &&
            (key->key == UI_KEY_ENTER || (key->key == UI_KEY_CHAR && key->text == ' '))) {
            if (key->action == UI_KEY_PRESS)
                activate(label);
            return true;
        }
        return false;
    }
    if (event->kind != UI_EVENT_POINTER)
        return false;
    const struct ui_pointer_event *p = &event->data.pointer;
    if (p->kind == UI_POINTER_LEAVE || p->kind == UI_POINTER_MOTION) {
        unsigned pseudo = widget->impl.style_pseudo & ~CSS_PSEUDO_HOVER;
        if (p->kind == UI_POINTER_MOTION && text_hit(label, p->pos)) pseudo |= CSS_PSEUDO_HOVER;
        ui_widget_set_style_pseudo(widget, pseudo);
    }
    if (p->kind == UI_POINTER_PRESS) {
        if (p->button != 0 || (!label->impl.selectable && !label->impl.link) ||
            !rect2i_contains(widget->bounds, p->pos) ||
            (!label->impl.selectable && !text_hit(label, p->pos)))
            return false;
        ui_widget_focus(widget);
        ui_widget_capture_pointer(widget);
        struct vector2i delta = vector2i_sub(p->pos, label->impl.click_pos);
        int distance = ui_px(widget->style_scale.dpi_scale, 4);
        bool repeated = label->impl.clicks && event->time_ms >= label->impl.click_time &&
            event->time_ms - label->impl.click_time <= 400 &&
            llabs((long long)delta.x) <= distance && llabs((long long)delta.y) <= distance;
        label->impl.clicks = repeated ? label->impl.clicks % 3 + 1 : 1;
        label->impl.click_time = event->time_ms;
        label->impl.click_pos = label->impl.press_pos = p->pos;
        label->impl.pressed = true;
        label->impl.dragged = false;
        if (label->impl.selectable) {
            size_t at = offset_at(label, ui_widget_to_local(widget, p->pos));
            if (label->impl.clicks > 1) {
                selection_unit(label, at, label->impl.clicks,
                    &label->impl.gesture_start, &label->impl.gesture_end);
                label->impl.anchor = label->impl.gesture_start;
                label->impl.caret = label->impl.gesture_end;
            } else {
                label->impl.caret = at;
                if (!(p->mods & UI_MOD_SHIFT)) label->impl.anchor = at;
            }
        }
        update_pseudo(label);
        ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
        return true;
    }
    if (p->kind == UI_POINTER_MOTION && label->impl.pressed) {
        struct vector2i delta = vector2i_sub(p->pos, label->impl.press_pos);
        int distance = ui_px(widget->style_scale.dpi_scale, 4);
        if (llabs((long long)delta.x) > distance || llabs((long long)delta.y) > distance)
            label->impl.dragged = true;
        if (label->impl.selectable && label->impl.dragged) {
            size_t at = offset_at(label, ui_widget_to_local(widget, p->pos));
            if (label->impl.clicks > 1) {
                size_t lo, hi;
                selection_unit(label, at, label->impl.clicks, &lo, &hi);
                label->impl.anchor = lo < label->impl.gesture_start
                    ? label->impl.gesture_end : label->impl.gesture_start;
                label->impl.caret = lo < label->impl.gesture_start ? lo
                    : hi > label->impl.gesture_end ? hi : label->impl.gesture_end;
            } else {
                label->impl.caret = at;
            }
            ui_widget_invalidate(widget, UI_WIDGET_INVALIDATE_PAINT);
        }
        return true;
    }
    if (p->kind == UI_POINTER_RELEASE && p->button == 0 && label->impl.pressed) {
        bool open = label->impl.link && !label->impl.dragged && label->impl.clicks == 1 &&
                    !ui_label_selection(label, NULL, NULL) && text_hit(label, p->pos);
        label->impl.pressed = false;
        ui_widget_release_pointer(widget);
        update_pseudo(label);
        if (open) activate(label);
        return true;
    }
    return false;
}

static enum ui_cursor label_cursor(const struct ui_widget *widget, struct vector2i pos)
{
    const struct ui_label *label = label_of(widget);
    if (widget->style.has_cursor && widget->style.cursor != UI_CURSOR_AUTO)
        return widget->style.cursor;
    if (label->impl.disabled) return UI_CURSOR_DEFAULT;
    if (label->impl.link && text_hit(label, pos)) return UI_CURSOR_POINTER;
    return label->impl.selectable ? UI_CURSOR_TEXT : UI_CURSOR_AUTO;
}

static void label_destroy(struct ui_widget *widget)
{
    struct ui_label *label = label_of(widget);
    free(label->impl.text);
    free(label->impl.link);
    label->impl.text = label->impl.link = NULL;
    label->impl.len = label->impl.anchor = label->impl.caret = 0;
}

static const struct ui_widget_ops LABEL_OPS = {
    .name = "label",
    .width_sensitive = true,
    .measure = label_measure,
    .event = label_event,
    .hit_test = label_hit_test,
    .paint = label_paint,
    .cursor = label_cursor,
    .debug_parts = label_parts,
    .destroy = label_destroy,
    .accessibility = label_accessibility,
    .accessibility_action = label_accessibility_action,
};

static void label_ensure_init(struct ui_label *label)
{
    if (!label || label->widget.ops)
        return;
    label_parts_init();
    ui_widget_init(&label->widget, &LABEL_OPS, NULL);
    ui_widget_set_style_identity(&label->widget, "label", NULL, "label");
}

void ui_label_init(struct ui_label *label)
{
    if (!label) return;
    *label = (struct ui_label){0};
    label_ensure_init(label);
}

bool ui_label_set_text(struct ui_label *label, const char *text)
{
    if (!label) return false;
    label_ensure_init(label);
    if (!text) text = "";
    size_t len = strlen(text);
    if (len > INT_MAX) return false;
    if (len == label->impl.len && !memcmp(ui_label_text(label), text, len)) return true;
    char *copy = len ? strdup(text) : NULL;
    if (len && !copy) return false;
    free(label->impl.text);
    label->impl.text = copy;
    label->impl.len = len;
    label->impl.anchor = label->impl.caret = 0;
    label->impl.clicks = 0;
    label->impl.pressed = false;
    ui_widget_release_pointer(&label->widget);
    update_pseudo(label);
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_MEASURE);
    return true;
}

const char *ui_label_text(const struct ui_label *label)
{
    return label && label->impl.text ? label->impl.text : "";
}

void ui_label_set_wrap(struct ui_label *label, enum ui_label_wrap wrap)
{
    if (!label || wrap < UI_LABEL_WRAP_WORD || wrap > UI_LABEL_ELLIPSIS) return;
    label_ensure_init(label);
    if (label->impl.wrap == wrap) return;
    label->impl.wrap = wrap;
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_label_set_align(struct ui_label *label, enum ui_label_align align)
{
    if (!label || align < UI_LABEL_ALIGN_LEFT || align > UI_LABEL_ALIGN_RIGHT) return;
    label_ensure_init(label);
    if (label->impl.align == align) return;
    label->impl.align = align;
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_PAINT);
}

static void update_focusable(struct ui_label *label)
{
    ui_widget_set_focusable(&label->widget,
        !label->impl.disabled && (label->impl.selectable || label->impl.link));
}

void ui_label_set_selectable(struct ui_label *label, bool selectable)
{
    if (!label) return;
    label_ensure_init(label);
    label->impl.selectable = selectable;
    label->impl.pressed = false;
    label->impl.clicks = 0;
    ui_widget_release_pointer(&label->widget);
    if (!selectable) ui_label_clear_selection(label);
    update_focusable(label);
    update_pseudo(label);
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_PAINT);
}

void ui_label_select_all(struct ui_label *label)
{
    if (!label || !label->impl.selectable) return;
    label->impl.anchor = 0;
    label->impl.caret = label->impl.len;
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_PAINT);
}

void ui_label_clear_selection(struct ui_label *label)
{
    if (!label) return;
    label->impl.anchor = label->impl.caret = 0;
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_PAINT);
}

bool ui_label_copy_selection(struct ui_label *label)
{
    size_t lo, hi;
    if (!ui_label_selection(label, &lo, &hi)) return false;
    char *copy = malloc(hi - lo + 1);
    if (!copy) return false;
    memcpy(copy, ui_label_text(label) + lo, hi - lo);
    copy[hi - lo] = '\0';
    bool copied = ui_surface_copy_text(ui_widget_surface(&label->widget), copy);
    free(copy);
    return copied;
}

bool ui_label_set_link(struct ui_label *label, const char *target)
{
    if (!label) return false;
    label_ensure_init(label);
    if (!target) target = "";
    if (!strcmp(ui_label_link(label), target)) return true;
    char *copy = *target ? strdup(target) : NULL;
    if (*target && !copy) return false;
    free(label->impl.link);
    label->impl.link = copy;
    label->impl.pressed = false;
    label->impl.clicks = 0;
    ui_widget_release_pointer(&label->widget);
    update_focusable(label);
    update_pseudo(label);
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_PAINT);
    return true;
}

const char *ui_label_link(const struct ui_label *label)
{
    return label && label->impl.link ? label->impl.link : "";
}

void ui_label_set_action_id(struct ui_label *label, int id)
{
    if (!label) return;
    label_ensure_init(label);
    label->impl.action_id = id;
}

void ui_label_set_disabled(struct ui_label *label, bool disabled)
{
    if (!label) return;
    label_ensure_init(label);
    label->impl.disabled = disabled;
    label->impl.pressed = false;
    label->impl.clicks = 0;
    ui_widget_release_pointer(&label->widget);
    update_focusable(label);
    update_pseudo(label);
    ui_widget_invalidate(&label->widget, UI_WIDGET_INVALIDATE_PAINT);
}
