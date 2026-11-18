#include "ui/style_context.h"

#include "ui/surface.h"
#include "ui/css.h"
#include "ui/internal/tree.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

static void drop_owned_sheet(struct ui_style_context *context)
{
    if (!context->owned_sheet)
        return;
    css_sheet_clear(context->owned_sheet);
    free(context->owned_sheet);
    context->owned_sheet = NULL;
}

static void rebuild_sheet(struct ui_style_context *context)
{
    if (!context)
        return;
    if (!context->host_sheet) {
        drop_owned_sheet(context);
        context->sheet = css_default_sheet();
        return;
    }
    if (context->host_sheet->has_defaults) {
        drop_owned_sheet(context);
        context->sheet = context->host_sheet;
        return;
    }

    if (!context->owned_sheet) {
        context->owned_sheet = calloc(1, sizeof(*context->owned_sheet));
        if (!context->owned_sheet) {
            context->sheet = css_default_sheet();
            return;
        }
    }

    css_sheet_clear(context->owned_sheet);
    css_init_default(context->owned_sheet);
    css_sheet_append(context->owned_sheet, context->host_sheet);
    context->sheet = context->owned_sheet;
}

static uint64_t next_generation(uint64_t generation)
{
    return generation == UINT64_MAX ? 1 : generation + 1;
}

void ui_style_context_init(struct ui_style_context *context)
{
    if (!context)
        return;
    memset(context, 0, sizeof(*context));
    context->scale = ui_scale_ctx_default();
    rebuild_sheet(context);
}

void ui_style_context_destroy(struct ui_style_context *context)
{
    if (!context)
        return;
    css_sheet_clear(context->owned_sheet);
    free(context->owned_sheet);
    free(context->path);
    context->owned_sheet = NULL;
    context->path = NULL;
    context->path_count = 0;
    context->path_cap = 0;
    context->sheet = NULL;
    context->host_sheet = NULL;
}

void ui_style_context_set_sheet(struct ui_style_context *context,
                                const struct css_sheet *sheet)
{
    if (!context)
        return;
    context->host_sheet = sheet;
    rebuild_sheet(context);
    context->generation = next_generation(context->generation);
}

void ui_style_context_set_scale(struct ui_style_context *context,
                                struct css_scale_context scale)
{
    if (!context)
        return;
    struct css_scale_context before = context->scale;
    ui_scale_ctx_set(&context->scale, scale.dpi_scale, scale.cell_w,
                     scale.cell_h);
    if (before.dpi_scale != context->scale.dpi_scale ||
        before.cell_w != context->scale.cell_w ||
        before.cell_h != context->scale.cell_h)
        context->generation = next_generation(context->generation);
}

const struct css_sheet *ui_style_context_sheet(
    const struct ui_style_context *context)
{
    return context && context->sheet ? context->sheet : css_default_sheet();
}

struct css_style ui_style_context_style(const struct ui_style_context *context,
                                        const char *class_name,
                                        unsigned pseudo_flags)
{
    struct css_style style;
    css_style_resolve(ui_style_context_sheet(context), class_name, NULL, NULL, pseudo_flags, &style);
    return style;
}

uint64_t ui_style_context_generation(const struct ui_style_context *context)
{
    return context ? context->generation : 0;
}

static void element_from_widget(const struct ui_widget *widget,
                                struct css_element *element)
{
    element->class_name = widget->impl.style_class;
    element->id_name = widget->impl.style_id;
    element->tag_name = widget->impl.style_tag;
}

/* Makes room for `count` ancestors in the scratch path of `context`. */
static bool reserve_path(struct ui_style_context *context, size_t count)
{
    if (count <= context->path_cap)
        return true;
    size_t cap = context->path_cap ? context->path_cap * 2 : 16;
    while (cap < count)
        cap *= 2;
    struct css_element *next = realloc(context->path, cap * sizeof(*next));
    if (!next)
        return false;
    context->path = next;
    context->path_cap = cap;
    return true;
}

static void apply_widget_subtree(struct ui_style_context *context,
                                 struct ui_widget *widget,
                                 bool parent_changed, size_t depth)
{
    if (!widget)
        return;

    uint64_t generation = context ? context->generation : 0;
    bool same_sheet = widget->impl.style_generation == generation;
    bool changed = parent_changed || !widget->impl.style_valid || !same_sheet;
    struct css_style before = widget->style;
    if (changed) {
        struct css_element element;
        element_from_widget(widget, &element);
        const struct css_element *ancestors = NULL;
        size_t ancestor_count = 0;
        if (context && depth > 0 && depth <= context->path_count) {
            ancestors = context->path;
            ancestor_count = depth;
        }
        css_style_resolve_node(ui_style_context_sheet(context), &element,
                               widget->impl.style_pseudo, ancestors,
                               ancestor_count, &widget->style);

        if (!widget->style.has_text_color && widget->parent &&
            widget->parent->impl.style_valid &&
            widget->parent->style.has_text_color) {
            widget->style.text_color = widget->parent->style.text_color;
            widget->style.has_text_color = true;
        }
        if (widget->parent && widget->parent->impl.style_valid) {
            float parent_opacity = widget->parent->style.opacity;
            if (parent_opacity < 0.0f) parent_opacity = 0.0f;
            if (parent_opacity > 1.0f) parent_opacity = 1.0f;
            widget->style.opacity *= parent_opacity;
            widget->style.has_opacity = widget->style.has_opacity ||
                                        widget->parent->style.has_opacity;
        }
        widget->impl.style_generation = generation;
        widget->impl.style_valid = true;
    }
    widget->style_scale = context ? context->scale
                                : (struct css_scale_context){0};
    if (changed) {
        bool layout = !same_sheet || !css_style_layout_eq(&before, &widget->style);
        ui_widget_invalidate(widget, layout ? UI_WIDGET_INVALIDATE_MEASURE
                                            : UI_WIDGET_INVALIDATE_PAINT);
        if (widget->ops && widget->ops->style_changed)
            widget->ops->style_changed(widget);
    }

    /* The children of this widget see it as their nearest ancestor. */
    if (context && reserve_path(context, depth + 1)) {
        element_from_widget(widget, &context->path[depth]);
        context->path_count = depth + 1;
    }

    for (struct ui_widget *c = widget->first_child; c; c = c->next_sibling)
        if (!c->impl.hidden)
            apply_widget_subtree(context, c, changed, depth + 1);
}

void ui_style_context_apply(struct ui_style_context *context,
                            struct ui_widget *root)
{
    if (context)
        context->path_count = 0;
    if (root)
        apply_widget_subtree(context, root, false, 0);
}

#define PART_ANCESTOR_STACK 32

struct css_element_list {
    struct css_element stack[PART_ANCESTOR_STACK];
    struct css_element *heap;
    size_t count;
};

static struct css_element *element_list_items(struct css_element_list *list)
{
    return list->heap ? list->heap : list->stack;
}

/* Collects `widget` and its ancestors, root first. Returns false when the
   tree is deeper than the list can hold. */
static bool element_list_fill(struct css_element_list *list,
                              const struct ui_widget *widget)
{
    size_t count = 0;
    for (const struct ui_widget *p = widget; p; p = p->parent)
        count++;
    if (count > PART_ANCESTOR_STACK) {
        list->heap = malloc(count * sizeof(*list->heap));
        if (!list->heap)
            return false;
    }
    struct css_element *items = element_list_items(list);
    size_t i = count;
    for (const struct ui_widget *p = widget; p; p = p->parent)
        element_from_widget(p, &items[--i]);
    list->count = count;
    return true;
}

static void resolve_part_style(const struct ui_widget *widget,
                               const char *class_name, unsigned pseudo_flags,
                               struct css_style *out)
{
    const struct css_element element = { .class_name = class_name };
    struct css_element_list list = {0};
    const struct css_element *ancestors = NULL;
    size_t ancestor_count = 0;
    if (widget && element_list_fill(&list, widget)) {
        ancestors = element_list_items(&list);
        ancestor_count = list.count;
    }
    css_style_resolve_node(ui_widget_sheet(widget), &element, pseudo_flags,
                           ancestors, ancestor_count, out);
    free(list.heap);
}

struct css_style ui_widget_part_style(const struct ui_widget *widget,
                                      const char *class_name,
                                      unsigned pseudo_flags)
{
    struct css_style style;
    resolve_part_style(widget, class_name, pseudo_flags, &style);
    return style;
}

struct part_table {
    char **names;
    uint32_t count;
    uint32_t name_cap;
    uint32_t *buckets;
    uint32_t bucket_cap;
};

static struct part_table part_table;

static uint32_t part_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void part_bucket_insert(uint32_t *buckets, uint32_t cap, uint32_t id,
                               uint32_t hash)
{
    uint32_t mask = cap - 1;
    uint32_t slot = hash & mask;
    while (buckets[slot])
        slot = (slot + 1) & mask;
    buckets[slot] = id;
}

/* Reports a part-table failure. The UI core has no logger, so the message
   goes to stderr like every other UI core diagnostic. */
static void part_log(const char *message)
{
    fprintf(stderr, "wsap: %s\n", message);
}

static void part_abort(const char *message)
{
    part_log(message);
    abort();
}

static bool part_table_grow(void)
{
    uint32_t cap = part_table.bucket_cap ? part_table.bucket_cap * 2 : 64;
    uint32_t *buckets = calloc(cap, sizeof(*buckets));
    if (!buckets)
        return false;
    for (uint32_t i = 0; i < part_table.count; i++)
        part_bucket_insert(buckets, cap, i + 1,
                           part_hash(part_table.names[i]));
    free(part_table.buckets);
    part_table.buckets = buckets;
    part_table.bucket_cap = cap;
    return true;
}

ui_part_id ui_part_intern(const char *name)
{
    if (!name || !name[0])
        return 0;
    uint32_t hash = part_hash(name);
    if (part_table.bucket_cap) {
        uint32_t mask = part_table.bucket_cap - 1;
        for (uint32_t slot = hash & mask; part_table.buckets[slot];
             slot = (slot + 1) & mask) {
            ui_part_id id = part_table.buckets[slot];
            if (strcmp(part_table.names[id - 1], name) == 0)
                return id;
        }
    }

    if ((part_table.count + 1) * 2 > part_table.bucket_cap &&
        !part_table_grow()) {
        part_log("out of memory growing the UI part table");
        return 0;
    }
    if (part_table.count == part_table.name_cap) {
        uint32_t cap = part_table.name_cap ? part_table.name_cap * 2 : 32;
        char **names = realloc(part_table.names, cap * sizeof(*names));
        if (!names) {
            part_log("out of memory growing the UI part names");
            return 0;
        }
        part_table.names = names;
        part_table.name_cap = cap;
    }
    char *copy = strdup(name);
    if (!copy) {
        part_log("out of memory copying a UI part name");
        return 0;
    }
    part_table.names[part_table.count++] = copy;
    ui_part_id id = part_table.count;
    part_bucket_insert(part_table.buckets, part_table.bucket_cap, id, hash);
    return id;
}

const char *ui_part_name(ui_part_id part)
{
    if (part == 0)
        return NULL;
    if (part > part_table.count)
        part_abort("unknown UI part handle");
    return part_table.names[part - 1];
}

void ui_part_shutdown(void)
{
    for (uint32_t i = 0; i < part_table.count; i++)
        free(part_table.names[i]);
    free(part_table.names);
    free(part_table.buckets);
    part_table = (struct part_table){0};
}

static struct ui_part_slot *find_part_slot(const struct ui_widget *widget,
                                           ui_part_id part,
                                           unsigned pseudo_flags)
{
    for (struct ui_part_slot *s = widget->impl.part_slots; s; s = s->next)
        if (s->part == part && s->pseudo == pseudo_flags)
            return s;
    return NULL;
}

static uint64_t part_context_generation(const struct ui_widget *widget)
{
    const struct ui_surface *surface = ui_widget_surface(widget);
    return surface ? ui_style_context_generation(&surface->impl.style) : 0;
}

struct css_style ui_widget_part(const struct ui_widget *widget,
                                ui_part_id part, unsigned pseudo_flags)
{
    const char *name = ui_part_name(part);
    struct css_style out = {0};
    if (!widget)
        return out;
    uint64_t context = part_context_generation(widget);
    uint64_t style = widget->impl.style_generation;
    struct ui_part_slot *slot = find_part_slot(widget, part, pseudo_flags);
    if (!slot) {
        slot = calloc(1, sizeof(*slot));
        if (!slot) {
            resolve_part_style(widget, name, pseudo_flags, &out);
            return out;
        }
        struct ui_widget *owner = (struct ui_widget *)widget;
        slot->part = part;
        slot->pseudo = pseudo_flags;
        slot->next = owner->impl.part_slots;
        owner->impl.part_slots = slot;
    }
    if (!slot->valid || slot->context_generation != context ||
        slot->style_generation != style) {
        resolve_part_style(widget, name, pseudo_flags, &slot->style);
        slot->context_generation = context;
        slot->style_generation = style;
        slot->valid = true;
    }
    return slot->style;
}

void ui_widget_part_cache_stale(struct ui_widget *widget)
{
    for (struct ui_part_slot *s = widget->impl.part_slots; s; s = s->next)
        s->valid = false;
}

void ui_widget_part_cache_stale_subtree(struct ui_widget *widget)
{
    ui_widget_part_cache_stale(widget);
    for (struct ui_widget *c = widget->first_child; c; c = c->next_sibling)
        ui_widget_part_cache_stale_subtree(c);
}

void ui_widget_part_cache_free(struct ui_widget *widget)
{
    struct ui_part_slot *s = widget->impl.part_slots;
    while (s) {
        struct ui_part_slot *next = s->next;
        free(s);
        s = next;
    }
    widget->impl.part_slots = NULL;
}

const struct css_style *ui_widget_resolved_style(const struct ui_widget *widget)
{
    return widget && widget->impl.style_valid ? &widget->style : NULL;
}
