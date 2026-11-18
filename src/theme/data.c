#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "theme/internal.h"
#include "util/file.h"
#include "util/grow.h"
#include "util/name.h"
#include "util/str.h"

static void data_warning(const char *path, int line, const char *message,
                         const char *detail)
{
    if (detail)
        fprintf(stderr, "[%s] %s:%d: %s '%s'\n", app_name(), path, line,
                message, detail);
    else
        fprintf(stderr, "[%s] %s:%d: %s\n", app_name(), path, line, message);
}

static bool data_set(struct theme_data *data, const char *block,
                     const char *key, const char *value)
{
    for (int i = 0; i < data->count; i++) {
        struct theme_value *item = &data->items[i];
        if (strcmp(item->block, block) || strcmp(item->key, key))
            continue;
        char *copy = strdup(value);
        if (!copy)
            return false;
        free(item->value);
        item->value = copy;
        return true;
    }
    if (!UTIL_GROW(data->items, data->cap, data->count + 1))
        return false;
    struct theme_value *item = &data->items[data->count];
    *item = (struct theme_value){
        .block = strdup(block),
        .key = strdup(key),
        .value = strdup(value),
    };
    if (!item->block || !item->key || !item->value) {
        free(item->block);
        free(item->key);
        free(item->value);
        return false;
    }
    data->count++;
    return true;
}

static enum theme_polarity parse_polarity(const char *value)
{
    if (!strcasecmp(value, "dark"))
        return THEME_POLARITY_DARK;
    if (!strcasecmp(value, "light"))
        return THEME_POLARITY_LIGHT;
    return THEME_POLARITY_UNKNOWN;
}

bool theme_data_load(const char *path, struct theme_data *data,
                     struct look_theme *meta)
{
    char *text = util_read_file(path, app_name());
    if (!text)
        return false;
    char *section = NULL;
    int line = 0;
    for (char *p = text; p && *p;) {
        char *end = strchr(p, '\n');
        if (end)
            *end = '\0';
        line++;
        char *entry = str_trim(p);
        if (*entry && *entry != '#') {
            if (*entry == '[') {
                char *close = strchr(entry + 1, ']');
                if (!close || close[1] != '\0') {
                    data_warning(path, line, "bad data block header", entry);
                } else {
                    *close = '\0';
                    section = str_trim(entry + 1);
                    if (!*section)
                        data_warning(path, line, "empty data block name", NULL);
                }
            } else {
                char *eq = strchr(entry, '=');
                if (!eq) {
                    data_warning(path, line, "missing value for key", entry);
                } else {
                    *eq = '\0';
                    char *key = str_trim(entry);
                    char *value = str_trim(eq + 1);
                    if (section) {
                        if (!data_set(data, section, key, value))
                            break;
                    } else if (!strcmp(key, "label")) {
                        char *copy = strdup(value);
                        if (!copy)
                            break;
                        free(meta->label);
                        meta->label = copy;
                    } else if (!strcmp(key, "polarity")) {
                        meta->polarity = parse_polarity(value);
                        if (meta->polarity == THEME_POLARITY_UNKNOWN)
                            data_warning(path, line, "bad polarity", value);
                    } else {
                        data_warning(path, line, "unknown theme metadata key",
                                     key);
                    }
                }
            }
        }
        p = end ? end + 1 : NULL;
    }
    free(text);
    return true;
}

bool theme_data_copy(struct theme_data *dst, const struct theme_data *src)
{
    if (!src)
        return true;
    for (int i = 0; i < src->count; i++) {
        const struct theme_value *item = &src->items[i];
        if (!data_set(dst, item->block, item->key, item->value))
            return false;
    }
    return true;
}

const char *theme_data_value(const struct theme_data *data, const char *block,
                             const char *key)
{
    if (!data || !block || !key)
        return NULL;
    for (int i = 0; i < data->count; i++) {
        const struct theme_value *item = &data->items[i];
        if (!strcmp(item->block, block) && !strcmp(item->key, key))
            return item->value;
    }
    return NULL;
}

void theme_data_clear(struct theme_data *data)
{
    if (!data)
        return;
    for (int i = 0; i < data->count; i++) {
        free(data->items[i].block);
        free(data->items[i].key);
        free(data->items[i].value);
    }
    free(data->items);
    *data = (struct theme_data){0};
}
