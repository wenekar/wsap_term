#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme/internal.h"
#include "util/file.h"
#include "util/app_identity.h"

/* The log prefix of the file that parses now. Loading is one thread. */
static const char *load_tag = "";

static void css_warning(const struct css_diagnostic *diagnostic, void *userdata)
{
    const char *path = userdata;
    fprintf(stderr, "[%s] %s:%zu:%zu: %s '%s: %s'\n", load_tag, path,
            diagnostic->line, diagnostic->column,
            css_diagnostic_message(diagnostic->kind),
            diagnostic->property, diagnostic->value);
}

struct appearance *appearance_load(const struct app_identity *identity,
                                   const char *name, const char *css_path,
                                   const struct theme_data *data)
{
    struct appearance *ap = calloc(1, sizeof(*ap));
    if (!ap)
        return NULL;
    if (identity && identity->id[0])
        load_tag = identity->id;
    ap->refs = 1;
    if (!theme_data_copy(&ap->data, data)) {
        appearance_unref(ap);
        return NULL;
    }
    if (name && !(ap->name = strdup(name))) {
        appearance_unref(ap);
        return NULL;
    }
    /* The theme rules layer over the toolkit and app rules that the caller
       holds. The sheet holds no core rule. */
    if (css_path) {
        char *text = util_read_file(css_path, load_tag);
        if (text && !css_parse_deferred(&ap->chrome, text, css_warning,
                                        (void *)css_path))
            fprintf(stderr, "[%s] cannot parse CSS file %s\n", load_tag,
                    css_path);
        free(text);
    }
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
    theme_data_clear(&ap->data);
    free(ap->name);
    free(ap);
}
