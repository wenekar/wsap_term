#include "osc.h"
#include "../internal.h"
#include <stdlib.h>
#include <string.h>

static void osc7_cwd(struct wsap_vtcore *t, const char *uri)
{
    if (!wsap_vtcore_has_prefix_ci(uri, "file://"))
        return;
    const char *p = uri + 7;
    const char *slash = strchr(p, '/');
    const char *path = slash ? slash : p;
    char *cwd = malloc(strlen(path) + 1);
    if (!cwd)
        return;
    char *d = cwd;
    while (*path) {
        int hi, lo;
        if (*path == '%' && (hi = wsap_vtcore_hex_nib(path[1])) >= 0 &&
            (lo = wsap_vtcore_hex_nib(path[2])) >= 0) {
            *d++ = (char)(hi * 16 + lo);
            path += 3;
        } else {
            *d++ = *path++;
        }
    }
    *d = '\0';
    free(t->cwd);
    t->cwd = cwd;
}

static void osc777_notify(struct wsap_vtcore *t, const char *params)
{
    if (strncmp(params, "notify;", 7))
        return;
    const char *title = params + 7;
    const char *sep = strchr(title, ';');
    if (!sep)
        return;
    char *wsep = (char *)sep;
    *wsep = '\0';
    t->cb.notify(t->cb.ud, title, sep + 1);
    *wsep = ';';
}

void wsap_vtcore_osc_misc_dispatch(struct wsap_vtcore *t, long code, const char *params)
{
    if (!params)
        return;
    switch (code) {
    case 7:
        osc7_cwd(t, params);
        break;
    case 9:
        if (t->cb.notify && strncmp(params, "4;", 2))
            t->cb.notify(t->cb.ud, t->name, params);
        break;
    case 22:
        free(t->pointer_shape);
        t->pointer_shape = *params ? wsap_vtcore_strdup(params) : NULL;
        break;
    case 777:
        if (t->cb.notify)
            osc777_notify(t, params);
        break;
    default:
        break;
    }
}
