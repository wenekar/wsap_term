#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme/internal.h"
#include "util/config.h"
#include "util/file.h"
#include "util/name.h"

static void conf_line(void *ctx, const char *path, int line,
                      const char *k, const char *v)
{
    if (!v || !strcmp(k, "label"))
        return;
    if (!wsap_vtcore_theme_set_key(ctx, k, v))
        fprintf(stderr, "[%s] %s:%d: bad palette entry '%s = %s'\n",
                app_name(), path, line, k, v);
}

struct appearance *appearance_load(const char *name, const char *css_path,
                                   const char *conf_path,
                                   const struct wsap_vtcore_theme *base)
{
    struct appearance *ap = calloc(1, sizeof(*ap));
    if (!ap)
        return NULL;
    ap->refs = 1;
    if (base)
        ap->palette = *base;
    else
        wsap_vtcore_theme_default(&ap->palette);
    css_init_default(&ap->chrome);
    if (name && !(ap->name = strdup(name))) {
        appearance_unref(ap);
        return NULL;
    }
    if (css_path) {
        char *text = util_read_file(css_path, app_name());
        if (text && !css_parse(&ap->chrome, text))
            fprintf(stderr, "[%s] cannot parse CSS file %s\n", app_name(), css_path);
        free(text);
    }
    if (conf_path)
        config_parse_file(conf_path, conf_line, &ap->palette);
    return ap;
}

struct appearance *appearance_ref(struct appearance *ap)
{
    if (ap)
        ap->refs++;
    return ap;
}

void appearance_unref(struct appearance *ap)
{
    if (!ap || --ap->refs > 0)
        return;
    css_sheet_clear(&ap->chrome);
    free(ap->name);
    free(ap);
}
