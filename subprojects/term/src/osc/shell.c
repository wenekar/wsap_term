#include "osc.h"
#include "../internal.h"
#include "../marks.h"
#include <stdlib.h>
#include <string.h>

/* Shell integration: OSC 133 marks, ConEmu OSC 9;4 and 9;9, and a subset
 * of iTerm2 OSC 1337. */

/* The core keeps at most this many bytes of a path from OSC 9;9 or 1337. */
#define SHELL_PATH_MAX 4096
/* Limits of a user variable (OSC 1337;SetUserVar). The core ignores a
 * variable with a longer name or value. */
#define SHELL_VAR_NAME_MAX 255
#define SHELL_VAR_VALUE_MAX 4096
#define SHELL_VAR_ENCODED_MAX (((SHELL_VAR_VALUE_MAX + 2) / 3) * 4)

/* Read a decimal field that ends at ';' or at the end. An empty field gives
 * -1. Return false for other characters. Large values stop at 1000000000. */
static bool shell_field(const char **pp, long *out)
{
    const char *p = *pp;
    long v = -1;
    for (; *p >= '0' && *p <= '9'; p++)
        v = v < 0 ? *p - '0' : (v >= 100000000 ? 1000000000 : v * 10 + (*p - '0'));
    if (*p && *p != ';')
        return false;
    *pp = p;
    *out = v;
    return true;
}

/* Is A a secondary prompt? Kitty sends k=s for continuation lines. Only the
 * k=i form, or no k option, starts a prompt. */
static bool shell_secondary(const char *opts)
{
    while (*opts) {
        const char *end = strchr(opts, ';');
        size_t n = end ? (size_t)(end - opts) : strlen(opts);
        if (n >= 2 && opts[0] == 'k' && opts[1] == '=' && !(n == 3 && opts[2] == 'i'))
            return true;
        opts += n;
        if (*opts == ';')
            opts++;
    }
    return false;
}

/* The exit status of OSC 133;D: an optional '-' and decimal digits that fit
 * in 32 bits. */
static bool shell_status(const char *s, int32_t *out)
{
    bool neg = *s == '-';
    s += neg;
    if (*s < '0' || *s > '9')
        return false;
    int64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        if (v > (int64_t)INT32_MAX + 1)
            return false;
    }
    if (*s && *s != ';')
        return false;
    v = neg ? -v : v;
    if (v > INT32_MAX)
        return false;
    *out = (int32_t)v;
    return true;
}

void wsap_vtcore_osc133(struct wsap_vtcore *t, const char *params)
{
    char kind = params[0];
    if (t->alternate || !kind || (params[1] && params[1] != ';'))
        return;
    const char *opts = params[1] ? params + 2 : "";
    switch (kind) {
    case 'A':
        if (shell_secondary(opts))
            break;
        t->prompt_y = t->cursor_y;
        t->has_prompt = true;
        wsap_vtcore_marks_set(t, RF_PROMPT);
        break;
    case 'C':
        t->prompt_y = -1;
        t->has_prompt = false;
        wsap_vtcore_marks_set(t, RF_OUTPUT);
        break;
    case 'D': {
        t->prompt_y = -1;
        t->has_prompt = false;
        int32_t status = 0;
        bool has_status = shell_status(opts, &status);
        wsap_vtcore_marks_finish(t, has_status, status);
        break;
    }
    default:
        /* B (command input start) needs no state. */
        break;
    }
}

static void shell_set_cwd(struct wsap_vtcore *t, const char *path)
{
    size_t n = strlen(path);
    if (n >= 2 && path[0] == '"' && path[n - 1] == '"') {
        path++;
        n -= 2;
    }
    if (!n || n > SHELL_PATH_MAX)
        return;
    char *cwd = malloc(n + 1);
    if (!cwd)
        return;
    memcpy(cwd, path, n);
    cwd[n] = '\0';
    free(t->cwd);
    t->cwd = cwd;
}

/* OSC 9;4;state;percent. */
static void shell_progress(struct wsap_vtcore *t, const char *p)
{
    long state = 0, percent = -1;
    if (*p == ';') {
        p++;
        if (!shell_field(&p, &state))
            return;
        if (state < 0)
            state = 0;
    }
    if (*p == ';') {
        p++;
        if (!shell_field(&p, &percent))
            return;
    }
    if (state > WSAP_VTCORE_PROGRESS_PAUSED || !t->cb.progress)
        return;
    if (state == WSAP_VTCORE_PROGRESS_REMOVE || state == WSAP_VTCORE_PROGRESS_INDETERMINATE)
        percent = -1;
    else if (state == WSAP_VTCORE_PROGRESS_NORMAL && percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    t->cb.progress(t->cb.ud, (enum wsap_vtcore_progress)state, (int)percent);
}

bool wsap_vtcore_osc9_conemu(struct wsap_vtcore *t, const char *params)
{
    if (params[0] == '4' && (!params[1] || params[1] == ';')) {
        shell_progress(t, params + 1);
        return true;
    }
    if (params[0] == '9' && (!params[1] || params[1] == ';')) {
        if (params[1])
            shell_set_cwd(t, params + 2);
        return true;
    }
    return false;
}

/* OSC 1337;SetUserVar=name=base64. */
static void shell_user_var(struct wsap_vtcore *t, const char *p)
{
    const char *eq = strchr(p, '=');
    if (!eq || eq == p || (size_t)(eq - p) > SHELL_VAR_NAME_MAX || !t->cb.user_var)
        return;
    const char *enc = eq + 1;
    size_t enc_len = strlen(enc);
    if (enc_len > SHELL_VAR_ENCODED_MAX)
        return;
    char name[SHELL_VAR_NAME_MAX + 1];
    char value[(SHELL_VAR_ENCODED_MAX / 4 + 1) * 3 + 1];
    size_t name_len = (size_t)(eq - p), len;
    memcpy(name, p, name_len);
    name[name_len] = '\0';
    if (!wsap_vtcore_b64_decode(enc, enc_len, value, &len) || len > SHELL_VAR_VALUE_MAX)
        return;
    value[len] = '\0';
    t->cb.user_var(t->cb.ud, name, value, len);
}

void wsap_vtcore_osc1337(struct wsap_vtcore *t, const char *params)
{
    if (!strncmp(params, "CurrentDir=", 11))
        shell_set_cwd(t, params + 11);
    else if (!strncmp(params, "SetUserVar=", 11))
        shell_user_var(t, params + 11);
}
