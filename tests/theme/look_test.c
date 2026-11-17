#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "theme/look.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "ui/css.h"

static char user_themes[4096];

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    assert(f);
    int written = fputs(text, f);
    int closed = fclose(f);
    assert(written >= 0 && closed == 0);
}

static struct look *open_look(const char *theme)
{
    struct wsap_vtcore_theme base;
    wsap_vtcore_theme_default(&base);
    struct look *look = look_create(&base, theme);
    assert(look);
    return look;
}

static uint32_t look_fg(const struct look *a)
{
    return look_palette(a)->fg;
}

static uint32_t look_bg(const struct look *a)
{
    return look_palette(a)->bg;
}

static int theme_index(struct look *a, const char *name)
{
    for (int i = 0; i < look_theme_count(a); i++)
        if (!strcmp(look_theme_at(a, i)->name, name))
            return i;
    return -1;
}

static const struct look_theme *theme_named(struct look *a, const char *name)
{
    return look_theme_at(a, theme_index(a, name));
}

static uint32_t tab_bar_bg(struct look *a)
{
    struct css_style style = {0};
    css_style_resolve(look_sheet(a), "tab-bar", NULL, NULL,
                      CSS_PSEUDO_NONE, &style);
    assert(style.has_bg_color);
    return style.bg_color;
}

static void test_theme_discover_and_labels(void)
{
    struct look *a = open_look(NULL);
    assert(look_theme_count(a) >= 12);

    const struct look_theme *t = theme_named(a, "breeze-twilight");
    assert(t && !strcmp(t->label, "Breeze Twilight"));
    assert(strstr(t->css_path, "breeze-twilight.css"));
    assert(strstr(t->conf_path, "breeze-twilight-term.conf"));
    assert(!strcmp(theme_named(a, "macos26-dark")->label, "macOS 26 Dark"));
    assert(!strcmp(theme_named(a, "pop-os-light")->label, "Pop!_OS Light"));

    t = theme_named(a, "my-custom-theme");
    assert(t && !strcmp(t->label, "My Custom Theme") && !t->conf_path);

    t = theme_named(a, "breeze-dark");
    assert(t && strstr(t->css_path, user_themes) == t->css_path);
    assert(t->conf_path && strstr(t->conf_path, user_themes) != t->conf_path);

    for (int i = 1; i < look_theme_count(a); i++)
        assert(strcasecmp(look_theme_at(a, i - 1)->label,
                          look_theme_at(a, i)->label) <= 0);
    look_destroy(a);
}

static struct look *open_theme_file(const char *name)
{
    char *css_path = NULL;
    assert(asprintf(&css_path, "%s/%s.css", WSAP_DEV_THEMES_DIR, name) > 0);
    struct look *themed = open_look(css_path);
    free(css_path);
    return themed;
}

static struct css_style resolve_class(const struct css_sheet *sheet,
                                      const char *class_name,
                                      unsigned pseudo_flags)
{
    struct css_style style = {0};
    css_style_resolve(sheet, class_name, NULL, NULL, pseudo_flags, &style);
    return style;
}

static void test_theme_apply(void)
{
    struct look *a = open_look(NULL);

    assert(look_preview(a, theme_index(a, "breeze-twilight")));
    look_commit(a);
    assert(!strcmp(look_committed_name(a), "breeze-twilight"));
    assert(look_bg(a) == 0xFF232627u);
    assert(look_fg(a) == 0xFFFCFCFCu);
    assert(tab_bar_bg(a) == 0xFFEFF0F1u);
    look_destroy(a);

    static const char *const css_themes[] = {
        "adwaita-dark", "adwaita-light", "breeze-dark", "breeze-light",
        "breeze-twilight", "gtk3-dark", "gtk3-light", "macos26-dark",
        "macos26-light", "pop-os-dark", "pop-os-light",
    };
    for (size_t i = 0; i < sizeof(css_themes) / sizeof(css_themes[0]); i++) {
        struct look *themed = open_theme_file(css_themes[i]);
        const struct css_sheet *sheet = look_sheet(themed);

        struct css_style bar = resolve_class(sheet, "tab-bar", CSS_PSEUDO_NONE);
        assert(bar.has_bg_color && bar.has_border_color);

        struct css_style tab = resolve_class(sheet, "tab", CSS_PSEUDO_NONE);
        assert(tab.has_bg_color && tab.has_text_color &&
               tab.border_radius.val > 0.0f);

        struct css_style tab_focus = resolve_class(sheet, "tab", CSS_PSEUDO_FOCUS);
        assert(tab_focus.has_border_color);

        struct css_style tab_new = resolve_class(sheet, "tab-new", CSS_PSEUDO_NONE);
        assert(tab_new.has_text_color);

        struct css_style menu = resolve_class(sheet, "context-menu",
                                              CSS_PSEUDO_NONE);
        assert(menu.has_bg_color && menu.has_border_color && menu.has_shadow);

        struct css_style thumb = resolve_class(sheet, "scrollbar-thumb",
                                               CSS_PSEUDO_NONE);
        assert(thumb.has_bg_color && thumb.border_radius.val > 0.0f);
        assert(resolve_class(sheet, "scrollbar-thumb", CSS_PSEUDO_HOVER)
                   .has_bg_color);
        assert(resolve_class(sheet, "scrollbar-thumb", CSS_PSEUDO_ACTIVE)
                   .has_bg_color);

        struct css_style input = resolve_class(sheet, "text-input",
                                               CSS_PSEUDO_NONE);
        assert(input.has_bg_color && input.has_border_color);
        assert(resolve_class(sheet, "text-input-text", CSS_PSEUDO_NONE)
                   .has_text_color);
        assert(resolve_class(sheet, "text-input-selection", CSS_PSEUDO_NONE)
                   .has_bg_color);
        assert(resolve_class(sheet, "text-input-caret", CSS_PSEUDO_NONE)
                   .has_bg_color);

        struct css_style palette = resolve_class(sheet, "command-palette",
                                                 CSS_PSEUDO_NONE);
        assert(palette.has_bg_color && palette.has_border_color);

        struct css_style palette_input = {0};
        const struct css_element input_element = { .class_name = "text-input" };
        const struct css_element palette_parent = { .class_name = "command-palette" };
        css_style_resolve_node(sheet, &input_element, CSS_PSEUDO_NONE,
                               &palette_parent, 1, &palette_input);
        assert(palette_input.has_bg_color && palette_input.bg_color == 0u);

        assert(resolve_class(sheet, "label-link", CSS_PSEUDO_NONE)
                   .has_text_color);

        look_destroy(themed);
    }
}

static void test_theme_config_names_theme(void)
{
    struct look *a = open_look("Breeze Twilight");
    assert(!strcmp(look_committed_name(a), "breeze-twilight"));
    assert(look_bg(a) == 0xFF232627u);
    look_destroy(a);
}

static void test_theme_preview_cancel_and_rescan(void)
{
    struct look *a = open_look(NULL);
    int n = look_theme_count(a);
    assert(n > 1);
    const struct css_sheet *base = look_sheet(a);
    assert(look_previewed(a) == -1);

    assert(look_preview(a, 0));
    assert(look_previewed(a) == 0);
    assert(look_sheet(a) != base);
    assert(look_preview(a, 1));
    assert(look_preview(a, 0));
    look_cancel(a);
    assert(look_previewed(a) == -1);
    assert(look_sheet(a) == base);

    int idx = theme_index(a, "breeze-twilight");
    assert(look_preview(a, idx));
    look_commit(a);
    assert(look_previewed(a) == -1);
    assert(look_rescan(a) == n);
    assert(!strcmp(look_committed_name(a), "breeze-twilight"));
    assert(look_bg(a) == 0xFF232627u);
    look_destroy(a);
}

static void test_theme_rescan_reloads_edited_files(void)
{
    struct look *a = open_look(NULL);

    assert(look_preview(a, theme_index(a, "my-custom-theme")));
    const struct css_sheet *first = look_sheet(a);
    assert(tab_bar_bg(a) == 0xFF102030u);
    look_rescan(a);
    assert(look_preview(a, theme_index(a, "my-custom-theme")));
    assert(look_sheet(a) == first);

    write_file(user_themes, "my-custom-theme.css",
               ".tab-bar { background: #405060; }\n/* longer now */\n");
    look_rescan(a);
    assert(look_preview(a, theme_index(a, "my-custom-theme")));
    assert(tab_bar_bg(a) == 0xFF405060u);

    write_file(user_themes, "my-custom-theme.css",
               ".tab-bar { background: #708090; }\n/* longer now */\n");
    look_rescan(a);
    assert(look_preview(a, theme_index(a, "my-custom-theme")));
    assert(tab_bar_bg(a) == 0xFF708090u);
    look_destroy(a);
}

static void test_theme_keeps_config_palette(void)
{
    struct wsap_vtcore_theme base;
    wsap_vtcore_theme_default(&base);
    base.fg = 0xFF112233u;
    struct look *a = look_create(&base, "my-custom-theme");
    assert(a);
    assert(look_fg(a) == 0xFF112233u);

    assert(look_preview(a, theme_index(a, "my-custom-theme")));
    assert(look_fg(a) == 0xFF112233u);
    look_commit(a);
    assert(look_fg(a) == 0xFF112233u);

    assert(look_preview(a, theme_index(a, "breeze-twilight")));
    assert(look_fg(a) == 0xFFFCFCFCu);
    look_cancel(a);
    assert(look_fg(a) == 0xFF112233u);
    look_destroy(a);
}

static void setup_user_themes(char *root, size_t n)
{
    assert(getcwd(root, n));
    snprintf(root + strlen(root), n - strlen(root), "/theme-test-%d", (int)getpid());
    assert(mkdir(root, 0700) == 0);
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(user_themes, sizeof(user_themes), "%s/wsap_term", root);
    assert(mkdir(user_themes, 0700) == 0);
    strcat(user_themes, "/themes");
    assert(mkdir(user_themes, 0700) == 0);
    write_file(user_themes, "my-custom-theme.css", ".tab-bar { background: #102030; }\n");
    write_file(user_themes, "breeze-dark.css", ".tab-bar { background: #010101; }\n");
}

static void test_settings_replace_base_palette(void)
{
    struct look *look = open_look(NULL);
    int custom = theme_index(look, "my-custom-theme");
    assert(look_preview(look, custom));
    look_cancel(look);
    struct wsap_vtcore_theme palette;
    wsap_vtcore_theme_default(&palette);
    palette.fg = 0xFF123456u;
    assert(look_apply(look, &palette, NULL));
    assert(look_fg(look) == palette.fg);
    assert(look_preview(look, custom));
    assert(look_fg(look) == palette.fg);
    look_cancel(look);
    assert(look_apply(look, &palette, "breeze-dark"));
    assert(!strcmp(look_committed_name(look), "breeze-dark"));
    assert(!look_apply(look, &palette, "missing-theme"));
    assert(!strcmp(look_committed_name(look), "breeze-dark"));
    look_destroy(look);
}

static void test_theme_root_overrides_and_defaults(void)
{
    struct look *twilight = open_theme_file("breeze-twilight");
    const struct css_sheet *tw_sheet = look_sheet(twilight);
    struct css_style bar = resolve_class(tw_sheet, "tab-bar", CSS_PSEUDO_NONE);
    assert(bar.has_bg_color && bar.bg_color == 0xFFEFF0F1u);
    struct css_style tab = resolve_class(tw_sheet, "tab", CSS_PSEUDO_NONE);
    assert(tab.has_bg_color && tab.bg_color == 0xFFFFFFFFu);
    assert(tab.has_text_color && tab.text_color == 0xFF232629u);
    struct css_style menu =
        resolve_class(tw_sheet, "context-menu", CSS_PSEUDO_NONE);
    assert(menu.has_bg_color && menu.bg_color == 0xFF141618u);
    assert(menu.has_shadow && menu.shadow_color == 0x33000000u);
    struct css_style close_hover =
        resolve_class(tw_sheet, "tab-close", CSS_PSEUDO_HOVER);
    assert(close_hover.has_bg_color && close_hover.bg_color == 0xFFDA4453u);
    assert(close_hover.has_text_color && close_hover.text_color == 0xFFFFFFFFu);
    struct css_style item =
        resolve_class(tw_sheet, "menu-item", CSS_PSEUDO_NONE);
    assert(item.has_text_color && item.text_color == 0xFFFCFCFCu);
    look_destroy(twilight);

    struct look *mac = open_theme_file("macos26-dark");
    const struct css_sheet *mac_sheet = look_sheet(mac);
    struct css_style mac_dis =
        resolve_class(mac_sheet, "menu-item", CSS_PSEUDO_DISABLED);
    assert(mac_dis.has_text_color && mac_dis.text_color == 0xFF6E6E6Eu);
    struct css_style mac_short =
        resolve_class(mac_sheet, "menu-shortcut", CSS_PSEUDO_NONE);
    assert(mac_short.has_text_color && mac_short.text_color == 0xFF8C8C8Cu);
    look_destroy(mac);

    struct look *pop = open_theme_file("pop-os-light");
    const struct css_sheet *pop_sheet = look_sheet(pop);
    struct css_style pop_thumb =
        resolve_class(pop_sheet, "scrollbar-thumb", CSS_PSEUDO_NONE);
    assert(pop_thumb.has_bg_color && pop_thumb.bg_color == 0xFFB8B2ACu);
    struct css_style pop_short =
        resolve_class(pop_sheet, "menu-shortcut", CSS_PSEUDO_NONE);
    assert(pop_short.has_text_color && pop_short.text_color == 0xFF4D4D4Du);
    look_destroy(pop);

    const struct css_sheet *def = css_default_sheet();
    struct css_style def_bar = resolve_class(def, "tab-bar", CSS_PSEUDO_NONE);
    assert(!def_bar.has_bg_color);
    struct css_style def_tab = resolve_class(def, "tab", CSS_PSEUDO_NONE);
    assert(def_tab.has_bg_color && def_tab.bg_color == 0xFF1E1E2Eu);
}

int main(void)
{
    char root[4096];
    setup_user_themes(root, sizeof(root));

    test_theme_discover_and_labels();
    test_theme_apply();
    test_theme_config_names_theme();
    test_theme_preview_cancel_and_rescan();
    test_theme_rescan_reloads_edited_files();
    test_theme_keeps_config_palette();
    test_settings_replace_base_palette();
    test_theme_root_overrides_and_defaults();

    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    puts("PASS: theme tests");
    return 0;
}
