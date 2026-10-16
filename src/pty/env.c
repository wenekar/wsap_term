#include "pty/internal.h"

#include <stdlib.h>
#include <string.h>

static bool env_reserve(struct pty_env *e, size_t n)
{
    if (e->failed)
        return false;
    if (n <= e->cap)
        return true;
    size_t cap = e->cap ? e->cap * 2 : 64;
    while (cap < n)
        cap *= 2;
    char **vars = realloc(e->vars, cap * sizeof(*vars));
    if (!vars) {
        e->failed = true;
        return false;
    }
    e->vars = vars;
    e->cap = cap;
    return true;
}

static size_t env_find(const struct pty_env *e, const char *name, size_t n)
{
    if (n > 0) {
        char first = name[0];
        for (size_t i = 0; i < e->len; i++)
            if (e->vars[i][0] == first &&
                !strncmp(e->vars[i], name, n) &&
                e->vars[i][n] == '=')
                return i;
        return e->len;
    }
    for (size_t i = 0; i < e->len; i++)
        if (!strncmp(e->vars[i], name, n) && e->vars[i][n] == '=')
            return i;
    return e->len;
}

static void env_insert(struct pty_env *e, char *entry, size_t n)
{
    if (!entry || !env_reserve(e, e->len + 2)) {
        free(entry);
        e->failed = true;
        return;
    }
    size_t i = env_find(e, entry, n);
    if (i < e->len) {
        free(e->vars[i]);
        e->vars[i] = entry;
        return;
    }
    e->vars[e->len++] = entry;
    e->vars[e->len] = NULL;
}

void pty_env_init(struct pty_env *e, char *const *base)
{
    *e = (struct pty_env){0};
    size_t n = 0;
    while (base && base[n])
        n++;
    if (!env_reserve(e, n + 1))
        return;
    for (size_t i = 0; i < n; i++) {
        char *copy = strdup(base[i]);
        if (!copy) {
            e->failed = true;
            break;
        }
        e->vars[e->len++] = copy;
    }
    e->vars[e->len] = NULL;
}

const char *pty_env_get(const struct pty_env *e, const char *name)
{
    size_t n = strlen(name);
    size_t i = env_find(e, name, n);
    return i < e->len ? e->vars[i] + n + 1 : NULL;
}

void pty_env_set(struct pty_env *e, const char *name, const char *value)
{
    if (e->failed)
        return;
    size_t n = strlen(name), v = strlen(value);
    char *entry = malloc(n + v + 2);
    if (entry) {
        memcpy(entry, name, n);
        entry[n] = '=';
        memcpy(entry + n + 1, value, v + 1);
    }
    env_insert(e, entry, n);
}

void pty_env_put(struct pty_env *e, const char *entry)
{
    const char *eq = strchr(entry, '=');
    if (e->failed || !eq || eq == entry)
        return;
    env_insert(e, strdup(entry), (size_t)(eq - entry));
}

void pty_env_unset(struct pty_env *e, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = env_find(e, name, n); i < e->len;
         i = env_find(e, name, n)) {
        free(e->vars[i]);
        memmove(&e->vars[i], &e->vars[i + 1],
                (e->len - i) * sizeof(*e->vars));
        e->len--;
    }
}

void pty_env_free(struct pty_env *e)
{
    for (size_t i = 0; i < e->len; i++)
        free(e->vars[i]);
    free(e->vars);
    *e = (struct pty_env){0};
}
