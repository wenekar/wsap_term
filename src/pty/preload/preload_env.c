#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "pty/preload/shim.h"

typedef int (*fp_token_cb)(const char *tok, size_t len, void *ud);

static int preload_each_token(const char *list, fp_token_cb cb, void *ud)
{
    while (*list) {
        list += strspn(list, ": ");
        size_t n = strcspn(list, ": ");
        if (n && cb(list, n, ud))
            return 1;
        list += n;
    }
    return 0;
}

struct preload_match {
    const char *token;
    size_t len;
};

static int preload_match_cb(const char *tok, size_t n, void *ud)
{
    const struct preload_match *m = ud;
    return n == m->len && !memcmp(tok, m->token, n);
}

static int preload_contains(const char *list, const char *token, size_t len)
{
    struct preload_match m = { token, len };
    return preload_each_token(list, preload_match_cb, &m);
}

struct preload_strip {
    char *p;
    char *list;
};

static int preload_strip_cb(const char *tok, size_t n, void *ud)
{
    struct preload_strip *s = ud;
    if (preload_contains(fp_preload, tok, n))
        return 0;
    if (s->p != s->list)
        *s->p++ = ':';
    memcpy(s->p, tok, n);
    s->p += n;
    *s->p = '\0';
    return 0;
}

int fp_env_has_preload(char *const envp[])
{
    for (size_t i = 0; envp && envp[i]; i++)
        if (!strncmp(envp[i], "LD_PRELOAD=", sizeof("LD_PRELOAD=") - 1))
            return 1;
    return 0;
}

int fp_env_disables_preload(char *const envp[])
{
    static const char disabled[] = "WSAP_NO_PRELOAD=1";
    for (size_t i = 0; envp && envp[i]; i++)
        if (!strcmp(envp[i], disabled))
            return 1;
    return 0;
}

struct preload_merge {
    char *p;
    char *list;
};

static int preload_merge_cb(const char *tok, size_t n, void *ud)
{
    struct preload_merge *m = ud;
    if (!preload_contains(m->list, tok, n)) {
        if (m->p != m->list)
            *m->p++ = ':';
        memcpy(m->p, tok, n);
        m->p += n;
        *m->p = '\0';
    }
    return 0;
}

static char **env_copy_without_preload(char *const envp[],
                                       const char **existing_out)
{
    size_t count = 0;
    while (envp && envp[count]) count++;
    char **env = calloc(count + 2, sizeof(*env));
    if (!env)
        return NULL;
    const char *existing = NULL;
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strncmp(envp[i], "LD_PRELOAD=", sizeof("LD_PRELOAD=") - 1)) {
            if (!existing)
                existing = envp[i] + sizeof("LD_PRELOAD=") - 1;
        } else {
            env[out++] = envp[i];
        }
    }
    *existing_out = existing;
    return env;
}

char **fp_env_with_preload(char *const envp[], char **entry_out)
{
    const char *existing;
    char **env = env_copy_without_preload(envp, &existing);
    if (!env)
        return NULL;
    size_t out = 0;
    while (env[out]) out++;

    size_t preload_len = strlen(fp_preload);
    size_t existing_len = existing ? strlen(existing) : 0;
    size_t entry_len = sizeof("LD_PRELOAD=") + preload_len + existing_len + 32;
    char *entry = malloc(entry_len);
    if (!entry) {
        free(env);
        return NULL;
    }
    char *p = entry;
    memcpy(p, "LD_PRELOAD=", sizeof("LD_PRELOAD=") - 1);
    p += sizeof("LD_PRELOAD=") - 1;
    char *list = p;
    if (existing_len) {
        memcpy(p, existing, existing_len);
        p += existing_len;
    }
    *p = '\0';
    struct preload_merge merge = { .p = p, .list = list };
    preload_each_token(fp_preload, preload_merge_cb, &merge);

    env[out] = entry;
    *entry_out = entry;
    return env;
}

char **fp_env_without_fast_preload(char *const envp[], char **entry_out)
{
    const char *existing;
    char **env = env_copy_without_preload(envp, &existing);
    if (!env)
        return NULL;
    size_t out = 0;
    while (env[out]) out++;

    size_t existing_len = existing ? strlen(existing) : 0;
    char *entry = malloc(sizeof("LD_PRELOAD=") + existing_len);
    if (!entry) {
        free(env);
        return NULL;
    }
    memcpy(entry, "LD_PRELOAD=", sizeof("LD_PRELOAD=") - 1);
    struct preload_strip strip = {
        .p = entry + sizeof("LD_PRELOAD=") - 1,
        .list = entry + sizeof("LD_PRELOAD=") - 1,
    };
    *strip.p = '\0';
    if (existing_len)
        preload_each_token(existing, preload_strip_cb, &strip);
    env[out] = entry;
    *entry_out = entry;
    return env;
}
