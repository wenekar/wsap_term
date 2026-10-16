
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/config/config.h"
#include "font/font.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "util/options.h"

static char tmpdir[4096];

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f);
    int written = fputs(content, f);
    int closed = fclose(f);
    assert(written >= 0);
    assert(closed == 0);
}

static char *path_join(char *out, const char *a, const char *b)
{
    snprintf(out, 4096, "%s/%s", a, b);
    return out;
}

static void clear_env(void)
{
    unsetenv("WSAP_CONFIG");
    unsetenv("WSAP_CURSOR_STYLE");
    unsetenv("WSAP_CURSOR_BLINK");
    unsetenv("WSAP_FONT_SIZE");
    unsetenv("WSAP_FULLSCREEN");
    unsetenv("WSAP_WIDTH");
    unsetenv("XDG_CONFIG_HOME");
}

/* Loads the config and drops the path, which these tests do not check. */
static int load(struct app_config *c, int argc, char **argv)
{
    char *path = NULL;
    int result = opts_load(&app_config_spec,c, argc, argv, &path);
    free(path);
    return result;
}

static void defaults(struct app_config *c)
{
    memset(c, 0, sizeof(*c));
    app_config_defaults(c);
}

static void write_default(FILE *out)
{
    struct app_config def;
    defaults(&def);
    opts_write_default(&app_config_spec, &def, out);
    opts_destroy(&app_config_spec,&def);
}

static int parse_args(struct app_config *c, const char *path, int extra,
                      char **extra_argv)
{
    char *argv[16] = { (char *)"wsap_term", (char *)"--config", (char *)path };
    for (int i = 0; i < extra; i++)
        argv[3 + i] = extra_argv[i];
    return load(c, 3 + extra, argv);
}

static int parse(struct app_config *c, const char *path)
{
    return parse_args(c, path, 0, NULL);
}

static void test_xdg_path_and_creation(void)
{
    char xdg[4096], conf[8192], dir[4096];
    path_join(xdg, tmpdir, "xdg");
    setenv("XDG_CONFIG_HOME", xdg, 1);

    struct app_config c;
    char *argv[] = { (char *)"wsap_term", NULL };
    assert(load(&c, 1, argv) == 0);

    path_join(dir, xdg, "wsap_term");
    path_join(conf, dir, "term.conf");
    struct stat st;
    assert(stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    assert(stat(conf, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(conf, "r");
    char buf[8192] = {0};
    assert(f);
    size_t nread = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    assert(nread > 0);
    assert(strstr(buf, "#font-size = 10"));
    assert(strstr(buf, "#cursor-style = block"));
    assert(strstr(buf, "#color15 ="));
    assert(strstr(buf, "#command ="));

    assert(c.terminal.cursor_blink && !c.window.tearing);
    assert(!c.window.size.x && !c.window.size.y && !c.terminal.command);
    assert(c.terminal.cursor_style == WSAP_VTCORE_CURSOR_BLOCK);

    write_file(conf, "width = 640\n");
    opts_destroy(&app_config_spec,&c);
    assert(load(&c, 1, argv) == 0);
    assert(c.window.size.x == 640);
    f = fopen(conf, "r");
    assert(f);
    char *line = fgets(buf, sizeof(buf), f);
    fclose(f);
    assert(line);
    assert(!strcmp(buf, "width = 640\n"));
    unsetenv("XDG_CONFIG_HOME");
    opts_destroy(&app_config_spec,&c);
}

static void test_home_fallback(void)
{
    char home[4096], conf[8192];
    path_join(home, tmpdir, "home");
    setenv("HOME", home, 1);
    unsetenv("XDG_CONFIG_HOME");

    struct app_config c;
    char *argv[] = { (char *)"wsap_term", NULL };
    assert(load(&c, 1, argv) == 0);
    snprintf(conf, sizeof(conf), "%s/.config/wsap_term/term.conf", home);
    struct stat st;
    assert(stat(conf, &st) == 0);

    char rel[] = "relative/xdg";
    assert(chdir(tmpdir) == 0);
    setenv("XDG_CONFIG_HOME", rel, 1);
    char home2[4096], conf2[8192];
    path_join(home2, tmpdir, "home2");
    setenv("HOME", home2, 1);
    opts_destroy(&app_config_spec,&c);
    assert(load(&c, 1, argv) == 0);
    snprintf(conf2, sizeof(conf2), "%s/.config/wsap_term/term.conf", home2);
    assert(stat(conf2, &st) == 0);
    assert(stat("relative/xdg/wsap_term/term.conf", &st) != 0);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
    opts_destroy(&app_config_spec,&c);
}

static void test_key_parsing(void)
{
    char conf[4096];
    path_join(conf, tmpdir, "keys.conf");
    write_file(conf,
        "# a comment\n"
        "  \n"
        "foreground = #102030\n"
        "background = #a0b0c0\n"
        "color0 = #010203\n"
        "color15 = #f0e0d0\n"
        "font = Test Sans\n"
        "font-size = 14\n"
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
        "hinting = none\n"
        "antialias = grayscale\n"
#else
        "hinting = full\n"
        "antialias = rgb\n"
#endif
        "cursor-style = underline\n"
        "cursor-blink = false\n"
        "vsync = off\n"
        "width = 800\n"
        "height = 600\n"
        "command = /bin/echo hello world\n"
        "theme = /tmp/theme.css\n"
        "menu-hover-delay = 500\n"
        "fullscreen = true\n");
    struct app_config c;
    assert(parse(&c, conf) == 0);
    assert(c.palette.fg == 0xFF102030u);
    assert(c.palette.bg == 0xFFA0B0C0u);
    assert(c.palette.ansi[0] == 0xFF010203u);
    assert(c.palette.ansi[15] == 0xFFF0E0D0u);
    assert(c.window.font_family && !strcmp(c.window.font_family, "Test Sans"));
    assert(c.window.font_size == 14);
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    assert(c.hinting == FONT_HINT_NONE);
    assert(c.antialias == FONT_AA_GRAYSCALE);
#else
    assert(c.hinting == FONT_HINT_FULL);
    assert(c.antialias == FONT_AA_RGB);
#endif
    assert(c.terminal.cursor_style == WSAP_VTCORE_CURSOR_UNDERLINE);
    assert(!c.terminal.cursor_blink);
    assert(c.window.tearing);
    assert(c.window.size.x == 800 && c.window.size.y == 600);
    assert(c.terminal.command && !strcmp(c.terminal.command, "/bin/echo hello world"));
    assert(c.theme && !strcmp(c.theme, "/tmp/theme.css"));
    assert(c.window.menu_hover_delay_ms == 500);
    assert(c.window.fullscreen);

    write_file(conf,
        "foreground = #102030\n"
        "color3 = #445566\n"
        "cursor-blink = false\n"
        "vsync = false\n"
        "width = 800\n"
        "font = Test Sans\n"
        "foreground =\n"
        "color3 =\n"
        "cursor-blink =\n"
        "vsync =\n"
        "width =\n"
        "font =\n");
    opts_destroy(&app_config_spec,&c);
    assert(parse(&c, conf) == 0);
    struct wsap_vtcore_theme def;
    wsap_vtcore_theme_default(&def);
    assert(c.palette.fg == def.fg);
    assert(c.palette.ansi[3] == def.ansi[3]);
    assert(c.terminal.cursor_blink && !c.window.tearing && !c.window.size.x && !c.window.font_family);
    opts_destroy(&app_config_spec,&c);
}

static void test_precedence(void)
{
    char conf[4096];
    path_join(conf, tmpdir, "env.conf");
    write_file(conf, "cursor-blink = true\ncursor-style = block\n"
                     "font-size = 11\nwidth = 300\n");

    setenv("WSAP_CURSOR_BLINK", "0", 1);
    setenv("WSAP_CURSOR_STYLE", "underline", 1);
    setenv("WSAP_FONT_SIZE", "12", 1);
    setenv("WSAP_WIDTH", "", 1);
    char *args[] = { (char *)"--font-size", (char *)"13",
                     (char *)"--cursor-style=bar" };
    struct app_config c;
    assert(parse_args(&c, conf, 3, args) == 0);
    assert(!c.terminal.cursor_blink);
    assert(c.terminal.cursor_style == WSAP_VTCORE_CURSOR_BAR);
    assert(c.window.font_size == 13);
    assert(c.window.size.x == 300);
    opts_destroy(&app_config_spec,&c);

    assert(parse(&c, conf) == 0);
    assert(c.terminal.cursor_style == WSAP_VTCORE_CURSOR_UNDERLINE && c.window.font_size == 12);
    clear_env();
    opts_destroy(&app_config_spec,&c);

    /* An empty argument resets the key, as an empty value in the file
     * does. */
    char *reset[] = { (char *)"--font-size=", (char *)"--width", (char *)"" };
    assert(parse_args(&c, conf, 3, reset) == 0);
    assert(c.window.font_size == FONT_DEFAULT_SIZE_PT && !c.window.size.x);
    opts_destroy(&app_config_spec,&c);
}

static void test_config_selection(void)
{
    char conf[4096], conf2[4096];
    path_join(conf, tmpdir, "sel.conf");
    path_join(conf2, tmpdir, "sel2.conf");
    write_file(conf, "width = 111\n");
    write_file(conf2, "width = 222\n");

    setenv("WSAP_CONFIG", conf, 1);
    struct app_config c;
    char *argv[] = { (char *)"wsap_term", NULL };
    assert(load(&c, 1, argv) == 0);
    assert(c.window.size.x == 111);

    opts_destroy(&app_config_spec,&c);
    assert(parse(&c, conf2) == 0);
    assert(c.window.size.x == 222);

    unsetenv("WSAP_CONFIG");
    char xdg[4096], created[8192];
    path_join(xdg, tmpdir, "xdg-none");
    setenv("XDG_CONFIG_HOME", xdg, 1);
    snprintf(created, sizeof(created), "%s/wsap_term/term.conf", xdg);
    char *none_argv[] = { (char *)"wsap_term", (char *)"--config=NONE", NULL };
    opts_destroy(&app_config_spec,&c);
    assert(load(&c, 2, none_argv) == 0);
    struct stat st;
    assert(stat(created, &st) != 0);
    const char *width = "width";
    assert(opts_set(&app_config_spec,&c, width, "333"));
    /* No config path, so there is nothing to save to. */
    assert(!opts_save(&app_config_spec, &c, NULL, &width, 1));
    assert(stat(created, &st) != 0);

    setenv("WSAP_CONFIG", "NONE", 1);
    opts_destroy(&app_config_spec,&c);
    assert(load(&c, 1, argv) == 0);
    assert(stat(created, &st) != 0);
    clear_env();
    opts_destroy(&app_config_spec,&c);
}

static void test_errors(void)
{
    char conf[4096];
    path_join(conf, tmpdir, "bad.conf");
    write_file(conf,
        "bogus-key = 1\n"
        "no equals sign\n"
        "= orphan value\n"
        "width = notanumber\n"
        "cursor-style = spiral\n"
        "height = 480\n");
    struct app_config c;
    assert(parse(&c, conf) == 0);
    assert(c.window.size.y == 480);
    assert(!c.window.size.x && c.terminal.cursor_style == WSAP_VTCORE_CURSOR_BLOCK);
    opts_destroy(&app_config_spec,&c);

    /* An overlong line is skipped whole, even the last one without a
     * newline; its first bytes do not pass for a setting. */
    char overlong[2100] = "height = 480\nfont = ";
    size_t head = strlen(overlong);
    memset(overlong + head, 'x', sizeof(overlong) - head - 1);
    overlong[sizeof(overlong) - 1] = 0;
    write_file(conf, overlong);
    assert(parse(&c, conf) == 0);
    assert(c.window.size.y == 480);
    assert(!c.window.font_family || !strchr(c.window.font_family, 'x'));
    opts_destroy(&app_config_spec,&c);

    /* A line that just fills the read buffer still counts. */
    char full[1100] = "font = ";
    head = strlen(full);
    memset(full + head, 'y', 1023 - head);
    strcpy(full + 1023, "\nheight = 300\n");
    write_file(conf, full);
    assert(parse(&c, conf) == 0);
    assert(c.window.font_family && strlen(c.window.font_family) == 1023 - head);
    assert(c.window.size.y == 300);
    opts_destroy(&app_config_spec,&c);

    char *unknown[] = { (char *)"--bogus", (char *)"1" };
    assert(parse_args(&c, conf, 2, unknown) == 2);
    opts_destroy(&app_config_spec,&c);
    char *bad[] = { (char *)"--width=wide" };
    assert(parse_args(&c, conf, 1, bad) == 2);
    opts_destroy(&app_config_spec,&c);
    char *dangling[] = { (char *)"--theme" };
    assert(parse_args(&c, conf, 1, dangling) == 2);
    opts_destroy(&app_config_spec,&c);
}

/* Uncommenting the generated file must reproduce the built-in defaults. */
static void test_generated_file_matches_defaults(void)
{
    char gen[4096], conf[4096];
    path_join(gen, tmpdir, "gen.conf");
    path_join(conf, tmpdir, "uncommented.conf");
    FILE *f = fopen(gen, "w");
    assert(f);
    write_default(f);
    fclose(f);

    f = fopen(gen, "r");
    FILE *out = fopen(conf, "w");
    assert(f && out);
    char line[512];
    int keys = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' && line[1] != ' ' && strstr(line, " = ")) {
            fputs(line + 1, out);
            keys++;
        }
    }
    fclose(f);
    fclose(out);
    assert(keys >= 26);

    struct app_config def, got;
    assert(parse(&def, "NONE") == 0);
    assert(parse(&got, conf) == 0);
    assert(!memcmp(&def.palette, &got.palette, sizeof(def.palette)));
    assert(def.window.size.x == got.window.size.x && def.window.size.y == got.window.size.y);
    assert(def.window.font_size == got.window.font_size);
    assert(def.window.menu_hover_delay_ms == got.window.menu_hover_delay_ms);
    assert(def.hinting == got.hinting && def.antialias == got.antialias);
    assert(def.terminal.cursor_style == got.terminal.cursor_style);
    assert(def.terminal.cursor_blink == got.terminal.cursor_blink);
    assert(def.window.tearing == got.window.tearing);
    assert(def.window.fullscreen == got.window.fullscreen);
    opts_destroy(&app_config_spec,&def);
    opts_destroy(&app_config_spec,&got);
}

/* Turning vsync off does not also go fullscreen. */
static void test_vsync_and_fullscreen_are_separate(void)
{
    char conf[4096];
    write_file(path_join(conf, tmpdir, "vsync.conf"), "vsync = false\n");
    struct app_config c;
    assert(parse(&c, conf) == 0);
    assert(c.window.tearing && !c.window.fullscreen);
    opts_destroy(&app_config_spec,&c);

    setenv("WSAP_FULLSCREEN", "true", 1);
    assert(parse(&c, conf) == 0);
    assert(c.window.tearing && c.window.fullscreen);
    clear_env();
    opts_destroy(&app_config_spec,&c);
}

static void test_edit_save_and_copy(void)
{
    char conf[4096];
    path_join(conf, tmpdir, "edit.conf");
    const char *original = "# Keep this note.\nfont-size = 10\nfont-size = 11\n"
                           "future-option = keep\ncommand = /bin/sh";
    write_file(conf, original);
    struct app_config config;
    assert(parse(&config, conf) == 0);
    assert(opts_set(&app_config_spec,&config, "font-size", "15"));
    assert(opts_set(&app_config_spec,&config, "font", "monospace"));
    struct app_config copy = {0};
    assert(opts_copy(&app_config_spec,&copy, &config));
    opts_destroy(&app_config_spec,&config);
    const char *keys[] = { "font-size", "font" };
    assert(opts_save(&app_config_spec, &copy, conf, keys, 2));
    FILE *file = fopen(conf, "r");
    assert(file);
    char text[512] = {0};
    assert(fread(text, 1, sizeof(text) - 1, file) > 0);
    fclose(file);
    assert(!strcmp(text, "# Keep this note.\nfont-size = 15\nfont-size = 15\n"
                         "future-option = keep\ncommand = /bin/sh\nfont = monospace\n"));
    struct app_config loaded;
    assert(parse(&loaded, conf) == 0);
    assert(loaded.window.font_size == 15 && !strcmp(loaded.window.font_family, "monospace"));
    opts_destroy(&app_config_spec,&loaded);

    assert(opts_set(&app_config_spec,&copy, "font", ""));
    assert(opts_save(&app_config_spec, &copy, conf, &keys[1], 1));
    assert(parse(&loaded, conf) == 0);
    assert(!loaded.window.font_family);
    opts_destroy(&app_config_spec,&loaded);
    assert(opts_set(&app_config_spec,&copy, "command", "bad\nfont-size = 99"));
    const char *command = "command";
    assert(!opts_save(&app_config_spec, &copy, conf, &command, 1) && errno == EINVAL);
    assert(parse(&loaded, conf) == 0);
    assert(loaded.window.font_size == 15 && !strcmp(loaded.terminal.command, "/bin/sh"));
    opts_destroy(&app_config_spec,&loaded);
    opts_destroy(&app_config_spec,&copy);

    write_file(conf, "css-path = breeze-dark\n");
    assert(parse(&config, conf) == 0);
    assert(opts_set(&app_config_spec,&config, "theme", "breeze-light"));
    const char *theme = "theme";
    assert(opts_save(&app_config_spec, &config, conf, &theme, 1));
    assert(parse(&loaded, conf) == 0);
    assert(!strcmp(loaded.theme, "breeze-light"));
    opts_destroy(&app_config_spec,&loaded);
    opts_destroy(&app_config_spec,&config);
}

static void test_option_values(void)
{
    struct app_config config;
    defaults(&config);
    char value[256];
    assert(opts_format(&app_config_spec, &config, "font", value, sizeof(value)) && !value[0]);
    assert(opts_format(&app_config_spec, &config, "theme", value, sizeof(value)) && !value[0]);
    assert(opts_format(&app_config_spec, &config, "font-size", value, sizeof(value)));
    assert(!strcmp(value, "10"));
    assert(!opts_format(&app_config_spec, &config, "font-size", value, 1));
    assert(!opts_format(&app_config_spec, &config, "missing", value, sizeof(value)));
    for (size_t i = 0; i < opts_option_count(&app_config_spec); i++) {
        struct opts_option option;
        assert(opts_option_at(&app_config_spec, i, &option));
        assert(opts_format(&app_config_spec, &config, option.key, value, sizeof(value)));
        if (option.kind != OPT_KIND_CHOICE) continue;
        for (size_t j = 0; opts_choice_at(&app_config_spec, option.key, j); j++)
            assert(opts_set(&app_config_spec,&config, option.key,
                                  opts_choice_at(&app_config_spec, option.key, j)));
    }
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
    assert(!strcmp(opts_choice_at(&app_config_spec, "hinting", 0), "auto"));
    assert(!strcmp(opts_choice_at(&app_config_spec, "hinting", 1), "none"));
    assert(!opts_choice_at(&app_config_spec, "hinting", 2));
    assert(!strcmp(opts_choice_at(&app_config_spec, "antialias", 0), "auto"));
    assert(!strcmp(opts_choice_at(&app_config_spec, "antialias", 1), "grayscale"));
    assert(!strcmp(opts_choice_at(&app_config_spec, "antialias", 2), "mono"));
    assert(!opts_choice_at(&app_config_spec, "antialias", 3));
    assert(!opts_set(&app_config_spec,&config, "hinting", "full"));
    assert(!opts_set(&app_config_spec,&config, "antialias", "rgb"));
#endif
    opts_destroy(&app_config_spec,&config);
}

/* Each accepted hinting or antialias string maps to one canonical name.
 * NULL means the key rejects the string. */
static void test_hinting_names(void)
{
    static const struct { const char *key, *in, *out; } cases[] = {
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
        { "hinting", "auto", "auto" }, { "hinting", "system", "auto" },
        { "hinting", "default", "auto" }, { "hinting", "AUTO", "auto" },
        { "hinting", "none", "none" }, { "hinting", "off", "none" },
        { "hinting", "NONE", "none" },
        { "hinting", "false", NULL }, { "hinting", "0", NULL },
        { "hinting", "no", NULL }, { "hinting", "none hinting", NULL },
        { "hinting", "slight", NULL }, { "hinting", "light", NULL },
        { "hinting", "slight hinting", NULL }, { "hinting", "medium", NULL },
        { "hinting", "medium hinting", NULL }, { "hinting", "full", NULL },
        { "hinting", "normal", NULL }, { "hinting", "full hinting", NULL },
        { "hinting", "grayscale", NULL }, { "hinting", "mono", NULL },
        { "antialias", "auto", "auto" }, { "antialias", "system", "auto" },
        { "antialias", "default", "auto" },
        { "antialias", "grayscale", "grayscale" },
        { "antialias", "GrayScale", "grayscale" },
        { "antialias", "mono", "mono" },
        { "antialias", "gray", NULL }, { "antialias", "greyscale", NULL },
        { "antialias", "grey", NULL }, { "antialias", "rgb", NULL },
        { "antialias", "subpixel", NULL }, { "antialias", "lcd", NULL },
        { "antialias", "lcd-rgb", NULL }, { "antialias", "bgr", NULL },
        { "antialias", "lcd-bgr", NULL }, { "antialias", "vrgb", NULL },
        { "antialias", "vertical-rgb", NULL }, { "antialias", "lcd-vrgb", NULL },
        { "antialias", "vbgr", NULL }, { "antialias", "vertical-bgr", NULL },
        { "antialias", "lcd-vbgr", NULL }, { "antialias", "monochrome", NULL },
        { "antialias", "aliased", NULL }, { "antialias", "1bit", NULL },
        { "antialias", "none", NULL }, { "antialias", "off", NULL },
#else
        { "hinting", "auto", "auto" }, { "hinting", "system", "auto" },
        { "hinting", "default", "auto" }, { "hinting", "AUTO", "auto" },
        { "hinting", "none", "none" }, { "hinting", "off", "none" },
        { "hinting", "false", "none" }, { "hinting", "0", "none" },
        { "hinting", "no", "none" }, { "hinting", "none hinting", "none" },
        { "hinting", "slight", "slight" }, { "hinting", "light", "slight" },
        { "hinting", "slight hinting", "slight" },
        { "hinting", "medium", "medium" },
        { "hinting", "medium hinting", "medium" },
        { "hinting", "full", "full" }, { "hinting", "normal", "full" },
        { "hinting", "full hinting", "full" }, { "hinting", "FULL", "full" },
        { "hinting", "grayscale", NULL }, { "hinting", "rgb", NULL },
        { "hinting", "mono", NULL }, { "hinting", "lcd", NULL },
        { "antialias", "auto", "auto" }, { "antialias", "system", "auto" },
        { "antialias", "default", "auto" },
        { "antialias", "grayscale", "grayscale" },
        { "antialias", "gray", "grayscale" },
        { "antialias", "greyscale", "grayscale" },
        { "antialias", "grey", "grayscale" },
        { "antialias", "rgb", "rgb" }, { "antialias", "subpixel", "rgb" },
        { "antialias", "lcd", "rgb" }, { "antialias", "lcd-rgb", "rgb" },
        { "antialias", "LCD-RGB", "rgb" },
        { "antialias", "bgr", "bgr" }, { "antialias", "lcd-bgr", "bgr" },
        { "antialias", "vrgb", "vrgb" }, { "antialias", "vertical-rgb", "vrgb" },
        { "antialias", "lcd-vrgb", "vrgb" },
        { "antialias", "vbgr", "vbgr" }, { "antialias", "vertical-bgr", "vbgr" },
        { "antialias", "lcd-vbgr", "vbgr" },
        { "antialias", "mono", "mono" }, { "antialias", "monochrome", "mono" },
        { "antialias", "aliased", "mono" }, { "antialias", "1bit", "mono" },
        { "antialias", "none", NULL }, { "antialias", "off", NULL },
        { "antialias", "0", NULL }, { "antialias", "slight", NULL },
        { "antialias", "full", NULL }, { "antialias", "normal", NULL },
#endif
        { "hinting", "bogus", NULL }, { "antialias", "bogus", NULL },
        { "hinting", "lcd-", NULL }, { "antialias", "rgb ", NULL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct app_config s;
        defaults(&s);
        bool ok = opts_set(&app_config_spec,&s, cases[i].key, cases[i].in);
        if (!cases[i].out) {
            if (ok)
                fprintf(stderr, "%s = %s: accepted\n", cases[i].key, cases[i].in);
            assert(!ok);
            assert(!opts_validate(&app_config_spec, cases[i].key, cases[i].in));
        } else {
            char name[64];
            assert(ok && opts_validate(&app_config_spec, cases[i].key, cases[i].in));
            assert(opts_format(&app_config_spec, &s, cases[i].key, name, sizeof(name)));
            if (strcmp(name, cases[i].out))
                fprintf(stderr, "%s = %s: %s\n", cases[i].key, cases[i].in, name);
            assert(!strcmp(name, cases[i].out));
            assert(opts_set(&app_config_spec,&s, cases[i].key, name));
            char again[64];
            assert(opts_format(&app_config_spec, &s, cases[i].key, again, sizeof(again)));
            assert(!strcmp(again, name));
        }
        opts_destroy(&app_config_spec,&s);
    }

    const struct { const char *key; const char *const *names; } choices[] = {
#if defined(__APPLE__) && !defined(WSAP_USE_FREETYPE)
        { "hinting", (const char *const[]){ "auto", "none", NULL } },
        { "antialias", (const char *const[]){ "auto", "grayscale", "mono", NULL } },
#else
        { "hinting", (const char *const[]){
            "auto", "none", "slight", "medium", "full", NULL } },
        { "antialias", (const char *const[]){
            "auto", "grayscale", "rgb", "bgr", "vrgb", "vbgr", "mono", NULL } },
#endif
    };
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); i++) {
        size_t j = 0;
        for (; choices[i].names[j]; j++) {
            const char *got = opts_choice_at(&app_config_spec, choices[i].key, j);
            assert(got && !strcmp(got, choices[i].names[j]));
        }
        assert(!opts_choice_at(&app_config_spec, choices[i].key, j));
    }
}

int main(void)
{
    assert(getcwd(tmpdir, sizeof(tmpdir)) != NULL);
    snprintf(tmpdir + strlen(tmpdir), sizeof(tmpdir) - strlen(tmpdir),
             "/cfg-test-%d", (int)getpid());
    assert(mkdir(tmpdir, 0700) == 0);
    clear_env();

    test_xdg_path_and_creation();
    test_home_fallback();
    test_key_parsing();
    test_precedence();
    test_config_selection();
    test_errors();
    test_generated_file_matches_defaults();
    test_vsync_and_fullscreen_are_separate();
    test_edit_save_and_copy();
    test_option_values();
    test_hinting_names();

    char cmd[4400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    int rc = system(cmd);
    assert(rc == 0);
    printf("PASS: config checks\n");
    return 0;
}
