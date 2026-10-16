#include "ui/debug.h"

#include "ui/internal/tree.h"
#include "ui/surface.h"
#include "ui/style_context.h"
#include "ui/ui_util.h"
#include "util/grow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parts --------------------------------------------------------- */

void ui_debug_part_text(struct ui_debug_parts *out, const char *name,
                        int index, struct rect2i rect,
                        const char *text, size_t len)
{
    if (!out || !UTIL_GROW(out->items, out->cap, out->n + 1))
        return;
    struct ui_debug_part *p = &out->items[out->n++];
    *p = (struct ui_debug_part){ .name = name, .index = index, .rect = rect };
    if (text && len)
        p->text = strndup(text, len);
}

static void parts_clear(struct ui_debug_parts *parts)
{
    for (int i = 0; i < parts->n; i++)
        free(parts->items[i].text);
    parts->n = 0;
}

void ui_debug_parts_free(struct ui_debug_parts *parts)
{
    if (!parts)
        return;
    parts_clear(parts);
    free(parts->items);
    *parts = (struct ui_debug_parts){0};
}

void ui_debug_part(struct ui_debug_parts *out, const char *name, int index,
                   struct rect2i rect)
{
    ui_debug_part_text(out, name, index, rect, NULL, 0);
}

void ui_debug_collect_parts(const struct ui_widget *widget,
                            struct ui_debug_parts *out)
{
    if (!out)
        return;
    parts_clear(out);
    if (widget && !widget->impl.destroyed && widget->ops &&
        widget->ops->debug_parts)
        widget->ops->debug_parts(widget, out);
}

/* ---- tree navigation ---------------------------------------------- */

const char *ui_debug_type_name(const struct ui_widget *widget)
{
    return widget && widget->ops && widget->ops->name
        ? widget->ops->name : "widget";
}

/* Membership by address only: never dereferences candidate, so it is
 * safe on pointers that may be stale (scene scope owners, selections). */
static bool in_tree(const struct ui_widget *root,
                    const struct ui_widget *candidate)
{
    if (!root || !candidate)
        return false;
    if (root == candidate)
        return true;
    for (const struct ui_widget *c = root->first_child; c; c = c->next_sibling)
        if (in_tree(c, candidate))
            return true;
    return false;
}

static int child_index(const struct ui_widget *widget)
{
    int i = 0;
    for (const struct ui_widget *s = widget->prev_sibling; s; s = s->prev_sibling)
        i++;
    return i;
}

bool ui_debug_path(const struct ui_widget *root,
                   const struct ui_widget *widget, char *buf, size_t cap)
{
    if (!buf || cap < 2 || !in_tree(root, widget))
        return false;
    int idx[64];
    int n = 0;
    for (const struct ui_widget *w = widget; w && w != root; w = w->parent) {
        if (n >= (int)(sizeof(idx) / sizeof(idx[0])))
            return false;
        idx[n++] = child_index(w);
    }
    size_t used = 0;
    if (n == 0) {
        buf[0] = '/';
        buf[1] = '\0';
        return true;
    }
    while (n--) {
        int k = snprintf(buf + used, cap - used, "/%d", idx[n]);
        if (k < 0 || (size_t)k >= cap - used)
            return false;
        used += (size_t)k;
    }
    return true;
}

static const struct ui_widget *find_by(const struct ui_widget *w,
                                       char kind, const char *name)
{
    if (!w || w->impl.destroyed)
        return NULL;
    const char *have = kind == '.' ? w->impl.style_class
                     : kind == '#' ? w->impl.style_id
                     : ui_debug_type_name(w);
    if (have && !strcmp(have, name))
        return w;
    for (const struct ui_widget *c = w->first_child; c; c = c->next_sibling) {
        const struct ui_widget *hit = find_by(c, kind, name);
        if (hit)
            return hit;
    }
    return NULL;
}

const struct ui_widget *ui_debug_find(const struct ui_widget *root,
                                      const char *selector)
{
    if (!root || !selector || !*selector)
        return NULL;
    if (selector[0] == '/') {
        const struct ui_widget *w = root;
        const char *p = selector + 1;
        while (*p) {
            char *end;
            long i = strtol(p, &end, 10);
            if (end == p || i < 0)
                return NULL;
            const struct ui_widget *c = w->first_child;
            while (c && i--)
                c = c->next_sibling;
            if (!c)
                return NULL;
            w = c;
            p = *end == '/' ? end + 1 : end;
            if (*end && *end != '/')
                return NULL;
        }
        return w;
    }
    if (selector[0] == '0' && (selector[1] == 'x' || selector[1] == 'X')) {
        char *end;
        unsigned long long addr = strtoull(selector, &end, 16);
        if (*end)
            return NULL;
        const struct ui_widget *w =
            (const struct ui_widget *)(uintptr_t)addr;
        return in_tree(root, w) ? w : NULL;
    }
    if (selector[0] == '.' || selector[0] == '#')
        return find_by(root, selector[0], selector + 1);
    return find_by(root, 0, selector);
}

/* "content" describes the box model, not a hit region. */
static bool part_is_region(const struct ui_debug_part *part)
{
    return strcmp(part->name, "content") != 0;
}

static bool part_hit(const struct ui_widget *w, struct vector2i p,
                     bool *has_parts)
{
    static struct ui_debug_parts parts;
    ui_debug_collect_parts(w, &parts);
    *has_parts = false;
    for (int i = 0; i < parts.n; i++) {
        if (!part_is_region(&parts.items[i]))
            continue;
        *has_parts = true;
        if (rect2i_contains(parts.items[i].rect, p))
            return true;
    }
    return false;
}

/* A widget that reports parts occupies only its parts; one that does not
 * occupies its bounds. Tabbar paints into a window-sized canvas, so its
 * bounds alone would claim the whole window. */
static bool occupies(const struct ui_widget *w, struct vector2i p)
{
    if (!rect2i_contains(w->bounds, p))
        return false;
    bool has_parts;
    return part_hit(w, p, &has_parts) || !has_parts;
}

const struct ui_widget *ui_debug_widget_at(const struct ui_widget *root,
                                          struct vector2i p)
{
    if (!root || root->impl.destroyed || root->impl.hidden ||
        root->impl.pointer_transparent)
        return NULL;
    if (!root->ops || !root->ops->clip_children ||
        rect2i_contains(root->bounds, p)) {
        for (const struct ui_widget *c = root->last_child; c; c = c->prev_sibling) {
            const struct ui_widget *hit = ui_debug_widget_at(c, p);
            if (hit)
                return hit;
        }
    }
    return occupies(root, p) ? root : NULL;
}

/* ---- scene helpers ------------------------------------------------- */

/* Scopes are in paint order, so the last node scope that holds the op is
 * the innermost one. */
const struct ui_widget *ui_debug_op_owner(const struct ui_debug_view *view,
                                          int op_index)
{
    const struct ui_scene *s = view ? view->scene : NULL;
    if (!s)
        return NULL;
    for (int i = s->n_scopes - 1; i >= 0; i--) {
        const struct ui_scene_scope *sc = &s->scopes[i];
        if (sc->owner && op_index >= sc->first_op && op_index < sc->end_op)
            return in_tree(view->root, sc->owner) ? sc->owner : NULL;
    }
    return NULL;
}

static bool owned_by(const struct ui_debug_view *view, int op_index,
                     const struct ui_widget *filter)
{
    if (!filter)
        return true;
    for (const struct ui_widget *w = ui_debug_op_owner(view, op_index);
         w; w = w->parent)
        if (w == filter)
            return true;
    return false;
}

struct rect2i ui_debug_text_box(const struct ui_scene *scene,
                                const struct ui_scene_op *op)
{
    return ui_scene_text_box(scene, op);
}

static const char *op_kind_name(enum ui_scene_op_kind kind)
{
    switch (kind) {
    case UI_SCENE_OP_PUSH_CLIP:    return "push_clip";
    case UI_SCENE_OP_POP_CLIP:     return "pop_clip";
    case UI_SCENE_OP_RECT:         return "rect";
    case UI_SCENE_OP_BLEND_RECT:   return "blend_rect";
    case UI_SCENE_OP_ROUNDED_RECT: return "rounded_rect";
    case UI_SCENE_OP_CIRCLE:       return "circle";
    case UI_SCENE_OP_SHADOW:       return "shadow";
    case UI_SCENE_OP_IMAGE:        return "image";
    case UI_SCENE_OP_TEXT:         return "text";
    }
    return "?";
}

static bool op_is_fill(const struct ui_scene_op *op)
{
    return op->kind == UI_SCENE_OP_RECT ||
           op->kind == UI_SCENE_OP_ROUNDED_RECT ||
           (op->kind == UI_SCENE_OP_BLEND_RECT && op->alpha > 0);
}

/* ---- JSON: widgets ------------------------------------------------- */

static void write_color(struct json_writer *j, uint32_t argb)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "#%08x", argb);
    json_str(j, buf);
}

static void write_path(struct json_writer *j, const struct ui_debug_view *view,
                       const struct ui_widget *w)
{
    char path[256];
    if (ui_debug_path(view->root, w, path, sizeof(path)))
        json_str(j, path);
    else
        json_null(j);
}

static void write_ref(struct json_writer *j, const struct ui_debug_view *view,
                      const struct ui_widget *w)
{
    if (!w) {
        json_null(j);
        return;
    }
    json_obj_begin(j);
    json_key(j, "type"); json_str(j, ui_debug_type_name(w));
    json_key(j, "path"); write_path(j, view, w);
    json_key(j, "addr"); json_hex(j, (uintptr_t)w);
    json_obj_end(j);
}

static void op_range(const struct ui_debug_view *view,
                     const struct ui_widget *w, int *first, int *end)
{
    *first = *end = -1;
    const struct ui_scene *s = view->scene;
    if (!s)
        return;
    for (int i = 0; i < s->n_scopes; i++) {
        if (s->scopes[i].owner == w) {
            *first = s->scopes[i].first_op;
            *end = s->scopes[i].end_op;
            return;
        }
    }
}

static void write_widget_head(struct json_writer *j,
                              const struct ui_debug_view *view,
                              const struct ui_widget *w)
{
    json_key(j, "type"); json_str(j, ui_debug_type_name(w));
    json_key(j, "path"); write_path(j, view, w);
    json_key(j, "addr"); json_hex(j, (uintptr_t)w);
    if (w->impl.style_class) { json_key(j, "class"); json_str(j, w->impl.style_class); }
    if (w->impl.style_id)    { json_key(j, "id");    json_str(j, w->impl.style_id); }
    if (w->impl.style_tag)   { json_key(j, "tag");   json_str(j, w->impl.style_tag); }
    json_key(j, "bounds"); json_rect(j, w->bounds);
    json_key(j, "layout_bounds");
    json_obj_begin(j);
    json_key(j, "x"); json_double(j, w->layout_bounds.pos.x);
    json_key(j, "y"); json_double(j, w->layout_bounds.pos.y);
    json_key(j, "width"); json_double(j, w->layout_bounds.size.x);
    json_key(j, "height"); json_double(j, w->layout_bounds.size.y);
    json_obj_end(j);
    json_key(j, "measured"); json_vec(j, w->measured_size);

    json_key(j, "flags");
    json_arr_begin(j);
    const struct ui_surface *surface = view->surface;
    if (surface && ui_surface_focused(surface) == w) json_str(j, "focused");
    if (surface && ui_surface_captured(surface) == w) json_str(j, "captured");
    if (w->impl.focusable) json_str(j, "can_focus");
    if (w->impl.hidden) json_str(j, "hidden");
    if (w->impl.destroyed) json_str(j, "destroyed");
    if (!w->impl.style_valid) json_str(j, "style_stale");
    if (w->ops && w->ops->clip_children) json_str(j, "clip_children");
    if (w->impl.modal) json_str(j, "modal");
    if (w->impl.pointer_transparent) json_str(j, "pointer_transparent");
    if (surface && w->impl.surface != surface) json_str(j, "foreign_surface");
    json_arr_end(j);

    json_key(j, "dirty");
    json_arr_begin(j);
    if (w->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE) json_str(j, "measure");
    if (w->impl.invalidation & UI_WIDGET_INVALIDATE_LAYOUT) json_str(j, "layout");
    if (w->impl.invalidation & UI_WIDGET_INVALIDATE_PAINT) json_str(j, "paint");
    json_arr_end(j);

    int first, end;
    op_range(view, w, &first, &end);
    if (first >= 0) {
        json_key(j, "ops");
        json_arr_begin(j);
        json_int(j, first);
        json_int(j, end);
        json_arr_end(j);
    }
}

static void write_tree_node(struct json_writer *j,
                            const struct ui_debug_view *view,
                            const struct ui_widget *w, int depth,
                            int max_depth)
{
    json_obj_begin(j);
    write_widget_head(j, view, w);
    if (w->first_child) {
        json_key(j, "children");
        if (max_depth >= 0 && depth >= max_depth) {
            json_int(j, (int64_t)w->impl.child_count);
        } else {
            json_arr_begin(j);
            for (const struct ui_widget *c = w->first_child; c;
                 c = c->next_sibling)
                write_tree_node(j, view, c, depth + 1, max_depth);
            json_arr_end(j);
        }
    }
    json_obj_end(j);
}

void ui_debug_write_tree(struct json_writer *j,
                         const struct ui_debug_view *view,
                         const struct ui_widget *start, int max_depth)
{
    if (!start && view)
        start = view->root;
    if (!view || !start || !in_tree(view->root, start)) {
        json_null(j);
        return;
    }
    write_tree_node(j, view, start, 0, max_depth);
}

static void write_css_value(struct json_writer *j, struct css_value v,
                            float basis, const struct css_scale_context *scale)
{
    if (v.unit == CSS_UNIT_GROW) {
        json_str(j, "grow");
        return;
    }
    if (v.unit == CSS_UNIT_AUTO) {
        json_str(j, "auto");
        return;
    }
    json_int(j, ui_css_resolve_int(v, basis, 0.0f, scale, false));
}

static void write_style(struct json_writer *j, const struct ui_widget *w)
{
    const struct css_style *s = &w->style;
    const struct css_scale_context *scale = &w->style_scale;
    struct vector2i basis = w->parent ? w->parent->bounds.size : w->bounds.size;

    json_obj_begin(j);
    json_key(j, "scale");
    json_obj_begin(j);
    json_key(j, "dpi"); json_double(j, scale->dpi_scale);
    json_key(j, "cell_w"); json_double(j, scale->cell_w);
    json_key(j, "cell_h"); json_double(j, scale->cell_h);
    json_obj_end(j);
    json_key(j, "width"); write_css_value(j, s->width, (float)basis.x, scale);
    json_key(j, "height"); write_css_value(j, s->height, (float)basis.y, scale);
    json_key(j, "min_width"); write_css_value(j, s->min_width, (float)basis.x, scale);
    json_key(j, "min_height"); write_css_value(j, s->min_height, (float)basis.y, scale);
    json_key(j, "max_width"); write_css_value(j, s->max_width, (float)basis.x, scale);
    json_key(j, "max_height"); write_css_value(j, s->max_height, (float)basis.y, scale);
    json_key(j, "padding");
    json_insets(j, ui_css_resolve_insets(s->padding, w->bounds.size, scale, false));
    json_key(j, "margin");
    json_insets(j, ui_css_resolve_insets(s->margin, basis, scale, false));
    json_key(j, "gap"); write_css_value(j, s->gap, (float)w->bounds.width, scale);
    if (s->has_flex_direction) {
        json_key(j, "flex_direction");
        json_str(j, s->flex_direction == CSS_FLEX_COLUMN ? "column" : "row");
    }
    if (s->has_align_items) {
        static const char *const names[] = { "stretch", "start", "center", "end" };
        json_key(j, "align_items");
        json_str(j, (unsigned)s->align_items < 4 ? names[s->align_items] : "?");
    }
    if (s->has_justify_content) {
        static const char *const names[] = {
            "start", "center", "end",
        };
        json_key(j, "justify_content");
        json_str(j, (unsigned)s->justify_content < 3
                 ? names[s->justify_content] : "?");
    }
    if (s->has_bg_color) { json_key(j, "background"); write_color(j, s->bg_color); }
    if (s->has_text_color) { json_key(j, "color"); write_color(j, s->text_color); }
    if (s->has_border_color) { json_key(j, "border_color"); write_color(j, s->border_color); }
    json_key(j, "border_width"); write_css_value(j, s->border_width, 0, scale);
    json_key(j, "border_radius"); write_css_value(j, s->border_radius, 0, scale);
    if (s->has_opacity) { json_key(j, "opacity"); json_double(j, s->opacity); }
    json_obj_end(j);
}

static void write_parts(struct json_writer *j, const struct ui_debug_parts *parts)
{
    json_arr_begin(j);
    for (int i = 0; i < parts->n; i++) {
        const struct ui_debug_part *p = &parts->items[i];
        json_obj_begin(j);
        json_key(j, "name"); json_str(j, p->name);
        if (p->index >= 0) { json_key(j, "index"); json_int(j, p->index); }
        json_key(j, "rect"); json_rect(j, p->rect);
        if (p->text) { json_key(j, "text"); json_str(j, p->text); }
        json_obj_end(j);
    }
    json_arr_end(j);
}

static void write_op(struct json_writer *j, const struct ui_debug_view *view,
                     int index, bool with_owner)
{
    const struct ui_scene *s = view->scene;
    const struct ui_scene_op *op = &s->ops[index];
    json_obj_begin(j);
    json_key(j, "i"); json_int(j, index);
    json_key(j, "kind"); json_str(j, op_kind_name(op->kind));
    json_key(j, "rect"); json_rect(j, op->rect);
    switch (op->kind) {
    case UI_SCENE_OP_PUSH_CLIP:
    case UI_SCENE_OP_POP_CLIP:
        break;
    case UI_SCENE_OP_TEXT: {
        const char *text = ui_scene_op_text(s, op);
        json_key(j, "text"); json_strn(j, text, text ? op->text_len : 0);
        json_key(j, "box"); json_rect(j, ui_debug_text_box(s, op));
        json_key(j, "advance"); json_int(j, op->style.advance);
        json_key(j, "cell_h"); json_int(j, op->style.cell_h);
        if (op->style.flags) { json_key(j, "flags"); json_int(j, op->style.flags); }
        json_key(j, "color"); write_color(j, op->color);
        break;
    }
    default:
        json_key(j, "color"); write_color(j, op->color);
        if (op->kind == UI_SCENE_OP_BLEND_RECT || op->kind == UI_SCENE_OP_CIRCLE) {
            json_key(j, "alpha"); json_int(j, op->alpha);
        }
        if (op->radius) { json_key(j, "radius"); json_int(j, op->radius); }
        if (op->kind == UI_SCENE_OP_SHADOW) { json_key(j, "blur"); json_int(j, op->blur); }
        break;
    }
    if (with_owner) {
        json_key(j, "owner");
        write_ref(j, view, ui_debug_op_owner(view, index));
    }
    json_obj_end(j);
}

void ui_debug_write_widget(struct json_writer *j,
                           const struct ui_debug_view *view,
                           const struct ui_widget *w)
{
    if (!view || !w || !in_tree(view->root, w)) {
        json_null(j);
        return;
    }
    json_obj_begin(j);
    write_widget_head(j, view, w);
    json_key(j, "parent"); write_ref(j, view, w == view->root ? NULL : w->parent);
    json_key(j, "child_count"); json_int(j, (int64_t)w->impl.child_count);
    json_key(j, "style"); write_style(j, w);

    static struct ui_debug_parts parts;
    ui_debug_collect_parts(w, &parts);
    json_key(j, "parts"); write_parts(j, &parts);

    json_key(j, "text");
    json_arr_begin(j);
    if (view->scene) {
        for (int i = 0; i < view->scene->n_ops; i++)
            if (view->scene->ops[i].kind == UI_SCENE_OP_TEXT &&
                owned_by(view, i, w))
                write_op(j, view, i, false);
    }
    json_arr_end(j);
    json_obj_end(j);
}

void ui_debug_write_hit(struct json_writer *j,
                        const struct ui_debug_view *view, struct vector2i p)
{
    if (!view || !view->root) {
        json_null(j);
        return;
    }
    const struct ui_widget *target = ui_debug_widget_at(view->root, p);
    json_obj_begin(j);
    json_key(j, "point"); json_vec(j, p);
    json_key(j, "target"); write_ref(j, view, target);

    json_key(j, "stack");
    json_arr_begin(j);
    const struct ui_widget *chain[64];
    int n = 0;
    for (const struct ui_widget *w = target; w && n < 64; w = w->parent) {
        chain[n++] = w;
        if (w == view->root)
            break;
    }
    while (n--) {
        const struct ui_widget *w = chain[n];
        json_obj_begin(j);
        json_key(j, "type"); json_str(j, ui_debug_type_name(w));
        json_key(j, "path"); write_path(j, view, w);
        json_key(j, "bounds"); json_rect(j, w->bounds);
        static struct ui_debug_parts parts;
        ui_debug_collect_parts(w, &parts);
        json_key(j, "parts");
        json_arr_begin(j);
        for (int i = 0; i < parts.n; i++) {
            const struct ui_debug_part *part = &parts.items[i];
            if (!rect2i_contains(part->rect, p))
                continue;
            json_obj_begin(j);
            json_key(j, "name"); json_str(j, part->name);
            if (part->index >= 0) { json_key(j, "index"); json_int(j, part->index); }
            json_key(j, "rect"); json_rect(j, part->rect);
            json_obj_end(j);
        }
        json_arr_end(j);
        json_obj_end(j);
    }
    json_arr_end(j);

    /* Topmost first: the op the user actually sees at p. */
    json_key(j, "ops");
    json_arr_begin(j);
    if (view->scene) {
        int shown = 0;
        for (int i = view->scene->n_ops - 1; i >= 0 && shown < 8; i--) {
            const struct ui_scene_op *op = &view->scene->ops[i];
            if (op->kind == UI_SCENE_OP_PUSH_CLIP ||
                op->kind == UI_SCENE_OP_POP_CLIP)
                continue;
            struct rect2i r = op->kind == UI_SCENE_OP_TEXT
                ? ui_debug_text_box(view->scene, op) : op->rect;
            if (!rect2i_contains(r, p))
                continue;
            write_op(j, view, i, true);
            shown++;
        }
    }
    json_arr_end(j);
    json_obj_end(j);
}

void ui_debug_write_scene(struct json_writer *j,
                          const struct ui_debug_view *view,
                          const struct ui_widget *filter)
{
    const struct ui_scene *s = view ? view->scene : NULL;
    if (!s) {
        json_null(j);
        return;
    }
    json_obj_begin(j);
    json_key(j, "valid"); json_bool(j, ui_scene_is_valid(s));
    json_key(j, "n_ops"); json_int(j, s->n_ops);
    json_key(j, "n_scopes"); json_int(j, s->n_scopes);
    json_key(j, "text_len"); json_uint(j, s->text_len);
    json_key(j, "overflow"); json_bool(j, s->overflow);
    json_key(j, "bounds"); json_rect(j, ui_scene_bounds(s));
    json_key(j, "ops");
    json_arr_begin(j);
    for (int i = 0; i < s->n_ops; i++)
        if (owned_by(view, i, filter))
            write_op(j, view, i, true);
    json_arr_end(j);
    json_obj_end(j);
}

/* ---- lint ---------------------------------------------------------- */

/* Where the text starts, at mid height: overflowing text still starts
 * on the surface it overflows. */
static bool rect_contains_text_start(struct rect2i r, struct rect2i box)
{
    struct vector2i c = {
        .x = box.x,
        .y = geom_sat_i((int64_t)box.y + box.height / 2),
    };
    return rect2i_contains(r, c);
}

/* The rect a text op sits on. Parts win (they are the widget's own row
 * and slot geometry); then the nearest earlier fill that is row-sized;
 * then the owner's bounds when row-sized. Row-sized means at most three
 * cells tall, so a panel background never counts as a row. */
static bool text_surface(const struct ui_debug_view *view, int index,
                         const struct ui_widget *owner,
                         const struct ui_debug_parts *parts,
                         struct rect2i box, struct rect2i *out)
{
    const struct ui_scene *s = view->scene;
    int row_max = box.height * 3;
    bool found = false;
    for (int i = 0; parts && i < parts->n; i++) {
        struct rect2i r = parts->items[i].rect;
        if (r.height < box.height || r.height > row_max ||
            !rect_contains_text_start(r, box))
            continue;
        if (!found || (int64_t)r.width * r.height <
                      (int64_t)out->width * out->height) {
            *out = r;
            found = true;
        }
    }
    if (found)
        return true;
    for (int i = index - 1; i >= 0; i--) {
        const struct ui_scene_op *op = &s->ops[i];
        if (!op_is_fill(op) || op->rect.height < box.height ||
            op->rect.height > row_max || !rect_contains_text_start(op->rect, box))
            continue;
        if (owner && ui_debug_op_owner(view, i) != owner)
            continue;
        *out = op->rect;
        return true;
    }
    if (owner && owner->bounds.height >= box.height &&
        owner->bounds.height <= row_max &&
        rect_contains_text_start(owner->bounds, box)) {
        *out = owner->bounds;
        return true;
    }
    return false;
}

static int push_lint(struct ui_debug_lint *out, int cap, int n,
                     struct ui_debug_lint finding)
{
    if (out && n < cap)
        out[n] = finding;
    return n + 1;
}

int ui_debug_lint_scene(const struct ui_debug_view *view,
                        struct ui_debug_lint *out, int cap)
{
    if (!view || !view->scene)
        return 0;
    const struct ui_scene *s = view->scene;
    static struct ui_debug_parts parts;
    const struct ui_widget *parts_owner = NULL;
    bool parts_valid = false;
    int n = 0;

    for (int i = 0; i < s->n_ops; i++) {
        const struct ui_scene_op *op = &s->ops[i];
        if (op->kind != UI_SCENE_OP_TEXT || op->style.cell_h <= 0)
            continue;
        struct rect2i box = ui_debug_text_box(s, op);
        if (rect2i_is_empty(box))
            continue;
        const struct ui_widget *owner = ui_debug_op_owner(view, i);
        if (!parts_valid || owner != parts_owner) {
            ui_debug_collect_parts(owner, &parts);
            parts_owner = owner;
            parts_valid = true;
        }

        struct rect2i surface;
        if (text_surface(view, i, owner, &parts, box, &surface)) {
            int top = box.y - surface.y;
            int bottom = rect2i_end(surface).y - rect2i_end(box).y;
            if (abs(top - bottom) > 2)
                n = push_lint(out, cap, n, (struct ui_debug_lint){
                    .kind = UI_DEBUG_LINT_V_CENTER, .op = i, .other_op = -1,
                    .owner = owner, .text_box = box, .surface = surface,
                    .offset = (top - bottom) / 2,
                });
            int over_r = rect2i_end(box).x - rect2i_end(surface).x;
            int over_l = surface.x - box.x;
            int over = over_r > over_l ? over_r : over_l;
            if (over > 0)
                n = push_lint(out, cap, n, (struct ui_debug_lint){
                    .kind = UI_DEBUG_LINT_OVERFLOW, .op = i, .other_op = -1,
                    .owner = owner, .text_box = box, .surface = surface,
                    .offset = over,
                });
        }

        /* Compare doubled centers so boxes of different heights that
         * are centered on the same line do not count. */
        for (int k = 0; k < i; k++) {
            const struct ui_scene_op *prev = &s->ops[k];
            if (prev->kind != UI_SCENE_OP_TEXT ||
                ui_debug_op_owner(view, k) != owner)
                continue;
            struct rect2i pb = ui_debug_text_box(s, prev);
            struct rect2i ov = rect2i_intersect(pb, (struct rect2i){
                .pos = { .x = INT_MIN / 2, .y = box.y },
                .size = { .x = INT_MAX, .y = box.height },
            });
            int min_h = pb.height < box.height ? pb.height : box.height;
            if (ov.height * 2 < min_h)
                continue;
            int d2 = (2 * box.y + box.height) - (2 * pb.y + pb.height);
            if (abs(d2) > 2) {
                n = push_lint(out, cap, n, (struct ui_debug_lint){
                    .kind = UI_DEBUG_LINT_ROW, .op = i, .other_op = k,
                    .owner = owner, .text_box = box, .surface = pb,
                    .offset = d2 / 2,
                });
            }
            break;
        }
    }
    return n;
}

static const char *lint_kind_name(enum ui_debug_lint_kind kind)
{
    switch (kind) {
    case UI_DEBUG_LINT_V_CENTER: return "v-center";
    case UI_DEBUG_LINT_OVERFLOW: return "overflow";
    case UI_DEBUG_LINT_ROW:      return "row";
    }
    return "?";
}

/* Every finding, in a buffer the caller frees. */
static struct ui_debug_lint *lint_all(const struct ui_debug_view *view, int *n)
{
    *n = ui_debug_lint_scene(view, NULL, 0);
    struct ui_debug_lint *found = *n ? calloc((size_t)*n, sizeof(*found)) : NULL;
    if (!found)
        *n = 0;
    else
        ui_debug_lint_scene(view, found, *n);
    return found;
}

void ui_debug_write_lint(struct json_writer *j,
                         const struct ui_debug_view *view)
{
    int n;
    struct ui_debug_lint *found = lint_all(view, &n);
    json_obj_begin(j);
    json_key(j, "count"); json_int(j, n);
    json_key(j, "findings");
    json_arr_begin(j);
    for (int i = 0; i < n; i++) {
        const struct ui_debug_lint *f = &found[i];
        const struct ui_scene_op *op = &view->scene->ops[f->op];
        const char *text = ui_scene_op_text(view->scene, op);
        json_obj_begin(j);
        json_key(j, "kind"); json_str(j, lint_kind_name(f->kind));
        json_key(j, "op"); json_int(j, f->op);
        json_key(j, "text"); json_strn(j, text, text ? op->text_len : 0);
        json_key(j, "owner"); write_ref(j, view, f->owner);
        json_key(j, "box"); json_rect(j, f->text_box);
        json_key(j, f->kind == UI_DEBUG_LINT_ROW ? "other_box" : "surface");
        json_rect(j, f->surface);
        if (f->other_op >= 0) {
            const struct ui_scene_op *o = &view->scene->ops[f->other_op];
            const char *ot = ui_scene_op_text(view->scene, o);
            json_key(j, "other_op"); json_int(j, f->other_op);
            json_key(j, "other_text"); json_strn(j, ot, ot ? o->text_len : 0);
        }
        json_key(j, "offset"); json_int(j, f->offset);
        json_obj_end(j);
    }
    json_arr_end(j);
    json_obj_end(j);
    free(found);
}

/* ---- overlay ------------------------------------------------------- */

#define OV_BOUNDS_ALPHA 170
#define OV_BAND_ALPHA   70

static const uint32_t OV_DEPTH_COLORS[] = {
    0x3FA9F5, 0xF5A623, 0x7ED321, 0xBD10E0, 0x50E3C2, 0xF8E71C,
};
#define OV_MARGIN  0xF6B26B
#define OV_PADDING 0x93C47D
#define OV_CONTENT 0x6FA8DC
#define OV_TEXT    0x00E5FF
#define OV_RECT    0xFF4FD8
#define OV_PART    0xFFD400
#define OV_LINT    0xFF3B30

struct overlay_ctx {
    struct ui_scene *scene;
    int line;
};

static void ov_fill(struct overlay_ctx *o, struct rect2i r, uint32_t rgb,
                    uint8_t alpha)
{
    if (!rect2i_is_empty(r))
        ui_scene_blend_rect(o->scene, r, rgb, alpha);
}

static void ov_outline(struct overlay_ctx *o, struct rect2i r, uint32_t rgb,
                       uint8_t alpha, int t)
{
    if (r.width <= 0 || r.height <= 0)
        return;
    if (t * 2 >= r.width || t * 2 >= r.height) {
        ov_fill(o, r, rgb, alpha);
        return;
    }
    struct vector2i end = rect2i_end(r);
    ov_fill(o, (struct rect2i){ .pos = r.pos, .size = { .x = r.width, .y = t } },
            rgb, alpha);
    ov_fill(o, (struct rect2i){ .pos = { .x = r.x, .y = end.y - t },
                                .size = { .x = r.width, .y = t } }, rgb, alpha);
    ov_fill(o, (struct rect2i){ .pos = { .x = r.x, .y = r.y + t },
                                .size = { .x = t, .y = r.height - 2 * t } },
            rgb, alpha);
    ov_fill(o, (struct rect2i){ .pos = { .x = end.x - t, .y = r.y + t },
                                .size = { .x = t, .y = r.height - 2 * t } },
            rgb, alpha);
}

/* Fills outer minus inner as four bands. */
static void ov_band(struct overlay_ctx *o, struct rect2i outer,
                    struct rect2i inner, uint32_t rgb, uint8_t alpha)
{
    inner = rect2i_intersect(inner, outer);
    if (rect2i_is_empty(inner)) {
        ov_fill(o, outer, rgb, alpha);
        return;
    }
    struct vector2i oe = rect2i_end(outer), ie = rect2i_end(inner);
    ov_fill(o, rect2i_from_corners(outer.pos,
                                   (struct vector2i){ .x = oe.x, .y = inner.y }),
            rgb, alpha);
    ov_fill(o, rect2i_from_corners((struct vector2i){ .x = outer.x, .y = ie.y },
                                   oe), rgb, alpha);
    ov_fill(o, rect2i_from_corners((struct vector2i){ .x = outer.x, .y = inner.y },
                                   (struct vector2i){ .x = inner.x, .y = ie.y }),
            rgb, alpha);
    ov_fill(o, rect2i_from_corners((struct vector2i){ .x = ie.x, .y = inner.y },
                                   (struct vector2i){ .x = oe.x, .y = ie.y }),
            rgb, alpha);
}

static struct rect2i widget_content(const struct ui_widget *w,
                                    const struct ui_debug_parts *parts)
{
    for (int i = 0; i < parts->n; i++)
        if (!strcmp(parts->items[i].name, "content"))
            return parts->items[i].rect;
    struct insets2i pad = ui_css_resolve_insets(w->style.padding,
                                                w->bounds.size,
                                                &w->style_scale, true);
    return rect2i_apply_insets(w->bounds, pad);
}

static void ov_box(struct overlay_ctx *o, const struct ui_widget *w,
                   const struct ui_debug_parts *parts)
{
    struct vector2i basis = w->parent ? w->parent->bounds.size : w->bounds.size;
    struct insets2i m = ui_css_resolve_insets(w->style.margin, basis,
                                              &w->style_scale, true);
    struct rect2i outer = {
        .pos = { .x = w->bounds.x - m.l, .y = w->bounds.y - m.t },
        .size = { .x = w->bounds.width + m.l + m.r,
                  .y = w->bounds.height + m.t + m.b },
    };
    struct rect2i content = widget_content(w, parts);
    ov_band(o, outer, w->bounds, OV_MARGIN, OV_BAND_ALPHA);
    ov_band(o, w->bounds, content, OV_PADDING, OV_BAND_ALPHA);
    ov_fill(o, content, OV_CONTENT, OV_BAND_ALPHA);
}

static void ov_label(struct overlay_ctx *o, const struct ui_debug_view *view,
                     const struct ui_widget *w, const struct ui_debug_part *part,
                     struct ui_text_style style)
{
    char path[64];
    if (!ui_debug_path(view->root, w, path, sizeof(path)))
        snprintf(path, sizeof(path), "?");
    char text[160];
    int len;
    if (part)
        len = snprintf(text, sizeof(text), " %s %s %s[%d] %dx%d @%d,%d ",
                       ui_debug_type_name(w), path, part->name, part->index,
                       part->rect.width, part->rect.height,
                       part->rect.x, part->rect.y);
    else
        len = snprintf(text, sizeof(text), " %s %s %dx%d @%d,%d ",
                       ui_debug_type_name(w), path, w->bounds.width,
                       w->bounds.height, w->bounds.x, w->bounds.y);
    if (len <= 0)
        return;
    if ((size_t)len >= sizeof(text))
        len = (int)sizeof(text) - 1;

    struct rect2i anchor = part ? part->rect : w->bounds;
    struct vector2i view_size = view->root->bounds.size;
    struct rect2i box = {
        .pos = { .x = anchor.x, .y = rect2i_end(anchor).y },
        .size = { .x = len * style.advance, .y = style.cell_h },
    };
    if (box.y + box.height > view_size.y)
        box.y = anchor.y - box.height;
    if (box.y < 0)
        box.y = 0;
    if (box.x + box.width > view_size.x)
        box.x = view_size.x - box.width;
    if (box.x < 0)
        box.x = 0;
    ui_scene_blend_rect(o->scene, box, 0x101014, 225);
    ui_scene_text_run(o->scene, box, style, 0xFFFFFFFF, text, (size_t)len);
}

bool ui_debug_overlay_append(struct ui_scene *scene,
                             const struct ui_debug_view *view,
                             const struct ui_debug_overlay *overlay)
{
    if (!scene || !view || !view->root || !overlay || !overlay->flags ||
        !ui_scene_is_valid(scene))
        return true;
    const struct css_scale_context *scale = &view->root->style_scale;
    struct overlay_ctx o = {
        .scene = scene,
        .line = ui_px(scale->dpi_scale, 1),
    };
    unsigned flags = overlay->flags;

    /* Read the built scene before appending: the overlay's own ops must
     * not feed back into the lint or the text boxes. */
    const struct ui_scene *built = view->scene ? view->scene : scene;
    int built_ops = built->n_ops;
    struct ui_debug_lint *found = NULL;
    int n_lint = 0;
    if (flags & UI_DEBUG_OVERLAY_LINT) {
        struct ui_debug_view lint_view = *view;
        lint_view.scene = built;
        found = lint_all(&lint_view, &n_lint);
    }

    if (flags & UI_DEBUG_OVERLAY_BOUNDS) {
        for (int i = 0; i < built->n_scopes; i++) {
            const struct ui_scene_scope *sc = &built->scopes[i];
            const struct ui_widget *w = sc->owner;
            if (!w || !in_tree(view->root, w) || w == view->root)
                continue;
            uint32_t rgb = OV_DEPTH_COLORS[(unsigned)sc->depth %
                (sizeof(OV_DEPTH_COLORS) / sizeof(OV_DEPTH_COLORS[0]))];
            ov_outline(&o, w->bounds, rgb, OV_BOUNDS_ALPHA, o.line);
        }
    }

    if (flags & UI_DEBUG_OVERLAY_TEXT) {
        for (int i = 0; i < built_ops; i++) {
            const struct ui_scene_op *op = &built->ops[i];
            if (op->kind != UI_SCENE_OP_TEXT)
                continue;
            struct rect2i box = ui_debug_text_box(built, op);
            if (op->rect.height > box.height || op->rect.width > box.width)
                ov_outline(&o, op->rect, OV_RECT, 150, o.line);
            ov_outline(&o, box, OV_TEXT, 200, o.line);
        }
    }

    for (int i = 0; i < n_lint; i++) {
        ov_outline(&o, found[i].surface, OV_LINT, 110, o.line);
        ov_outline(&o, found[i].text_box, OV_LINT, 230, 2 * o.line);
    }
    free(found);

    const struct ui_widget *focus = NULL;
    if (overlay->selected && in_tree(view->root, overlay->selected))
        focus = overlay->selected;
    else if (overlay->has_pointer)
        focus = ui_debug_widget_at(view->root, overlay->pointer);

    if (focus) {
        static struct ui_debug_parts parts;
        ui_debug_collect_parts(focus, &parts);
        /* A widget with hit regions paints into a canvas; its bounds do
         * not describe a box. */
        bool has_regions = false;
        for (int i = 0; i < parts.n; i++)
            has_regions |= part_is_region(&parts.items[i]);
        if ((flags & UI_DEBUG_OVERLAY_BOX) && !has_regions)
            ov_box(&o, focus, &parts);
        const struct ui_debug_part *hovered = NULL;
        if (flags & UI_DEBUG_OVERLAY_PARTS) {
            for (int i = 0; i < parts.n; i++) {
                const struct ui_debug_part *p = &parts.items[i];
                bool under = overlay->has_pointer &&
                             rect2i_contains(p->rect, overlay->pointer);
                if (under && (!hovered ||
                              (int64_t)p->rect.width * p->rect.height <
                              (int64_t)hovered->rect.width * hovered->rect.height))
                    hovered = p;
                ov_outline(&o, p->rect, OV_PART, 120, o.line);
            }
            if (hovered)
                ov_outline(&o, hovered->rect, OV_PART, 255, 2 * o.line);
        }
        ov_outline(&o, focus->bounds, 0xFFFFFF, 230, o.line);
        if (flags & UI_DEBUG_OVERLAY_LABEL) {
            struct ui_text_style style = {
                .advance = ui_cell_dim(scale->cell_w, UI_DEFAULT_CELL_W),
                .cell_h = ui_cell_dim(scale->cell_h, UI_DEFAULT_CELL_H),
            };
            ov_label(&o, view, focus, hovered, style);
        }
    }
    return !o.scene->overflow;
}
