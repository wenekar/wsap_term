#define _GNU_SOURCE
#include "app/config/settings.h"

#include "app/config/config.h"
#include "theme/look.h"
#include "platform/platform.h"
#include "ui/describe.h"
#include "ui/ui.h"
#include "util/options.h"
#include "ui/yasl/client.h"
#include "ui/widget_impl.h"
#include "ui/yasl/window.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum category { ALL, APPEARANCE, TEXT, CURSOR, TERMINAL, WINDOW, CATEGORY_COUNT };
enum action_id { SAVE = 1, CANCEL, RESET, OPEN_CONFIG, FONT_HELP, COLOR_HELP, CATEGORY_BASE = 100 };

static const char *const categories[] = {
    "All settings", "Appearance", "Text", "Cursor", "Terminal", "Window",
};
static const char *const category_ids[] = {
    "category-all", "category-appearance", "category-text", "category-cursor",
    "category-terminal", "category-window",
};

struct field_info {
    const char *key, *label, *help;
    enum category category;
};

static const struct field_info INFO[] = {
    { "theme", "Theme", "Choose a theme or enter a CSS path.", APPEARANCE },
    { "font", "Font family", "Installed fonts or a custom pattern.", TEXT },
    { "font-size", "Font size", "Points. Updates all open terminals.", TEXT },
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    { "hinting", "Pixel positioning", "Auto uses native pixel alignment. None keeps fractional positions.", TEXT },
#else
    { "hinting", "Font hinting", "Outline hinting.", TEXT },
#endif
    { "antialias", "Antialiasing", "Smooth glyph edges.", TEXT },
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    { "font-smoothing", "Font smoothing", "Use native font smoothing when antialiasing is on.", TEXT },
#endif
    { "foreground", "Foreground", "Text color. Use the Default theme.", APPEARANCE },
    { "background", "Background", "Background color. Use the Default theme.", APPEARANCE },
    { "cursor-style", "Cursor shape", "Updates the default cursor in open tabs.", CURSOR },
    { "cursor-blink", "Cursor blink", "Blink the default cursor in open tabs.", CURSOR },
    { "scrollback", "History lines", "Lines kept by new tabs. 0 disables history.", TERMINAL },
    { "command", "Shell command", "Program for new tabs. Empty uses $SHELL.", TERMINAL },
    { "vsync", "Vertical sync", "Frame pacing for new windows.", WINDOW },
    { "fullscreen", "Start fullscreen", "Applies to new terminal windows.", WINDOW },
    { "width", "Window width", "Pixels for new windows. 0 uses cell size.", WINDOW },
    { "height", "Window height", "Pixels for new windows. 0 uses cell size.", WINDOW },
    { "menu-hover-delay", "Submenu delay", "Milliseconds before a submenu opens.", WINDOW },
};

struct settings_row {
    struct opts_option option;
    struct ui_box *row;
    struct ui_label *error;
    struct ui_input *input;
    struct ui_dropdown *dropdown;
    enum category category;
    char label[64], help_text[128], row_id[80];
    char title_key[96], help_key[96], error_key[96];
    char input_key[96], dropdown_key[96];
    char baseline[UI_INPUT_CAP];
    struct ui_node_desc node, row_children[2], description_children[3];
    struct ui_node_desc editor_children[2];
    struct ui_dropdown_item *initial_items;
    size_t initial_item_count;
    bool combo, choice, valid, dirty;
};

struct settings_form {
    struct settings_host host;
    struct window *window;
    struct ui_view *view;
    struct ui_widget *root;
    struct ui_box *layout, *header, *body, *sidebar, *fields, *footer;
    struct ui_input *search;
    struct ui_scrollview *scroll;
    struct ui_label *title, *status, *empty, *section[CATEGORY_COUNT];
    struct ui_label *font_help, *color_help;
    struct ui_button *nav[CATEGORY_COUNT], *save, *cancel, *reset, *config;
    struct settings_row *rows;
    struct ui_node_desc *field_nodes;
    size_t count;
    enum category category;
    bool loading;
};

static const char *const section_keys[] = {
    NULL, "section-appearance", "section-text", "section-cursor",
    "section-terminal", "section-window",
};

static void identity(struct ui_widget *widget, const char *classes, const char *id)
{
    ui_widget_set_style_identity(widget, classes, id, NULL);
}

static bool build_choices(struct settings_row *row, struct look *look,
                          const char *antialias);

static const char *row_value(const struct settings_row *row)
{
    return row->choice ? ui_dropdown_value(row->dropdown) : ui_input_text(row->input);
}

static bool theme_valid(struct settings_form *form, const char *value)
{
    if (!value[0]) return true;
    struct look *look = form->host.look;
    for (int i = 0; i < look_theme_count(look); i++) {
        const struct look_theme *theme = look_theme_at(look, i);
        if (!strcmp(theme->name, value) || !strcasecmp(theme->label, value))
            return true;
    }
    size_t len = strlen(value);
    return (strchr(value, '/') || (len > 4 && !strcasecmp(value + len - 4, ".css"))) &&
        access(value, R_OK) == 0;
}

static void validate(struct settings_form *form, struct settings_row *row)
{
    const char *value = row_value(row);
    row->dirty = strcmp(value, row->baseline) != 0;
    if (!row->dirty) {
        row->valid = true;
    } else {
        row->valid = opts_validate(&app_config_spec, row->option.key, value);
        if (!strcmp(row->option.key, "theme"))
            row->valid = row->valid && theme_valid(form, value);
        size_t len = strlen(value);
        if (strpbrk(value, "\r\n") || len + strlen(row->option.key) + 4 >= 1024 ||
            (len && (value[0] == ' ' || value[len - 1] == ' ')))
            row->valid = false;
    }
    char error[128] = "Enter a valid value. Empty resets the default.";
    if (row->option.kind == OPT_KIND_NUMBER)
        snprintf(error, sizeof(error), "Use %d to %d. Empty resets the default.",
                  row->option.min, row->option.max);
    else if (row->option.kind == OPT_KIND_COLOR)
        snprintf(error, sizeof(error), "Use #rrggbb, #rgb, or rgb:rr/gg/bb.");
    else if (!strcmp(row->option.key, "theme"))
        snprintf(error, sizeof(error), "Choose a listed theme or a readable CSS file.");
    ui_label_set_text(row->error, error);
    ui_widget_set_hidden(ui_label_widget(row->error), row->valid);
    struct ui_widget *editor = row->choice
        ? ui_dropdown_widget(row->dropdown) : ui_input_widget(row->input);
    if (editor) {
        ui_widget_set_accessible_invalid(editor, !row->valid);
        ui_widget_set_accessible_description(
            editor, row->valid ? row->help_text : error);
    }
}

static void update_status(struct settings_form *form)
{
    size_t changed = 0, invalid = 0;
    for (size_t i = 0; i < form->count; i++) {
        changed += form->rows[i].dirty;
        invalid += !form->rows[i].valid;
    }
    char status[128];
    if (invalid)
        snprintf(status, sizeof(status), "Fix %zu invalid value%s.", invalid, invalid == 1 ? "" : "s");
    else if (changed)
        snprintf(status, sizeof(status), "%zu unsaved change%s.", changed, changed == 1 ? "" : "s");
    else
        snprintf(status, sizeof(status), "No unsaved changes.");
    ui_label_set_text(form->status, status);
    ui_button_set_disabled(form->save, !changed || invalid);
}

static void filter_rows(struct settings_form *form)
{
    const char *query = ui_input_text(form->search);
    size_t visible = 0;
    bool sections[CATEGORY_COUNT] = {0};
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        bool match = (form->category == ALL || form->category == row->category) &&
            (strcasestr(row->label, query) || strcasestr(row->option.key, query) ||
             strcasestr(row->help_text, query) || strcasestr(categories[row->category], query) ||
             strcasestr(row_value(row), query));
        ui_widget_set_hidden(&row->row->widget, !match);
        sections[row->category] |= match;
        visible += match;
    }
    for (int i = 1; i < CATEGORY_COUNT; i++)
        ui_widget_set_hidden(ui_label_widget(form->section[i]), !sections[i]);
    ui_widget_set_hidden(ui_label_widget(form->empty), visible != 0);
    for (int i = 0; i < CATEGORY_COUNT; i++)
        identity(&form->nav[i]->widget, form->category == (enum category)i
                    ? "settings-button settings-nav active" : "settings-button settings-nav",
                    category_ids[i]);
}

static void set_row_value(struct settings_row *row, const char *value)
{
    if (!row->choice)
        ui_input_set_text(row->input, value);
    if (row->choice || row->combo)
        ui_dropdown_set_value(row->dropdown, value);
}

static struct settings_row *find_row(struct settings_form *form, const char *key)
{
    for (size_t i = 0; i < form->count; i++)
        if (!strcmp(form->rows[i].option.key, key))
            return &form->rows[i];
    return NULL;
}

static void sync_hinting_choices(struct settings_form *form)
{
    struct settings_row *hinting = find_row(form, "hinting");
    struct settings_row *antialias = find_row(form, "antialias");
    if (!hinting || !antialias || !hinting->choice)
        return;
    char value[UI_INPUT_CAP];
    snprintf(value, sizeof(value), "%s", row_value(hinting));
    if (!build_choices(hinting, form->host.look, row_value(antialias)))
        return;
    set_row_value(hinting, value);
    if (!row_value(hinting)[0])
        set_row_value(hinting, "auto");
    validate(form, hinting);
}

static void load_row(struct settings_form *form, struct settings_row *row,
                     const char *value, bool baseline, bool show)
{
    if (baseline) strcpy(row->baseline, value);
    if (show) set_row_value(row, value);
    validate(form, row);
}

static bool format_row(const struct settings_row *row, const struct app_config *config,
                       char *value, size_t size)
{
    return opts_format(&app_config_spec, config, row->option.key, value, size);
}

static void finish_load(struct settings_form *form)
{
    sync_hinting_choices(form);
    form->loading = false;
    update_status(form);
    filter_rows(form);
}

static bool load_values(struct settings_form *form, const struct app_config *config,
                         bool baseline)
{
    form->loading = true;
    bool ok = true;
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        char value[UI_INPUT_CAP];
        if (!format_row(row, config, value, sizeof(value))) {
            ok = false;
            break;
        }
        load_row(form, row, value, baseline, true);
    }
    finish_load(form);
    return ok;
}

static bool config_moved(const struct settings_form *form, const struct app_config *config)
{
    for (size_t i = 0; i < form->count; i++) {
        const struct settings_row *row = &form->rows[i];
        char value[UI_INPUT_CAP];
        if (format_row(row, config, value, sizeof(value)) && strcmp(value, row->baseline))
            return true;
    }
    return false;
}

static void settings_restyle(struct window *w)
{
    struct settings_form *form = window_ui_content(w);
    const struct app_config *config = form->host.config;
    if (!config_moved(form, config))
        return;
    form->loading = true;
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        char value[UI_INPUT_CAP];
        if (!format_row(row, config, value, sizeof(value)) || !strcmp(value, row->baseline))
            continue;
        bool edited = strcmp(row_value(row), row->baseline) != 0;
        load_row(form, row, value, true, !edited);
    }
    finish_load(form);
}

static void save_form(struct settings_form *form)
{
    const struct settings_host *host = &form->host;
    struct app_config draft = {0};
    const char **keys = calloc(form->count, sizeof(*keys));
    size_t count = 0;
    if (!keys || !opts_copy(&app_config_spec,&draft, host->config)) {
        free(keys);
        ui_label_set_text(form->status, "Cannot save. Memory is full.");
        return;
    }
    bool valid = true;
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        validate(form, row);
        if (!row->valid) valid = false;
        if (row->dirty) {
            keys[count++] = row->option.key;
            valid &= opts_set(&app_config_spec,&draft, row->option.key, row_value(row));
        }
    }
    bool has_file = host->config_path != NULL;
    if (!valid) {
        update_status(form);
    } else if (has_file && !host->save(host->ud, &draft, keys, count)) {
        char error[UI_LABEL_TEXT_CAP];
        snprintf(error, sizeof(error), "Save failed: %s", strerror(errno));
        ui_label_set_text(form->status, error);
    } else {
        bool applied = host->apply(host->ud, &draft);
        load_values(form, &draft, true);
        ui_label_set_text(form->status, !applied ? "Save failed." :
                          has_file ? "Saved." : "Applied. No config file.");
    }
    opts_destroy(&app_config_spec,&draft);
    free(keys);
}

static void open_config(struct settings_form *form)
{
    char absolute[PATH_MAX], url[PATH_MAX * 3 + 8];
    if (!form->host.config_path || !realpath(form->host.config_path, absolute)) {
        ui_label_set_text(form->status, "The config file does not exist. Save a change first.");
        return;
    }
    char *out = url;
    memcpy(out, "file://", 7);
    out += 7;
    for (const unsigned char *p = (unsigned char *)absolute; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("/-._~", *p))
            *out++ = (char)*p;
        else {
            snprintf(out, 4, "%%%02X", *p);
            out += 3;
        }
    }
    *out = 0;
    platform_open_url(url);
}

static bool form_action(struct ui_widget *source, const struct ui_action *action,
                         void *userdata)
{
    struct settings_form *form = userdata;
    if (form->loading) return true;
    if (action->kind == UI_ACTION_CHANGE) {
        if (source == ui_input_widget(form->search)) {
            form->category = ALL;
            filter_rows(form);
            ui_scrollview_scroll_to(form->scroll, 0);
            return true;
        }
        for (size_t i = 0; i < form->count; i++) {
            struct settings_row *row = &form->rows[i];
            if ((row->choice || row->combo) && source == &row->dropdown->widget) {
                if (row->combo)
                    ui_input_set_text(row->input, ui_dropdown_value(row->dropdown));
            } else if (!row->choice && source == &row->input->widget) {
                if (row->combo) ui_dropdown_set_value(row->dropdown, row_value(row));
            } else continue;
            validate(form, row);
            if (!strcmp(row->option.key, "antialias"))
                sync_hinting_choices(form);
            update_status(form);
            return true;
        }
        return false;
    }
    if (action->kind != UI_ACTION_ACTIVATE)
        return false;
    switch (action->id) {
    case SAVE: save_form(form); break;
    case CANCEL: window_close(form->window); break;
    case RESET: {
        struct app_config defaults = {0};
        app_config_defaults(&defaults);
        load_values(form, &defaults, false);
        opts_destroy(&app_config_spec,&defaults);
        break;
    }
    case OPEN_CONFIG: open_config(form); break;
    case FONT_HELP: platform_open_url("https://www.freedesktop.org/wiki/Software/fontconfig/"); break;
    case COLOR_HELP: platform_open_url("https://en.wikipedia.org/wiki/ANSI_escape_code#Colors"); break;
    default:
        if (action->id < CATEGORY_BASE || action->id >= CATEGORY_BASE + CATEGORY_COUNT)
            return false;
        form->category = action->id - CATEGORY_BASE;
        filter_rows(form);
        ui_scrollview_scroll_to(form->scroll, 0);
        break;
    }
    return true;
}

static bool handle_root_event(struct settings_form *form,
                              const struct ui_event *event)
{
    if (event->kind != UI_EVENT_KEY ||
        event->data.key.action == UI_KEY_RELEASE)
        return false;
    if (event->data.key.key == UI_KEY_ESCAPE) {
        window_close(form->window);
        return true;
    }
    if (event->data.key.key == UI_KEY_CHAR &&
        event->data.key.mods & (UI_MOD_CTRL | UI_MOD_SUPER)) {
        if (event->data.key.text == 'f') {
            ui_widget_focus(ui_input_widget(form->search));
            ui_input_select_all(form->search);
            return true;
        }
        if (event->data.key.text == 's') {
            if (!ui_button_is_disabled(form->save)) save_form(form);
            return true;
        }
    }
    return false;
}

static bool root_event(struct ui_widget *widget, const struct ui_event *event,
                       void *userdata)
{
    struct settings_form *form = userdata;
    (void)widget;
    if (event->kind == UI_EVENT_SHORTCUT &&
        event->data.shortcut.action == INPUT_SHORTCUT_CLOSE_TAB) {
        window_close(form->window);
        return true;
    }
    return handle_root_event(form, event);
}

static bool hinting_allowed(const char *antialias, const char *hinting)
{
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    (void)antialias;
    (void)hinting;
    return true;
#else
    return strcmp(antialias, "mono") != 0 || !strcmp(hinting, "auto") ||
        !strcmp(hinting, "none") || !strcmp(hinting, "full");
#endif
}

static int compare_families(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static bool make_choices(struct settings_row *row, struct look *look,
                         const char *antialias,
                         struct ui_dropdown_item **items_out,
                         size_t *count_out)
{
    const char *key = row->option.key;
    size_t count = 0, font_count = 0;
    char **fonts = NULL;
    if (!strcmp(key, "font")) {
        fonts = font_list_families(&font_count);
        if (fonts) qsort(fonts, font_count, sizeof(*fonts), compare_families);
        count = font_count + 2;
    } else if (!strcmp(key, "theme")) {
        count = (size_t)look_theme_count(look) + 1;
    } else {
        while (opts_choice_at(&app_config_spec, key, count)) count++;
    }
    struct ui_dropdown_item *items = calloc(count ? count : 1, sizeof(*items));
    if (!items) {
        for (size_t i = 0; i < font_count; i++) free(fonts[i]);
        free(fonts);
        return false;
    }
    if (!strcmp(key, "font")) {
        items[0] = (struct ui_dropdown_item){ "Default font", "" };
        items[1] = (struct ui_dropdown_item){ "System monospace", "monospace" };
        for (size_t i = 0; i < font_count; i++) {
            char *family = strdup(fonts[i]);
            if (!family) {
                for (size_t j = 0; j < i; j++)
                    free((char *)items[j + 2].label);
                free(items);
                for (size_t j = 0; j < font_count; j++) free(fonts[j]);
                free(fonts);
                return false;
            }
            items[i + 2] = (struct ui_dropdown_item){ family, family };
        }
    } else if (!strcmp(key, "theme")) {
        items[0] = (struct ui_dropdown_item){ "Default", "" };
        for (size_t i = 1; i < count; i++) {
            const struct look_theme *theme = look_theme_at(look, (int)i - 1);
            items[i] = (struct ui_dropdown_item){ theme->label, theme->name };
        }
    } else {
        size_t kept = 0;
        for (size_t i = 0; i < count; i++) {
            const char *value = opts_choice_at(&app_config_spec, key, i);
            if (!strcmp(key, "hinting") &&
                !hinting_allowed(antialias, value))
                continue;
            const char *label = !strcmp(value, "true") ? "On" :
                !strcmp(value, "false") ? "Off" : value;
            items[kept++] = (struct ui_dropdown_item){ label, value };
        }
        count = kept;
    }
    for (size_t i = 0; i < font_count; i++) free(fonts[i]);
    free(fonts);
    *items_out = items;
    *count_out = count;
    return true;
}

static void free_choices(const struct settings_row *row,
                         struct ui_dropdown_item *items, size_t count)
{
    if (!items)
        return;
    if (row->option.key && !strcmp(row->option.key, "font"))
        for (size_t i = 2; i < count; i++)
            free((char *)items[i].label);
    free(items);
}

static bool build_choices(struct settings_row *row, struct look *look,
                          const char *antialias)
{
    struct ui_dropdown_item *items = NULL;
    size_t count = 0;
    if (!make_choices(row, look, antialias, &items, &count))
        return false;
    bool ok = ui_dropdown_set_items(row->dropdown, items, count);
    free_choices(row, items, count);
    return ok;
}

static bool init_row(struct settings_row *row, size_t index, struct look *look)
{
    if (!opts_option_at(&app_config_spec, index, &row->option)) return false;
    const char *key = row->option.key;
    snprintf(row->label, sizeof(row->label), "%s", key);
    row->category = APPEARANCE;
    snprintf(row->help_text, sizeof(row->help_text), "ANSI palette color. Use the Default theme.");
    for (size_t i = 0; i < sizeof(INFO) / sizeof(INFO[0]); i++)
        if (!strcmp(key, INFO[i].key)) {
            row->category = INFO[i].category;
            snprintf(row->label, sizeof(row->label), "%s", INFO[i].label);
            snprintf(row->help_text, sizeof(row->help_text), "%s", INFO[i].help);
            break;
        }
    if (!strncmp(key, "color", 5))
        snprintf(row->label, sizeof(row->label), "ANSI color %s", key + 5);
    row->choice = row->option.kind == OPT_KIND_CHOICE;
    row->combo = !strcmp(key, "font") || !strcmp(key, "theme");
    snprintf(row->row_id, sizeof(row->row_id), "row-%s", key);
    snprintf(row->title_key, sizeof(row->title_key), "title-%s", key);
    snprintf(row->help_key, sizeof(row->help_key), "help-%s", key);
    snprintf(row->error_key, sizeof(row->error_key), "error-%s", key);
    snprintf(row->input_key, sizeof(row->input_key), "input-%s", key);
    snprintf(row->dropdown_key, sizeof(row->dropdown_key), "dropdown-%s", key);
    if ((row->choice || row->combo) &&
        !make_choices(row, look, "", &row->initial_items,
                      &row->initial_item_count))
        return false;

    row->description_children[0] = (struct ui_node_desc){
        .kind = UI_NODE_LABEL,
        .key = row->title_key,
        .props.label = {
            .text = row->label,
            .wrap = UI_LABEL_WRAP_WORD,
            .align = UI_LABEL_ALIGN_LEFT,
        },
    };
    row->description_children[1] = (struct ui_node_desc){
        .kind = UI_NODE_LABEL,
        .key = row->help_key,
        .class_name = "settings-help",
        .tag_name = "",
        .props.label = {
            .text = row->help_text,
            .wrap = UI_LABEL_WRAP_WORD,
            .align = UI_LABEL_ALIGN_LEFT,
            .selectable = true,
        },
    };
    row->description_children[2] = (struct ui_node_desc){
        .kind = UI_NODE_LABEL,
        .key = row->error_key,
        .class_name = "settings-error",
        .tag_name = "",
        .props.label = {
            .text = "",
            .wrap = UI_LABEL_WRAP_WORD,
            .align = UI_LABEL_ALIGN_LEFT,
        },
    };
    row->row_children[0] = (struct ui_node_desc){
        .kind = UI_NODE_COLUMN,
        .class_name = "settings-description",
        .tag_name = "",
        .children = row->description_children,
        .child_count = 3,
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
    };
    size_t editor_count = 0;
    if (!row->choice) {
        row->editor_children[editor_count++] = (struct ui_node_desc){
            .kind = UI_NODE_INPUT,
            .key = row->input_key,
            .class_name = "text-input",
            .tag_name = "",
            .id_name = key,
            .props.input = {
                .placeholder = "Default",
                .accessible_name = row->label,
            },
        };
    }
    if (row->choice || row->combo) {
        row->editor_children[editor_count++] = (struct ui_node_desc){
            .kind = UI_NODE_DROPDOWN,
            .key = row->dropdown_key,
            .class_name = "dropdown",
            .tag_name = "",
            .id_name = row->choice ? key : NULL,
            .props.dropdown = {
                .items = row->initial_items,
                .count = row->initial_item_count,
                .accessible_name = row->label,
                .overlay_key = "root",
            },
        };
    }
    row->row_children[1] = (struct ui_node_desc){
        .kind = UI_NODE_COLUMN,
        .class_name = "settings-editor",
        .tag_name = "",
        .children = row->editor_children,
        .child_count = editor_count,
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
    };
    row->node = (struct ui_node_desc){
        .kind = UI_NODE_ROW,
        .key = row->row_id,
        .class_name = "settings-row",
        .tag_name = "",
        .id_name = row->row_id,
        .children = row->row_children,
        .child_count = 2,
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
    };
    return true;
}

static void form_destroy(struct settings_form *form)
{
    ui_view_destroy(form->view);
    for (size_t i = 0; i < form->count; i++)
        free_choices(&form->rows[i], form->rows[i].initial_items,
                     form->rows[i].initial_item_count);
    free(form->field_nodes);
    free(form->rows);
    free(form);
}

static bool add_field_descriptions(struct settings_form *form)
{
    size_t count = CATEGORY_COUNT - 1 + form->count + 1;
    form->field_nodes = calloc(count, sizeof(*form->field_nodes));
    if (!form->field_nodes)
        return false;
    size_t at = 0;
    for (int category = 1; category < CATEGORY_COUNT; category++) {
        form->field_nodes[at++] = (struct ui_node_desc){
            .kind = UI_NODE_LABEL,
            .key = section_keys[category],
            .class_name = "settings-section",
            .tag_name = "",
            .props.label = {
                .text = categories[category],
                .wrap = UI_LABEL_WRAP_WORD,
                .align = UI_LABEL_ALIGN_LEFT,
            },
        };
        for (int combo = 1; combo >= 0; combo--)
            for (size_t i = 0; i < form->count; i++)
                if (form->rows[i].category == (enum category)category && form->rows[i].combo == combo)
                    form->field_nodes[at++] = form->rows[i].node;
    }
    form->field_nodes[at++] = (struct ui_node_desc){
        .kind = UI_NODE_LABEL,
        .key = "settings-empty",
        .props.label = {
            .text = "No matching settings. Try another search.",
            .wrap = UI_LABEL_WRAP_WORD,
            .align = UI_LABEL_ALIGN_LEFT,
        },
    };
    return at == count;
}

static bool mount_form(struct settings_form *form)
{
    struct ui_node_desc header_nodes[] = {
        {
            .kind = UI_NODE_LABEL,
            .key = "title",
            .class_name = "settings-title",
            .tag_name = "",
            .props.label = {
                .text = "Settings",
                .wrap = UI_LABEL_WRAP_WORD,
                .align = UI_LABEL_ALIGN_LEFT,
            },
        },
        {
            .kind = UI_NODE_BUTTON,
            .key = "config-link",
            .class_name = "settings-button settings-link",
            .tag_name = "",
            .id_name = "config-link",
            .action_id = OPEN_CONFIG,
            .props.button = { .text = "Config file" },
        },
        {
            .kind = UI_NODE_LABEL,
            .key = "font-link",
            .class_name = "settings-button settings-link",
            .tag_name = "",
            .id_name = "font-link",
            .action_id = FONT_HELP,
            .props.label = {
                .text = "Font help",
                .link = "https://www.freedesktop.org/wiki/Software/fontconfig/",
                .wrap = UI_LABEL_WRAP_WORD,
                .align = UI_LABEL_ALIGN_LEFT,
            },
        },
        {
            .kind = UI_NODE_LABEL,
            .key = "color-link",
            .class_name = "settings-button settings-link",
            .tag_name = "",
            .id_name = "color-link",
            .action_id = COLOR_HELP,
            .props.label = {
                .text = "Color help",
                .link = "https://en.wikipedia.org/wiki/ANSI_escape_code#Colors",
                .wrap = UI_LABEL_WRAP_WORD,
                .align = UI_LABEL_ALIGN_LEFT,
            },
        },
    };
    struct ui_node_desc nav_nodes[CATEGORY_COUNT];
    for (int i = 0; i < CATEGORY_COUNT; i++) {
        nav_nodes[i] = (struct ui_node_desc){
            .kind = UI_NODE_BUTTON,
            .key = category_ids[i],
            .class_name = "settings-button settings-nav",
            .tag_name = "",
            .id_name = category_ids[i],
            .action_id = CATEGORY_BASE + i,
            .props.button = { .text = categories[i] },
        };
    }
    struct ui_node_desc sidebar = {
        .kind = UI_NODE_COLUMN,
        .key = "sidebar",
        .class_name = "settings-sidebar",
        .tag_name = "",
        .children = nav_nodes,
        .child_count = CATEGORY_COUNT,
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
    };
    struct ui_node_desc scroll_child = {
        .kind = UI_NODE_COLUMN,
        .key = "fields",
        .class_name = "settings-fields",
        .tag_name = "",
        .children = form->field_nodes,
        .child_count = CATEGORY_COUNT - 1 + form->count + 1,
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
    };
    struct ui_node_desc scroll_children[] = { scroll_child };
    struct ui_node_desc scroll = {
        .kind = UI_NODE_SCROLLVIEW,
        .key = "scroll",
        .class_name = "settings-scroll",
        .tag_name = "",
        .children = scroll_children,
        .child_count = 1,
        .props.scrollview = { .scrollbar = true },
    };
    struct ui_node_desc body_nodes[] = { sidebar, scroll };
    struct ui_node_desc footer_nodes[] = {
        {
            .kind = UI_NODE_LABEL,
            .key = "status",
            .class_name = "settings-status",
            .tag_name = "",
            .id_name = "settings-status",
            .props.label = {
                .text = "",
                .wrap = UI_LABEL_WRAP_WORD,
                .align = UI_LABEL_ALIGN_LEFT,
                .selectable = true,
            },
        },
        {
            .kind = UI_NODE_BUTTON,
            .key = "reset",
            .class_name = "settings-button",
            .tag_name = "",
            .id_name = "settings-reset",
            .action_id = RESET,
            .props.button = { .text = "Reset all" },
        },
        {
            .kind = UI_NODE_BUTTON,
            .key = "cancel",
            .class_name = "settings-button",
            .tag_name = "",
            .id_name = "settings-cancel",
            .action_id = CANCEL,
            .props.button = { .text = "Cancel" },
        },
        {
            .kind = UI_NODE_BUTTON,
            .key = "save",
            .class_name = "settings-button settings-primary",
            .tag_name = "",
            .id_name = "settings-save",
            .action_id = SAVE,
            .props.button = { .text = "Save" },
        },
    };
    struct ui_node_desc layout_nodes[] = {
        {
            .kind = UI_NODE_ROW,
            .key = "header",
            .class_name = "settings-header",
            .tag_name = "",
            .children = header_nodes,
            .child_count = sizeof(header_nodes) / sizeof(header_nodes[0]),
            .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
        },
        {
            .kind = UI_NODE_INPUT,
            .key = "search",
            .class_name = "text-input settings-search",
            .tag_name = "",
            .id_name = "settings-search",
            .props.input = {
                .placeholder = "Search settings... (Ctrl+F)",
                .accessible_name = "Search settings",
            },
        },
        {
            .kind = UI_NODE_ROW,
            .key = "body",
            .class_name = "settings-body",
            .tag_name = "",
            .children = body_nodes,
            .child_count = sizeof(body_nodes) / sizeof(body_nodes[0]),
            .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
        },
        {
            .kind = UI_NODE_ROW,
            .key = "footer",
            .class_name = "settings-footer",
            .tag_name = "",
            .children = footer_nodes,
            .child_count = sizeof(footer_nodes) / sizeof(footer_nodes[0]),
            .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
        },
    };
    struct ui_node_desc root_children[] = {
        {
            .kind = UI_NODE_COLUMN,
            .key = "layout",
            .class_name = "settings-form",
            .tag_name = "",
            .children = layout_nodes,
            .child_count = sizeof(layout_nodes) / sizeof(layout_nodes[0]),
            .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
        },
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_STACK,
        .key = "root",
        .tag_name = "",
        .children = root_children,
        .child_count = sizeof(root_children) / sizeof(root_children[0]),
        .props.layout = { .align = UI_BOX_ALIGN_STRETCH },
        .on_action = form_action,
        .action_userdata = form,
        .on_event = root_event,
        .event_userdata = form,
    };
    enum ui_view_error error;
    form->view = ui_view_mount(&root, &error);
    if (!form->view)
        return false;

    form->root = ui_view_root(form->view);
    form->layout = ui_view_box(form->view, "layout");
    form->header = ui_view_box(form->view, "header");
    form->body = ui_view_box(form->view, "body");
    form->sidebar = ui_view_box(form->view, "sidebar");
    form->fields = ui_view_box(form->view, "fields");
    form->footer = ui_view_box(form->view, "footer");
    form->search = ui_view_input(form->view, "search");
    form->scroll = ui_view_scrollview(form->view, "scroll");
    form->title = ui_view_label(form->view, "title");
    form->status = ui_view_label(form->view, "status");
    form->empty = ui_view_label(form->view, "settings-empty");
    form->font_help = ui_view_label(form->view, "font-link");
    form->color_help = ui_view_label(form->view, "color-link");
    form->config = ui_view_button(form->view, "config-link");
    form->save = ui_view_button(form->view, "save");
    form->cancel = ui_view_button(form->view, "cancel");
    form->reset = ui_view_button(form->view, "reset");
    for (int i = 0; i < CATEGORY_COUNT; i++)
        form->nav[i] = ui_view_button(form->view, category_ids[i]);
    for (int i = 1; i < CATEGORY_COUNT; i++)
        form->section[i] = ui_view_label(form->view, section_keys[i]);
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        row->row = ui_view_box(form->view, row->row_id);
        row->error = ui_view_label(form->view, row->error_key);
        if (!row->choice)
            row->input = ui_view_input(form->view, row->input_key);
        if (row->choice || row->combo)
            row->dropdown = ui_view_dropdown(form->view, row->dropdown_key);
    }
    bool ok = form->root && form->layout && form->header && form->body &&
        form->sidebar && form->fields && form->footer && form->search &&
        form->scroll && form->title && form->status && form->empty &&
        form->font_help && form->color_help && form->config && form->save &&
        form->cancel && form->reset;
    for (int i = 0; i < CATEGORY_COUNT; i++)
        ok = ok && form->nav[i];
    for (int i = 1; i < CATEGORY_COUNT; i++)
        ok = ok && form->section[i];
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        ok = ok && row->row && row->error &&
            (row->choice || row->input) &&
            (!(row->choice || row->combo) || row->dropdown);
    }
    if (!ok)
        return false;
    for (size_t i = 0; i < form->count; i++) {
        struct settings_row *row = &form->rows[i];
        free_choices(row, row->initial_items, row->initial_item_count);
        row->initial_items = NULL;
        row->initial_item_count = 0;
        ui_widget_set_hidden(ui_label_widget(row->error), true);
    }
    return true;
}

static struct settings_form *form_create(const struct settings_host *host)
{
    struct settings_form *form = calloc(1, sizeof(*form));
    if (!form) return NULL;
    form->host = *host;
    form->count = opts_option_count(&app_config_spec);
    form->rows = calloc(form->count, sizeof(*form->rows));
    if (!form->rows) { free(form); return NULL; }
    for (size_t i = 0; i < form->count; i++)
        if (!init_row(&form->rows[i], i, host->look)) {
            form_destroy(form);
            return NULL;
        }
    if (!add_field_descriptions(form) || !mount_form(form)) {
        form_destroy(form);
        return NULL;
    }
    return form;
}

static void op_destroy(struct window *window)
{
    form_destroy(window_ui_content(window));
}

static bool settings_capture(struct ui_widget *widget,
                             const struct ui_event *event, void *userdata)
{
    (void)widget;
    return handle_root_event(userdata, event);
}

static const int settings_kind;

static struct window_ui_spec settings_spec(struct settings_form *form)
{
    return (struct window_ui_spec){
        .name = "settings",
        .kind = &settings_kind,
        .root = ui_view_root(form->view),
        .ui_font_size = FONT_DEFAULT_SIZE_PT,
        .restyle = settings_restyle,
        .destroy = op_destroy,
        .content = form,
    };
}

bool settings_mount(struct window *w, const struct settings_host *host)
{
    struct settings_form *form = form_create(host);
    if (!form)
        return false;
    struct window_ui_spec spec_ui = settings_spec(form);
    if (!window_ui_mount(w, &spec_ui)) {
        form_destroy(form);
        return false;
    }
    form->window = w;
    ui_widget_set_key_capture(ui_view_root(form->view), settings_capture, form);
    ui_surface_set_default_focus(window_ui_surface(w), ui_view_root(form->view));
    load_values(form, form->host.config, true);
    return true;
}

struct window *settings_find(struct window_host *host)
{
    for (int i = 0; i < host->count; i++)
        if (window_ui_kind(host->windows[i]) == &settings_kind)
            return host->windows[i];
    return NULL;
}
