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

/* The core keeps at most this many bytes of a title. */
#define TITLE_BYTES_MAX 4096

static char *title_copy(const char *title)
{
    size_t n = strlen(title);
    if (n > TITLE_BYTES_MAX) {
        n = TITLE_BYTES_MAX;
        /* Do not cut a UTF-8 sequence. */
        while (n && ((unsigned char)title[n] & 0xC0) == 0x80)
            n--;
    }
    char *copy = malloc(n + 1);
    if (copy) {
        memcpy(copy, title, n);
        copy[n] = '\0';
    }
    return copy;
}

void wsap_vtcore_title_set(struct wsap_vtcore *t, const char *title)
{
    if (!t->cb.title)
        return;
    /* On allocation failure, a later push saves an empty title. */
    free(t->title);
    t->title = title_copy(title);
    t->cb.title(t->cb.ud, title);
}

void wsap_vtcore_title_push(struct wsap_vtcore *t)
{
    if (!t->cb.title)
        return;
    if (t->title_depth == TITLE_STACK_SIZE) {
        free(t->title_stack[0]);
        memmove(t->title_stack, t->title_stack + 1,
                (TITLE_STACK_SIZE - 1) * sizeof(t->title_stack[0]));
        t->title_depth--;
    }
    /* A null entry is an empty title. The depth stays in step on failure. */
    t->title_stack[t->title_depth++] = t->title ? wsap_vtcore_strdup(t->title) : NULL;
}

void wsap_vtcore_title_pop(struct wsap_vtcore *t)
{
    if (!t->title_depth)
        return;
    free(t->title);
    t->title = t->title_stack[--t->title_depth];
    t->title_stack[t->title_depth] = NULL;
    t->cb.title(t->cb.ud, t->title ? t->title : "");
}

void wsap_vtcore_title_stack_clear(struct wsap_vtcore *t)
{
    while (t->title_depth) {
        t->title_depth--;
        free(t->title_stack[t->title_depth]);
        t->title_stack[t->title_depth] = NULL;
    }
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
