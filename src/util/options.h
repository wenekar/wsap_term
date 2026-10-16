#ifndef UTIL_OPTIONS_H
#define UTIL_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct opt;

enum opt_kind {
    OPT_KIND_TEXT, OPT_KIND_NUMBER, OPT_KIND_COLOR, OPT_KIND_CHOICE,
};

struct opt_type {
    bool (*parse)(const struct opt *o, const char *val, void *field);
    void (*format)(const struct opt *o, const void *field, char *buf, size_t n);
    void (*release)(void *field);
    size_t size;
    enum opt_kind kind;
    const char *(*choice)(const struct opt *o, size_t index);
};

struct enum_name {
    const char *name;
    int value;
};

struct opt {
    const char *key;
    const char *doc;
    const char *example;
    const struct opt_type *type;
    size_t off;
    int min, max;
    const struct enum_name *names;
    bool unlisted;
};

extern const struct opt_type OPT_TYPE_TEXT;
extern const struct opt_type OPT_TYPE_INT;
extern const struct opt_type OPT_TYPE_SIZE;
extern const struct opt_type OPT_TYPE_BOOL;
extern const struct opt_type OPT_TYPE_NOT_BOOL;
extern const struct opt_type OPT_TYPE_ENUM;

struct opts_spec {
    const struct opt *table;
    size_t count;
    size_t size;
    void (*defaults)(void *base);
    const char *(*name)(void);
    const char *env_prefix;
    const char *config_env;
    const char *config_file;
    const char *file_note;
    const char *usage_tail;
};

struct opts_option {
    const char *key;
    const char *doc;
    enum opt_kind kind;
    int min, max;
};

int opts_load(const struct opts_spec *spec, void *base, int argc, char **argv,
              char **config_path);
void opts_destroy(const struct opts_spec *spec, void *base);
bool opts_copy(const struct opts_spec *spec, void *dst, const void *src);

bool opts_set(const struct opts_spec *spec, void *base, const char *key,
              const char *val);
bool opts_validate(const struct opts_spec *spec, const char *key,
                   const char *val);
bool opts_format(const struct opts_spec *spec, const void *base,
                 const char *key, char *out, size_t size);

bool opts_save(const struct opts_spec *spec, const void *base,
               const char *path, const char *const *keys, size_t count);
void opts_write_default(const struct opts_spec *spec, const void *base,
                        FILE *out);

size_t opts_option_count(const struct opts_spec *spec);
bool opts_option_at(const struct opts_spec *spec, size_t index,
                    struct opts_option *out);
const char *opts_choice_at(const struct opts_spec *spec, const char *key,
                           size_t index);

#endif
