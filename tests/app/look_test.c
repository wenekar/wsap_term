#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "theme/look.h"
#include "util/app_identity.h"
#include "ui/css.h"

static char user_themes[4096];
static struct app_identity identity;

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
    struct look *look = look_create(&identity, theme);
    assert(look);
    return look;
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

/* The app layers the theme sheet over its own sheet, which holds the UI core
   rules and the app rules. */
static struct css_sheet layered_sheet(struct look *a)
{
    struct css_sheet sheet = {0};
    css_init_default(&sheet);
    assert(css_sheet_append(&sheet, look_sheet(a)));
    return sheet;
}

static uint32_t tab_bar_bg(struct look *a)
{
    struct css_sheet sheet = layered_sheet(a);
    struct css_style style = {0};
    css_style_resolve(&sheet, "tab-bar", NULL, NULL, CSS_PSEUDO_NONE, &style);
    css_sheet_clear(&sheet);
    assert(style.has_bg_color);
    return style.bg_color;
}

static void test_theme_discover_and_labels(void)
{
    struct look *a = open_look(NULL);
    assert(look_theme_count(a) >= 12);

    const struct look_theme *t = theme_named(a, "breeze-twilight");
    assert(t && !strcmp(t->label, "Breeze Twilight"));
    assert(t->polarity == THEME_POLARITY_DARK);
    assert(strstr(t->css_path, "breeze-twilight.css"));
    assert(strstr(t->data_path, "breeze-twilight.theme"));
    assert(!strcmp(theme_named(a, "macos26-dark")->label, "macOS 26 Dark"));
    assert(!strcmp(theme_named(a, "pop-os-light")->label, "Pop!_OS Light"));
    assert(theme_named(a, "pop-os-light")->polarity == THEME_POLARITY_LIGHT);

    t = theme_named(a, "my-custom-theme");
    assert(t && !strcmp(t->label, "My Custom Theme") && !t->data_path);
    assert(t->polarity == THEME_POLARITY_UNKNOWN);

    t = theme_named(a, "breeze-dark");
    assert(t && strstr(t->css_path, user_themes) == t->css_path);
    assert(t->data_path && strstr(t->data_path, user_themes) != t->data_path);

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
    assert(!strcmp(look_theme_value(a, "ansi", "background"), "#232627"));
    assert(!strcmp(look_theme_value(a, "ansi", "foreground"), "#FCFCFC"));
    assert(tab_bar_bg(a) == 0xFFEFF0F1u);
    look_destroy(a);

    static const char *const css_themes[] = {
        "adwaita-dark", "adwaita-light", "breeze-dark", "breeze-light",
        "breeze-twilight", "gtk3-dark", "gtk3-light", "macos26-dark",
        "macos26-light", "pop-os-dark", "pop-os-light",
    };
    for (size_t i = 0; i < sizeof(css_themes) / sizeof(css_themes[0]); i++) {
        struct look *themed = open_theme_file(css_themes[i]);
        const struct css_sheet *theme = look_sheet(themed);
        /* The theme sheet holds no app rule. */
        assert(!resolve_class(theme, "command-palette", CSS_PSEUDO_NONE)
                    .has_bg_color);

        struct css_sheet sheet = layered_sheet(themed);

        struct css_style bar = resolve_class(&sheet, "tab-bar", CSS_PSEUDO_NONE);
        assert(bar.has_bg_color && bar.has_border_color);

        struct css_style menu = resolve_class(&sheet, "context-menu",
                                              CSS_PSEUDO_NONE);
        assert(menu.has_bg_color && menu.has_border_color && menu.has_shadow);

        struct css_style thumb = resolve_class(&sheet, "scrollbar-thumb",
                                               CSS_PSEUDO_NONE);
        assert(thumb.has_bg_color && thumb.border_radius.val > 0.0f);
        assert(resolve_class(&sheet, "scrollbar-thumb", CSS_PSEUDO_HOVER)
                   .has_bg_color);
        assert(resolve_class(&sheet, "scrollbar-thumb", CSS_PSEUDO_ACTIVE)
                   .has_bg_color);

        struct css_style input = resolve_class(&sheet, "text-input",
                                               CSS_PSEUDO_NONE);
        assert(input.has_bg_color && input.has_border_color);
        assert(resolve_class(&sheet, "text-input-text", CSS_PSEUDO_NONE)
                   .has_text_color);
        assert(resolve_class(&sheet, "text-input-selection", CSS_PSEUDO_NONE)
                   .has_bg_color);
        assert(resolve_class(&sheet, "text-input-caret", CSS_PSEUDO_NONE)
                   .has_bg_color);

        assert(resolve_class(&sheet, "label-link", CSS_PSEUDO_NONE)
                   .has_text_color);

        css_sheet_clear(&sheet);
        look_destroy(themed);
    }
}

static void test_theme_config_names_theme(void)
{
    struct look *a = open_look("Breeze Twilight");
    assert(!strcmp(look_committed_name(a), "breeze-twilight"));
    assert(!strcmp(look_theme_value(a, "ansi", "background"), "#232627"));
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
    assert(!strcmp(look_theme_value(a, "ansi", "background"), "#232627"));
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

static void test_theme_data_blocks(void)
{
    struct look *a = open_look("my-custom-theme");
    assert(!look_theme_value(a, "ansi", "background"));

    assert(look_preview(a, theme_index(a, "breeze-twilight")));
    assert(!strcmp(look_theme_value(a, "ansi", "color4"), "#1D99F3"));
    assert(!look_theme_value(a, "syntax", "keyword"));
    look_cancel(a);
    assert(!look_theme_value(a, "ansi", "background"));
    look_destroy(a);

    /* A data block that the theme module does not know stays available to an
       application that knows it. */
    write_file(user_themes, "my-custom-theme.theme",
               "label = My Custom Theme\npolarity = light\n"
               "[syntax]\nkeyword = #112233\n");
    struct look *blocks = open_look("my-custom-theme");
    assert(!strcmp(look_theme_at(blocks, theme_index(blocks, "my-custom-theme"))->label,
                   "My Custom Theme"));
    assert(look_theme_at(blocks, theme_index(blocks, "my-custom-theme"))->polarity ==
           THEME_POLARITY_LIGHT);
    assert(!strcmp(look_theme_value(blocks, "syntax", "keyword"), "#112233"));
    look_destroy(blocks);
}

static void setup_user_themes(char *root, size_t n)
{
    assert(getcwd(root, n));
    snprintf(root + strlen(root), n - strlen(root), "/theme-test-%d", (int)getpid());
    assert(mkdir(root, 0700) == 0);
    setenv("XDG_CONFIG_HOME", root, 1);
    assert(app_identity_init(&identity, "look-test"));
    snprintf(user_themes, sizeof(user_themes), "%s/look-test", root);
    assert(mkdir(user_themes, 0700) == 0);
    strcat(user_themes, "/themes");
    assert(mkdir(user_themes, 0700) == 0);
    write_file(user_themes, "my-custom-theme.css", ".tab-bar { background: #102030; }\n");
    write_file(user_themes, "breeze-dark.css", ".tab-bar { background: #010101; }\n");
}

static void test_theme_apply_by_name(void)
{
    struct look *look = open_look(NULL);
    int custom = theme_index(look, "my-custom-theme");
    assert(look_preview(look, custom));
    look_cancel(look);
    assert(look_apply(look, NULL));
    assert(look_preview(look, custom));
    look_cancel(look);
    assert(look_apply(look, "breeze-dark"));
    assert(!strcmp(look_committed_name(look), "breeze-dark"));
    assert(!look_apply(look, "missing-theme"));
    assert(!strcmp(look_committed_name(look), "breeze-dark"));
    look_destroy(look);
}

static void test_theme_root_overrides_and_defaults(void)
{
    struct look *twilight = open_theme_file("breeze-twilight");
    struct css_sheet tw = layered_sheet(twilight);
    struct css_style bar = resolve_class(&tw, "tab-bar", CSS_PSEUDO_NONE);
    assert(bar.has_bg_color && bar.bg_color == 0xFFEFF0F1u);
    struct css_style menu = resolve_class(&tw, "context-menu", CSS_PSEUDO_NONE);
    assert(menu.has_bg_color && menu.bg_color == 0xFF141618u);
    assert(menu.has_shadow && menu.shadow_color == 0x33000000u);
    struct css_style item = resolve_class(&tw, "menu-item", CSS_PSEUDO_NONE);
    assert(item.has_text_color && item.text_color == 0xFFFCFCFCu);
    css_sheet_clear(&tw);
    look_destroy(twilight);

    struct look *mac = open_theme_file("macos26-dark");
    struct css_sheet mac_sheet = layered_sheet(mac);
    struct css_style mac_dis =
        resolve_class(&mac_sheet, "menu-item", CSS_PSEUDO_DISABLED);
    assert(mac_dis.has_text_color && mac_dis.text_color == 0xFF6E6E6Eu);
    struct css_style mac_short =
        resolve_class(&mac_sheet, "menu-shortcut", CSS_PSEUDO_NONE);
    assert(mac_short.has_text_color && mac_short.text_color == 0xFF8C8C8Cu);
    css_sheet_clear(&mac_sheet);
    look_destroy(mac);

    struct look *pop = open_theme_file("pop-os-light");
    struct css_sheet pop_sheet = layered_sheet(pop);
    struct css_style pop_thumb =
        resolve_class(&pop_sheet, "scrollbar-thumb", CSS_PSEUDO_NONE);
    assert(pop_thumb.has_bg_color && pop_thumb.bg_color == 0xFFB8B2ACu);
    struct css_style pop_short =
        resolve_class(&pop_sheet, "menu-shortcut", CSS_PSEUDO_NONE);
    assert(pop_short.has_text_color && pop_short.text_color == 0xFF4D4D4Du);
    css_sheet_clear(&pop_sheet);
    look_destroy(pop);

    /* The theme store holds no core rule. A caller layers the theme sheet
       over the core rules. */
    struct look *plain = open_look(NULL);
    const struct css_sheet *theme = look_sheet(plain);
    assert(!resolve_class(theme, "context-menu", CSS_PSEUDO_NONE).has_bg_color);
    struct css_sheet def = layered_sheet(plain);
    struct css_style def_menu = resolve_class(&def, "context-menu",
                                              CSS_PSEUDO_NONE);
    assert(def_menu.has_bg_color && def_menu.bg_color == 0xFF1E1E2Eu);
    struct css_style def_link = resolve_class(&def, "label-link",
                                              CSS_PSEUDO_NONE);
    assert(def_link.has_text_color && def_link.text_color == 0xFF89B4FAu);
    css_sheet_clear(&def);
    look_destroy(plain);
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
    test_theme_data_blocks();
    test_theme_apply_by_name();
    test_theme_root_overrides_and_defaults();

    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    puts("PASS: theme tests");
    return 0;
}
