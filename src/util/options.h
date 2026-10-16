#ifndef UTIL_OPTIONS_H
#define UTIL_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A generic option table. An app describes its settings struct with one
 * table and one spec. The engine loads them from defaults, a config
 * file, environment variables, and --key arguments, in that order. */

struct opt;

enum opt_kind {
    OPT_KIND_TEXT, OPT_KIND_NUMBER, OPT_KIND_COLOR, OPT_KIND_CHOICE,
};

struct opt_type {
    bool (*parse)(const struct opt *o, const char *val, void *field);
    void (*format)(const struct opt *o, const void *field, char *buf, size_t n);
    /* Frees an owned field. NULL for plain values. */
    void (*release)(void *field);
    size_t size;
    enum opt_kind kind;
    /* The text of choice index, or NULL past the last one. Only choice
     * types set this. */
    const char *(*choice)(const struct opt *o, size_t index);
};

struct enum_name {
    const char *name;
    int value;
};

struct opt {
    const char *key;
    /* The first line is also the --help text. NULL leaves the key out of
     * --help and the default file. */
    const char *doc;
    const char *example;
    const struct opt_type *type;
    size_t off;
    int min, max;
    const struct enum_name *names;
    /* Parsed and saved, but not listed in --help or the default file. */
    bool unlisted;
};

/* The built-in types. An app builds its own types for its own values. */
extern const struct opt_type OPT_TYPE_TEXT;
extern const struct opt_type OPT_TYPE_INT;
extern const struct opt_type OPT_TYPE_SIZE;
extern const struct opt_type OPT_TYPE_BOOL;
extern const struct opt_type OPT_TYPE_NOT_BOOL;
extern const struct opt_type OPT_TYPE_ENUM;

struct opts_spec {
    const struct opt *table;
    size_t count;
    /* sizeof the settings struct. */
    size_t size;
    /* Writes every default into base. base is zeroed on entry. */
    void (*defaults)(void *base);
    /* The program name for messages. */
    const char *(*name)(void);
    /* Prefix for environment overrides, such as "WSAP_". */
    const char *env_prefix;
    /* Environment variable that names the config file, such as
     * "WSAP_CONFIG". */
    const char *config_env;
    /* Config file path under $XDG_CONFIG_HOME or $HOME/.config. */
    const char *config_file;
    /* Text added to the default config file after its first lines. May
     * be NULL. */
    const char *file_note;
    /* Text added to --help after the option list. May be NULL. */
    const char *usage_tail;
};

struct opts_option {
    const char *key;
    const char *doc;
    enum opt_kind kind;
    int min, max;
};

/* Overwrites base with defaults, then loads the config file, the
 * environment, and argv into it. The caller must not hold owned strings
 * in base. Returns 0 to run, 1 after --help, 2 on a bad argument. On
 * success *config_path holds the file path, or NULL when none was used.
 * The caller frees it. On a nonzero return, base is destroyed. */
int opts_load(const struct opts_spec *spec, void *base, int argc, char **argv,
              char **config_path);
void opts_destroy(const struct opts_spec *spec, void *base);
/* Copies owned strings. The destination must be zero or empty. */
bool opts_copy(const struct opts_spec *spec, void *dst, const void *src);

/* Sets one key as a --key argument would. False, with a message on
 * stderr, for an unknown key or a bad value. */
bool opts_set(const struct opts_spec *spec, void *base, const char *key,
              const char *val);
/* Checks a value without changing anything or writing an error. */
bool opts_validate(const struct opts_spec *spec, const char *key,
                   const char *val);
bool opts_format(const struct opts_spec *spec, const void *base,
                 const char *key, char *out, size_t size);

/* Writes the selected keys to path. Keeps other lines. Uses an atomic
 * rename. Sets errno on failure. */
bool opts_save(const struct opts_spec *spec, const void *base,
               const char *path, const char *const *keys, size_t count);
void opts_write_default(const struct opts_spec *spec, const void *base,
                        FILE *out);

/* Lists the listed options in table order. */
size_t opts_option_count(const struct opts_spec *spec);
bool opts_option_at(const struct opts_spec *spec, size_t index,
                    struct opts_option *out);
const char *opts_choice_at(const struct opts_spec *spec, const char *key,
                           size_t index);

#endif
