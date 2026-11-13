#define _GNU_SOURCE
#include "app/config/settings/choices.h"

#include "app/config/config.h"
#include "font/font.h"
#include "platform/platform.h"
#include "theme/look.h"
#include "util/options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int compare_names(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
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

static bool build_spec(struct settings_choices *choices, const char *key,
                       const char *antialias, bool filter_hinting)
{
    size_t count = 0;
    while (opts_choice_at(&app_config_spec, key, count))
        count++;
    struct ui_dropdown_item *items = calloc(count ? count : 1, sizeof(*items));
    if (!items)
        return false;
    choices->items = items;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        const char *value = opts_choice_at(&app_config_spec, key, i);
        if (filter_hinting && !hinting_allowed(antialias, value))
            continue;
        const char *label = !strcmp(value, "true") ? "On" :
            !strcmp(value, "false") ? "Off" : value;
        items[kept++] = (struct ui_dropdown_item){ label, value };
    }
    choices->count = kept;
    return true;
}

static bool build_font(struct settings_choices *choices)
{
    size_t string_count = 0;
    char **strings = font_list_families(&string_count);
    if (strings)
        qsort(strings, string_count, sizeof(*strings), compare_names);
    choices->strings = strings;
    choices->string_count = string_count;
    struct ui_dropdown_item *items = calloc(string_count + 2, sizeof(*items));
    if (!items)
        return false;
    choices->items = items;
    items[0] = (struct ui_dropdown_item){ "Default font", "" };
    items[1] = (struct ui_dropdown_item){ "System monospace", "monospace" };
    for (size_t i = 0; i < string_count; i++)
        items[i + 2] = (struct ui_dropdown_item){ strings[i], strings[i] };
    choices->count = string_count + 2;
    return true;
}

static bool build_locale(struct settings_choices *choices)
{
    size_t string_count = 0;
    char **strings = platform_locales_list(&string_count);
    if (strings)
        qsort(strings, string_count, sizeof(*strings), compare_names);
    choices->strings = strings;
    choices->string_count = string_count;
    char default_locale[128], label[160];
    platform_locale_default(default_locale, sizeof(default_locale));
    snprintf(label, sizeof(label), "Default (%s)", default_locale);
    choices->heading = strdup(label);
    if (!choices->heading)
        return false;
    struct ui_dropdown_item *items = calloc(string_count + 1, sizeof(*items));
    if (!items)
        return false;
    choices->items = items;
    items[0] = (struct ui_dropdown_item){ choices->heading, "" };
    for (size_t i = 0; i < string_count; i++)
        items[i + 1] = (struct ui_dropdown_item){ strings[i], strings[i] };
    choices->count = string_count + 1;
    return true;
}

static bool build_theme(struct settings_choices *choices, struct look *look)
{
    size_t count = (size_t)look_theme_count(look) + 1;
    struct ui_dropdown_item *items = calloc(count, sizeof(*items));
    if (!items)
        return false;
    choices->items = items;
    items[0] = (struct ui_dropdown_item){ "Default", "" };
    for (size_t i = 1; i < count; i++) {
        const struct look_theme *theme = look_theme_at(look, (int)i - 1);
        items[i] = (struct ui_dropdown_item){ theme->label, theme->name };
    }
    choices->count = count;
    return true;
}

static bool fill_list(struct settings_choices *choices,
                      enum settings_choice_source source, const char *key,
                      struct look *look, const char *antialias)
{
    switch (source) {
    case SETTINGS_CHOICE_FONT:
        return build_font(choices);
    case SETTINGS_CHOICE_LOCALE:
        return build_locale(choices);
    case SETTINGS_CHOICE_THEME:
        return build_theme(choices, look);
    case SETTINGS_CHOICE_HINTING:
        return build_spec(choices, key, antialias, true);
    case SETTINGS_CHOICE_SPEC:
        break;
    }
    return build_spec(choices, key, antialias, false);
}

void settings_choices_destroy(struct settings_choices *choices)
{
    if (!choices)
        return;
    free(choices->heading);
    for (size_t i = 0; i < choices->string_count; i++)
        free(choices->strings[i]);
    free(choices->strings);
    free(choices->items);
    *choices = (struct settings_choices){0};
}

bool settings_choices_build(struct settings_choices *choices,
                            enum settings_choice_source source,
                            const char *key, struct look *look,
                            const char *antialias)
{
    struct settings_choices made = {0};
    if (!fill_list(&made, source, key, look, antialias)) {
        settings_choices_destroy(&made);
        return false;
    }
    settings_choices_destroy(choices);
    *choices = made;
    return true;
}
