#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>

#include "util/options.h"

enum mode { MODE_FAST = 1, MODE_SLOW = 2 };

struct cfg {
    int size;
    bool blink;
    bool tearing;
    enum mode mode;
    char *command;
    int hidden;
};

static const struct enum_name MODE_NAMES[] = {
    { "fast", MODE_FAST },
    { "slow", MODE_SLOW },
    { "quick", MODE_FAST },
    { 0 },
};

static const struct opt TABLE[] = {
    { .key = "size", .doc = "window size", .type = &OPT_TYPE_SIZE,
      .off = offsetof(struct cfg, size), .min = 64, .max = 4096 },
    { .key = "blink", .doc = "blink the cursor", .type = &OPT_TYPE_BOOL,
      .off = offsetof(struct cfg, blink) },
    { .key = "vsync", .doc = "false enables tearing", .type = &OPT_TYPE_NOT_BOOL,
      .off = offsetof(struct cfg, tearing) },
    { .key = "mode", .doc = "fast or slow", .type = &OPT_TYPE_ENUM,
      .off = offsetof(struct cfg, mode), .names = MODE_NAMES },
    { .key = "command", .doc = "program to run", .type = &OPT_TYPE_TEXT,
      .off = offsetof(struct cfg, command), .example = "/bin/sh" },
    { .key = "hidden", .type = &OPT_TYPE_INT, .off = offsetof(struct cfg, hidden),
      .min = 0, .max = 9, .unlisted = true },
};

static void cfg_defaults(void *base)
{
    struct cfg *c = base;
    *c = (struct cfg){ .size = 800, .blink = true, .mode = MODE_SLOW };
}

static const char *test_name(void)
{
    return "options-test";
}

static const struct opts_spec SPEC = {
    .table = TABLE,
    .count = sizeof(TABLE) / sizeof(TABLE[0]),
    .size = sizeof(struct cfg),
    .defaults = cfg_defaults,
    .name = test_name,
    .env_prefix = "OPTTEST_",
    .config_env = "OPTTEST_CONFIG",
    .config_file = NULL,
    .file_note = "# Environment overrides use OPTTEST_<KEY>.\n",
    .usage_tail = "Every option also reads OPTTEST_<KEY>.\n",
};

static char dir[] = "/tmp/wsap-options-test-XXXXXX";

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "r");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n >= 0);
    rewind(f);
    char *buf = calloc(1, (size_t)n + 1);
    assert(buf);
    assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return buf;
}

static int load(struct cfg *c, char **args, int n, char **config_path)
{
    char *argv[32] = { (char *)"wsap_options" };
    assert(n < 31);
    for (int i = 0; i < n; i++)
        argv[i + 1] = args[i];
    return opts_load(&SPEC, c, n + 1, argv, config_path);
}

static void test_defaults_and_args(void)
{
    struct cfg c;
    memset(&c, 0, sizeof(c));
    char *path = NULL;
    char *none[] = { "--config", "NONE" };
    assert(load(&c, none, 2, &path) == 0);
    assert(path == NULL);
    assert(c.size == 800 && c.blink && !c.tearing && c.mode == MODE_SLOW);
    assert(c.command == NULL);
    opts_destroy(&SPEC, &c);

    memset(&c, 0, sizeof(c));
    char *args[] = {
        "--config", "NONE", "--size", "1000", "--blink", "false",
        "--vsync", "false", "--mode", "fast", "--command", "/bin/cat",
    };
    assert(load(&c, args, 12, &path) == 0);
    assert(c.size == 1000 && !c.blink && c.tearing && c.mode == MODE_FAST);
    assert(strcmp(c.command, "/bin/cat") == 0);
    opts_destroy(&SPEC, &c);
}

static void test_precedence(void)
{
    char file[256];
    snprintf(file, sizeof(file), "%s/precedence.conf", dir);
    write_text(file, "size = 100\nmode = slow\n");

    struct cfg c;
    memset(&c, 0, sizeof(c));
    char *path = NULL;
    char *from_file[] = { "--config", file };
    assert(load(&c, from_file, 2, &path) == 0);
    assert(c.size == 100);
    assert(path && strcmp(path, file) == 0);
    free(path);
    opts_destroy(&SPEC, &c);

    setenv("OPTTEST_SIZE", "200", 1);
    memset(&c, 0, sizeof(c));
    assert(load(&c, from_file, 2, &path) == 0);
    assert(c.size == 200);
    free(path);
    opts_destroy(&SPEC, &c);

    memset(&c, 0, sizeof(c));
    char *from_args[] = { "--config", file, "--size", "300" };
    assert(load(&c, from_args, 4, &path) == 0);
    assert(c.size == 300);
    free(path);
    opts_destroy(&SPEC, &c);
    unsetenv("OPTTEST_SIZE");
}

static void test_empty_value_resets(void)
{
    struct cfg c;
    memset(&c, 0, sizeof(c));
    char *path = NULL;
    char *args[] = { "--config", "NONE", "--size", "300", "--size", "",
                     "--command", "/bin/sh", "--command", "" };
    assert(load(&c, args, 10, &path) == 0);
    assert(c.size == 800);
    assert(c.command == NULL);
    opts_destroy(&SPEC, &c);
}

static void test_bad_values_keep_field(void)
{
    struct cfg c;
    memset(&c, 0, sizeof(c));
    char *path = NULL;
    char *none[] = { "--config", "NONE" };
    assert(load(&c, none, 2, &path) == 0);

    assert(!opts_set(&SPEC, &c, "size", "abc"));
    assert(!opts_set(&SPEC, &c, "size", "9999"));
    assert(!opts_set(&SPEC, &c, "nope", "1"));
    assert(c.size == 800);

    assert(opts_set(&SPEC, &c, "size", "640"));
    assert(c.size == 640);
    opts_destroy(&SPEC, &c);
}

static void test_argument_errors(void)
{
    struct cfg c;
    char *path = NULL;

    memset(&c, 0, sizeof(c));
    char *help[] = { "--help" };
    assert(load(&c, help, 1, &path) == 1);
    assert(path == NULL);

    memset(&c, 0, sizeof(c));
    char *unknown[] = { "--config", "NONE", "--command", "/bin/sh", "--bogus", "1" };
    assert(load(&c, unknown, 6, &path) == 2);
    assert(path == NULL);
    assert(c.command == NULL);

    memset(&c, 0, sizeof(c));
    char *no_value[] = { "--config", "NONE", "--size" };
    assert(load(&c, no_value, 3, &path) == 2);
}

static void test_save_keeps_other_lines(void)
{
    char file[256];
    snprintf(file, sizeof(file), "%s/save.conf", dir);
    write_text(file, "# my comment\nsize = 100\n");

    struct cfg c;
    memset(&c, 0, sizeof(c));
    c.size = 500;
    c.blink = false;
    const char *keys[] = { "size", "blink" };
    assert(opts_save(&SPEC, &c, file, keys, 2));

    char *text = read_text(file);
    assert(strstr(text, "# my comment\n"));
    assert(strstr(text, "size = 500\n"));
    assert(strstr(text, "blink = false\n"));
    assert(!strstr(text, "size = 100"));
    free(text);
    opts_destroy(&SPEC, &c);
}

static void test_default_file_is_created(void)
{
    char file[256];
    snprintf(file, sizeof(file), "%s/created.conf", dir);
    unlink(file);

    struct cfg c;
    memset(&c, 0, sizeof(c));
    char *path = NULL;
    char *args[] = { "--config", file };
    assert(load(&c, args, 2, &path) == 0);
    free(path);
    opts_destroy(&SPEC, &c);

    char *text = read_text(file);
    assert(strstr(text, "#size = 800\n"));
    assert(strstr(text, "# Environment overrides use OPTTEST_<KEY>.\n"));
    assert(!strstr(text, "#hidden"));
    free(text);
}

static void test_listing_and_choices(void)
{
    assert(opts_option_count(&SPEC) == 5);

    struct opts_option o;
    assert(opts_option_at(&SPEC, 0, &o));
    assert(strcmp(o.key, "size") == 0 && o.kind == OPT_KIND_NUMBER);
    assert(opts_option_at(&SPEC, 3, &o));
    assert(strcmp(o.key, "mode") == 0 && o.kind == OPT_KIND_CHOICE);
    assert(!opts_option_at(&SPEC, 5, &o));

    assert(strcmp(opts_choice_at(&SPEC, "mode", 0), "fast") == 0);
    assert(strcmp(opts_choice_at(&SPEC, "mode", 1), "slow") == 0);
    assert(opts_choice_at(&SPEC, "mode", 2) == NULL);
    assert(strcmp(opts_choice_at(&SPEC, "blink", 0), "true") == 0);
    assert(opts_choice_at(&SPEC, "size", 0) == NULL);
}

static void test_validate_and_copy(void)
{
    assert(!opts_validate(&SPEC, "size", "abc"));
    assert(opts_validate(&SPEC, "size", ""));
    assert(opts_validate(&SPEC, "mode", "fast"));
    assert(!opts_validate(&SPEC, "mode", "nope"));

    struct cfg src;
    memset(&src, 0, sizeof(src));
    cfg_defaults(&src);
    src.command = strdup("/bin/x");
    struct cfg dst;
    memset(&dst, 0, sizeof(dst));
    assert(opts_copy(&SPEC, &dst, &src));
    assert(dst.size == 800 && dst.command != src.command);
    assert(strcmp(dst.command, "/bin/x") == 0);
    opts_destroy(&SPEC, &src);
    opts_destroy(&SPEC, &dst);
}

int main(void)
{
    assert(mkdtemp(dir));
    test_defaults_and_args();
    test_precedence();
    test_empty_value_resets();
    test_bad_values_keep_field();
    test_argument_errors();
    test_save_keeps_other_lines();
    test_default_file_is_created();
    test_listing_and_choices();
    test_validate_and_copy();
    puts("PASS: options load, save, and listing");
    return 0;
}
