#define _GNU_SOURCE

#include "util/options.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/config.h"
#include "util/fs.h"
#include "util/path.h"

static int util_parse_bool(const char *s)
{
    if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on") || !strcmp(s, "1"))
        return 1;
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
        !strcasecmp(s, "off") || !strcmp(s, "0"))
        return 0;
    return -1;
}

static bool util_parse_int(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end || errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static int failed_errno(void)
{
    return errno ? errno : EIO;
}

static FILE *temp_open(const char *path, char *temp, size_t size)
{
    int n = snprintf(temp, size, "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    int fd = mkstemp(temp);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        int err = errno;
        close(fd);
        unlink(temp);
        errno = err;
    }
    return f;
}

/* Syncs and closes F, then renames TEMP over PATH. On failure, removes
 * TEMP and sets errno. */
static bool temp_commit(FILE *f, const char *temp, const char *path)
{
    int err = 0;
    if (fflush(f) || fsync(fileno(f)))
        err = failed_errno();
    if (fclose(f) && !err)
        err = failed_errno();
    if (!err && rename(temp, path))
        err = failed_errno();
    if (err) {
        unlink(temp);
        errno = err;
        return false;
    }
    return true;
}

static void *field_of(const struct opt *o, void *base)
{
    return (char *)base + o->off;
}

static const void *field_of_const(const struct opt *o, const void *base)
{
    return (const char *)base + o->off;
}

static bool parse_text(const struct opt *o, const char *val, void *field)
{
    (void)o;
    char *copy = strdup(val);
    if (!copy)
        return false;
    free(*(char **)field);
    *(char **)field = copy;
    return true;
}

static void format_text(const struct opt *o, const void *field, char *buf, size_t n)
{
    const char *s = *(const char *const *)field;
    snprintf(buf, n, "%s", s ? s : o->example ? o->example : "");
}

static void release_text(void *field)
{
    free(*(char **)field);
    *(char **)field = NULL;
}

static bool parse_int(const struct opt *o, const char *val, void *field)
{
    int v;
    if (!util_parse_int(val, &v) || v < o->min || v > o->max)
        return false;
    *(int *)field = v;
    return true;
}

/* Zero, or min..max. */
static bool parse_size(const struct opt *o, const char *val, void *field)
{
    int v;
    if (!util_parse_int(val, &v) || (v && (v < o->min || v > o->max)))
        return false;
    *(int *)field = v;
    return true;
}

static void format_int(const struct opt *o, const void *field, char *buf, size_t n)
{
    (void)o;
    snprintf(buf, n, "%d", *(const int *)field);
}

static bool parse_bool(const struct opt *o, const char *val, void *field)
{
    (void)o;
    int b = util_parse_bool(val);
    if (b < 0)
        return false;
    *(bool *)field = b;
    return true;
}

static void format_bool(const struct opt *o, const void *field, char *buf, size_t n)
{
    (void)o;
    snprintf(buf, n, "%s", *(const bool *)field ? "true" : "false");
}

static bool parse_not_bool(const struct opt *o, const char *val, void *field)
{
    (void)o;
    int b = util_parse_bool(val);
    if (b < 0)
        return false;
    *(bool *)field = !b;
    return true;
}

static void format_not_bool(const struct opt *o, const void *field, char *buf,
                            size_t n)
{
    (void)o;
    snprintf(buf, n, "%s", *(const bool *)field ? "false" : "true");
}

static bool parse_enum(const struct opt *o, const char *val, void *field)
{
    for (const struct enum_name *e = o->names; e->name; e++) {
        if (!strcasecmp(val, e->name)) {
            *(int *)field = e->value;
            return true;
        }
    }
    return false;
}

static void format_enum(const struct opt *o, const void *field, char *buf, size_t n)
{
    int v = *(const int *)field;
    for (const struct enum_name *e = o->names; e->name; e++) {
        if (e->value == v) {
            snprintf(buf, n, "%s", e->name);
            return;
        }
    }
    snprintf(buf, n, "%d", v);
}

static const char *bool_choice(const struct opt *o, size_t index)
{
    (void)o;
    static const char *const choices[] = { "true", "false" };
    return index < 2 ? choices[index] : NULL;
}

/* Lists each value once. An alias repeats an earlier value. */
static const char *enum_choice(const struct opt *o, size_t index)
{
    for (const struct enum_name *e = o->names; e->name; e++) {
        bool alias = false;
        for (const struct enum_name *p = o->names; p < e; p++)
            alias |= p->value == e->value;
        if (!alias && !index--)
            return e->name;
    }
    return NULL;
}

const struct opt_type OPT_TYPE_TEXT = {
    .parse = parse_text,
    .format = format_text,
    .release = release_text,
    .size = sizeof(const char *),
    .kind = OPT_KIND_TEXT,
};

const struct opt_type OPT_TYPE_INT = {
    .parse = parse_int,
    .format = format_int,
    .size = sizeof(int),
    .kind = OPT_KIND_NUMBER,
};

const struct opt_type OPT_TYPE_SIZE = {
    .parse = parse_size,
    .format = format_int,
    .size = sizeof(int),
    .kind = OPT_KIND_NUMBER,
};

const struct opt_type OPT_TYPE_BOOL = {
    .parse = parse_bool,
    .format = format_bool,
    .size = sizeof(bool),
    .kind = OPT_KIND_CHOICE,
    .choice = bool_choice,
};

const struct opt_type OPT_TYPE_NOT_BOOL = {
    .parse = parse_not_bool,
    .format = format_not_bool,
    .size = sizeof(bool),
    .kind = OPT_KIND_CHOICE,
    .choice = bool_choice,
};

const struct opt_type OPT_TYPE_ENUM = {
    .parse = parse_enum,
    .format = format_enum,
    .size = sizeof(int),
    .kind = OPT_KIND_CHOICE,
    .choice = enum_choice,
};

static const struct opt *opt_find(const struct opts_spec *spec, const char *key,
                                  size_t len)
{
    if (!len) {
        for (size_t i = 0; i < spec->count; i++)
            if (!spec->table[i].key[0])
                return &spec->table[i];
        return NULL;
    }
    const char first = key[0];
    for (size_t i = 0; i < spec->count; i++) {
        const char *k = spec->table[i].key;
        if (k[0] == first && strlen(k) == len && !strncmp(k, key, len))
            return &spec->table[i];
    }
    return NULL;
}

/* The one way a value reaches a field. An empty value resets the key to
 * its default. On a bad value the field keeps its value. */
static bool opt_apply(const struct opts_spec *spec, void *base,
                      const struct opt *o, const char *val, const char *origin)
{
    void *field = field_of(o, base);
    if (!val[0]) {
        if (o->type->release)
            o->type->release(field);
        void *def = calloc(1, spec->size);
        if (!def)
            return false;
        spec->defaults(def);
        memcpy(field, field_of_const(o, def), o->type->size);
        free(def);
        return true;
    }
    if (o->type->parse(o, val, field))
        return true;
    fprintf(stderr, "[%s] %s: invalid value '%s' for '%s'\n",
            spec->name(), origin, val, o->key);
    return false;
}

static void usage(const struct opts_spec *spec, FILE *out)
{
    fprintf(out,
        "usage: %s [--config PATH] [--KEY VALUE]...\n"
        "  --config PATH      config file (also %s); NONE disables it\n"
        "  -h, --help         this text\n",
        spec->name(), spec->config_env);
    for (size_t i = 0; i < spec->count; i++) {
        const struct opt *o = &spec->table[i];
        if (o->doc)
            fprintf(out, "  --%-17s%s\n", o->key, o->doc);
    }
    if (spec->usage_tail)
        fputs(spec->usage_tail, out);
}

struct load_ctx {
    const struct opts_spec *spec;
    void *base;
};

static void load_line(void *ctx, const char *path, int line,
                      const char *key, const char *val)
{
    struct load_ctx *lc = ctx;
    char origin[PATH_MAX + 16];
    snprintf(origin, sizeof(origin), "%s:%d", path, line);
    if (!val) {
        fprintf(stderr, "[%s] %s: expected 'key = value'\n", lc->spec->name(), origin);
        return;
    }
    const struct opt *o = opt_find(lc->spec, key, strlen(key));
    if (!o) {
        fprintf(stderr, "[%s] %s: unknown key '%s'\n", lc->spec->name(), origin, key);
        return;
    }
    opt_apply(lc->spec, lc->base, o, val, origin);
}

static void write_default_file(const struct opts_spec *spec, const void *base,
                               const char *path)
{
    FILE *f = fs_create(path);
    if (!f)
        return;
    fchmod(fileno(f), 0600);
    opts_write_default(spec, base, f);
    fclose(f);
}

static void load_file(const struct opts_spec *spec, void *base, const char *path)
{
    struct load_ctx lc = { .spec = spec, .base = base };
    if (config_parse_file(path, load_line, &lc))
        return;
    if (errno == ENOENT)
        write_default_file(spec, base, path);
    else
        fprintf(stderr, "[%s] cannot read %s: %s\n",
                spec->name(), path, strerror(errno));
}

static void load_env(const struct opts_spec *spec, void *base)
{
    for (size_t i = 0; i < spec->count; i++) {
        char name[64];
        int w = snprintf(name, sizeof(name), "%s", spec->env_prefix);
        for (const char *k = spec->table[i].key; *k && (size_t)w + 1 < sizeof(name); k++)
            name[w++] = *k == '-' ? '_' : (char)toupper((unsigned char)*k);
        name[w] = '\0';
        const char *val = getenv(name);
        if (val && val[0]) {
            char origin[80];
            snprintf(origin, sizeof(origin), "$%s", name);
            opt_apply(spec, base, &spec->table[i], val, origin);
        }
    }
}

struct arg {
    const char *key;
    size_t klen;
    const char *val;
};

enum args_result { ARGS_OK, ARGS_HELP, ARGS_BAD };

/* Splits argv into "--key=value" and "--key value" pairs. args holds
 * argc entries. */
static enum args_result split_args(const struct opts_spec *spec, int argc,
                                   char **argv, struct arg *args, int *n)
{
    *n = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))
            return ARGS_HELP;
        if (strncmp(a, "--", 2) || !a[2]) {
            fprintf(stderr, "[%s] unexpected argument '%s'\n", spec->name(), a);
            return ARGS_BAD;
        }
        struct arg *out = &args[(*n)++];
        out->key = a + 2;
        const char *eq = strchr(out->key, '=');
        if (eq) {
            out->klen = (size_t)(eq - out->key);
            out->val = eq + 1;
        } else if (i + 1 < argc) {
            out->klen = strlen(out->key);
            out->val = argv[++i];
        } else {
            fprintf(stderr, "[%s] '%s' needs a value\n", spec->name(), a);
            return ARGS_BAD;
        }
    }
    return ARGS_OK;
}

static bool is_config_arg(const struct arg *a)
{
    const char *name = "config";
    return a->klen == strlen(name) && !strncmp(a->key, name, a->klen);
}

/* The config path from --config, then the environment, then the default
 * file. NONE, in any case, means no file. */
static const char *find_config(const struct opts_spec *spec, char *buf, size_t n,
                               const struct arg *args, int nargs)
{
    const char *path = NULL;
    for (int i = 0; i < nargs; i++)
        if (is_config_arg(&args[i]))
            path = args[i].val;
    if (!path) {
        const char *env = getenv(spec->config_env);
        if (env && env[0])
            path = env;
    }
    if (!path && spec->config_file && xdg_config_path(buf, n, spec->config_file))
        path = buf;
    return path && strcasecmp(path, "NONE") ? path : NULL;
}

int opts_load(const struct opts_spec *spec, void *base, int argc, char **argv,
              char **config_path)
{
    *config_path = NULL;
    memset(base, 0, spec->size);
    spec->defaults(base);
    struct arg *args = calloc((size_t)(argc > 0 ? argc : 1), sizeof(*args));
    if (!args)
        return 2;
    int nargs;
    enum args_result r = split_args(spec, argc, argv, args, &nargs);
    if (r != ARGS_OK) {
        usage(spec, r == ARGS_HELP ? stdout : stderr);
        free(args);
        return r == ARGS_HELP ? 1 : 2;
    }

    char defpath[PATH_MAX];
    const char *path = find_config(spec, defpath, sizeof(defpath), args, nargs);
    if (path) {
        *config_path = strdup(path);
        if (!*config_path) {
            free(args);
            opts_destroy(spec, base);
            return 2;
        }
        load_file(spec, base, path);
    }
    load_env(spec, base);

    int result = 0;
    for (int i = 0; i < nargs && !result; i++) {
        if (is_config_arg(&args[i]))
            continue;
        const struct opt *o = opt_find(spec, args[i].key, args[i].klen);
        if (!o) {
            fprintf(stderr, "[%s] unknown option '--%.*s'\n", spec->name(),
                    (int)args[i].klen, args[i].key);
            usage(spec, stderr);
            result = 2;
            continue;
        }
        char origin[80];
        snprintf(origin, sizeof(origin), "--%s", o->key);
        if (!opt_apply(spec, base, o, args[i].val, origin))
            result = 2;
    }
    free(args);
    if (result) {
        opts_destroy(spec, base);
        free(*config_path);
        *config_path = NULL;
    }
    return result;
}

void opts_destroy(const struct opts_spec *spec, void *base)
{
    for (size_t i = 0; i < spec->count; i++)
        if (spec->table[i].type->release)
            spec->table[i].type->release(field_of(&spec->table[i], base));
}

bool opts_copy(const struct opts_spec *spec, void *dst, const void *src)
{
    memcpy(dst, src, spec->size);
    for (size_t i = 0; i < spec->count; i++)
        if (spec->table[i].type == &OPT_TYPE_TEXT)
            *(const char **)field_of(&spec->table[i], dst) = NULL;
    for (size_t i = 0; i < spec->count; i++) {
        const struct opt *o = &spec->table[i];
        if (o->type != &OPT_TYPE_TEXT)
            continue;
        const char *value = *(const char *const *)field_of_const(o, src);
        /* Skip empty values and fields that an alias already copied. */
        if (!value || *(const char *const *)field_of_const(o, dst))
            continue;
        if (!parse_text(o, value, field_of(o, dst))) {
            opts_destroy(spec, dst);
            return false;
        }
    }
    return true;
}

bool opts_set(const struct opts_spec *spec, void *base, const char *key,
              const char *val)
{
    const struct opt *o = opt_find(spec, key, strlen(key));
    if (!o) {
        fprintf(stderr, "[%s] unknown key '%s'\n", spec->name(), key);
        return false;
    }
    return opt_apply(spec, base, o, val, key);
}

bool opts_validate(const struct opts_spec *spec, const char *key, const char *val)
{
    const struct opt *o = opt_find(spec, key, strlen(key));
    if (!o)
        return false;
    if (!val[0])
        return true;
    void *scratch = calloc(1, spec->size);
    if (!scratch)
        return false;
    bool valid = o->type->parse(o, val, field_of(o, scratch));
    opts_destroy(spec, scratch);
    free(scratch);
    return valid;
}

bool opts_format(const struct opts_spec *spec, const void *base, const char *key,
                 char *out, size_t size)
{
    const struct opt *o = opt_find(spec, key, strlen(key));
    if (!o || !size)
        return false;
    const void *field = field_of_const(o, base);
    if (o->type == &OPT_TYPE_TEXT) {
        const char *value = *(const char *const *)field;
        return (size_t)snprintf(out, size, "%s", value ? value : "") < size;
    }
    char buf[64];
    o->type->format(o, field, buf, sizeof(buf));
    return (size_t)snprintf(out, size, "%s", buf) < size;
}

bool opts_save(const struct opts_spec *spec, const void *base, const char *path,
               const char *const *keys, size_t count)
{
    if (!count)
        return true;
    if (!path) {
        errno = EINVAL;
        return false;
    }
    char resolved[PATH_MAX], temp[PATH_MAX + 16];
    char (*values)[1024] = calloc(count, sizeof(*values));
    bool *written = calloc(count, sizeof(*written));
    FILE *in = NULL, *out = NULL;
    char *line = NULL;
    size_t cap = 0;
    bool ok = false;
    int saved_errno = EINVAL;
    if (!values || !written) {
        saved_errno = ENOMEM;
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        if (!opts_format(spec, base, keys[i], values[i], sizeof(values[i])) ||
            strpbrk(values[i], "\r\n") ||
            strlen(keys[i]) + strlen(values[i]) + 4 >= 1024)
            goto done;
        size_t len = strlen(values[i]);
        if (len && (isspace((unsigned char)values[i][0]) ||
                    isspace((unsigned char)values[i][len - 1])))
            goto done;
    }
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "%s", path) >= (int)sizeof(target))
        goto done;
    if (realpath(target, resolved))
        strcpy(target, resolved);
    else if (errno != ENOENT)
        goto io_error;
    if (!mkdir_parents(target))
        goto io_error;
    in = fopen(target, "r");
    if (!in && errno != ENOENT)
        goto io_error;
    if (in) {
        struct stat st;
        if (fstat(fileno(in), &st) < 0)
            goto io_error;
        if (!S_ISREG(st.st_mode))
            goto done;
    }
    out = temp_open(target, temp, sizeof(temp));
    if (!out)
        goto io_error;
    bool newline = true;
    ssize_t len;
    while (in && (len = getline(&line, &cap, in)) >= 0) {
        const char *start = line;
        while (isspace((unsigned char)*start)) start++;
        const char *eq = strchr(start, '=');
        const char *end = eq;
        if (end)
            while (end > start && isspace((unsigned char)end[-1])) end--;
        const struct opt *existing = *start != '#' && end
            ? opt_find(spec, start, (size_t)(end - start)) : NULL;
        size_t i;
        for (i = 0; i < count; i++)
            if (existing) {
                const struct opt *selected = opt_find(spec, keys[i], strlen(keys[i]));
                if (existing->off == selected->off && existing->type == selected->type)
                    break;
            }
        if (i < count) {
            if (fprintf(out, "%s = %s\n", keys[i], values[i]) < 0)
                goto io_error;
            written[i] = true;
            newline = true;
        } else {
            if (fwrite(line, 1, (size_t)len, out) != (size_t)len)
                goto io_error;
            newline = len > 0 && line[len - 1] == '\n';
        }
    }
    if (in && ferror(in))
        goto io_error;
    for (size_t i = 0; i < count; i++) {
        if (written[i]) continue;
        if (!newline && fputc('\n', out) == EOF)
            goto io_error;
        if (fprintf(out, "%s = %s\n", keys[i], values[i]) < 0)
            goto io_error;
        newline = true;
    }
    FILE *staged = out;
    out = NULL;
    if (!temp_commit(staged, temp, target))
        goto io_error;
    ok = true;
    goto done;
io_error:
    saved_errno = errno ? errno : EIO;
done:
    if (out) {
        fclose(out);
        unlink(temp);
    }
    if (in) fclose(in);
    free(line);
    free(values);
    free(written);
    if (!ok) errno = saved_errno;
    return ok;
}

void opts_write_default(const struct opts_spec *spec, const void *base, FILE *out)
{
    fprintf(out,
        "# %s configuration: flat 'key = value' lines.\n"
        "# Everything below is commented out; shown values are the defaults.\n"
        "# An empty value ('key =') resets a key to its default.\n",
        spec->name());
    if (spec->file_note)
        fputs(spec->file_note, out);
    for (size_t i = 0; i < spec->count; i++) {
        const struct opt *o = &spec->table[i];
        if (o->unlisted)
            continue;
        char val[256];
        o->type->format(o, field_of_const(o, base), val, sizeof(val));
        if (o->doc)
            fprintf(out, "\n# %s\n", o->doc);
        fprintf(out, "#%s = %s\n", o->key, val);
    }
}

size_t opts_option_count(const struct opts_spec *spec)
{
    size_t n = 0;
    for (size_t i = 0; i < spec->count; i++)
        n += !spec->table[i].unlisted;
    return n;
}

bool opts_option_at(const struct opts_spec *spec, size_t index,
                    struct opts_option *out)
{
    for (size_t i = 0; i < spec->count; i++) {
        const struct opt *o = &spec->table[i];
        if (o->unlisted)
            continue;
        if (index--)
            continue;
        *out = (struct opts_option){
            .key = o->key, .doc = o->doc, .kind = o->type->kind,
            .min = o->min, .max = o->max,
        };
        return true;
    }
    return false;
}

const char *opts_choice_at(const struct opts_spec *spec, const char *key, size_t index)
{
    const struct opt *o = opt_find(spec, key, strlen(key));
    if (!o || !o->type->choice)
        return NULL;
    return o->type->choice(o, index);
}
