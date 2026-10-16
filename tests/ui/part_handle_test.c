#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ui/box.h"
#include "ui/surface.h"
#include "ui/widget_impl.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/list.h"
#include "ui/widgets/scrollview.h"
#include "ui/widgets/tabbar.h"

static const char SHEET_A[] =
    ".tab { background: #111111; color: #222222; }"
    ".tab:hover { background: #333333; }"
    ".tab.active { background: #444444; }"
    ".tab:focus { border-color: #555555; }"
    ".tab-close { color: #666666; }"
    ".menu-item { background: #777777; }"
    ".menu-item:hover { background: #888888; }"
    ".menu-label { color: #999999; }"
    ".menu-separator { height: 2px; }"
    ".label-selection { background: #aaaaaa; }"
    ".label-link { color: #bbbbbb; }"
    ".text-input-text { color: #cccccc; }"
    ".text-input-selection { background: #dddddd; }"
    ".scrollbar { background: #101010; }"
    ".scrollbar-thumb { background: #202020; }"
    ".tab-bar .tab { color: #0a0b0c; }"
    ".panel .menu-item { border-color: #123456; }";

static const char SHEET_B[] =
    ".tab { background: #112211; color: #223322; }"
    ".tab:hover { background: #334433; }"
    ".tab.active { background: #445544; }"
    ".tab:focus { border-color: #556655; }"
    ".tab-close { color: #667766; }"
    ".menu-item { background: #778877; }"
    ".menu-item:hover { background: #889988; }"
    ".menu-label { color: #9a9a9a; }"
    ".menu-separator { height: 3px; }"
    ".label-selection { background: #abbbcc; }"
    ".label-link { color: #bcbcbc; }"
    ".text-input-text { color: #cdcdcd; }"
    ".text-input-selection { background: #dedede; }"
    ".scrollbar { background: #202020; }"
    ".scrollbar-thumb { background: #303030; }"
    ".tab-bar .tab { color: #0d0e0f; }"
    ".panel .menu-item { border-color: #654321; }";

static const unsigned PSEUDOS[] = {
    CSS_PSEUDO_NONE,
    CSS_PSEUDO_HOVER,
    CSS_PSEUDO_FOCUS,
    CSS_PSEUDO_HOVER | CSS_PSEUDO_FOCUS,
    CSS_PSEUDO_ACTIVE,
    CSS_PSEUDO_DISABLED,
};

struct probe {
    const struct ui_widget *widget;
    const char *const *names;
    size_t name_count;
};

static void expect_parts_match_fresh(const struct probe *probe)
{
    for (size_t i = 0; i < probe->name_count; i++) {
        ui_part_id part = ui_part_intern(probe->names[i]);
        assert(part != 0);
        for (size_t p = 0; p < sizeof(PSEUDOS) / sizeof(PSEUDOS[0]); p++) {
            struct css_style fresh =
                ui_widget_part_style(probe->widget, probe->names[i], PSEUDOS[p]);
            struct css_style cached =
                ui_widget_part(probe->widget, part, PSEUDOS[p]);
            assert(memcmp(&cached, &fresh, sizeof(fresh)) == 0);
        }
    }
}

static void expect_all_probes_match(const struct probe *probes, size_t count)
{
    for (size_t i = 0; i < count; i++)
        expect_parts_match_fresh(&probes[i]);
}

static void intern_handles_test(void)
{
    ui_part_id tab = ui_part_intern("tab");
    ui_part_id active = ui_part_intern("tab active");
    assert(tab != 0 && active != 0 && tab != active);
    assert(ui_part_intern("tab") == tab);
    assert(ui_part_intern("tab active") == active);
    assert(strcmp(ui_part_name(active), "tab active") == 0);

    assert(ui_part_intern(NULL) == 0);
    assert(ui_part_intern("") == 0);
    assert(ui_part_name(0) == NULL);

    enum { COUNT = 600 };
    static ui_part_id ids[COUNT];
    char name[32];
    for (int i = 0; i < COUNT; i++) {
        snprintf(name, sizeof(name), "stress-part-%d", i);
        ids[i] = ui_part_intern(name);
        assert(ids[i] != 0);
    }
    for (int i = 0; i < COUNT; i++) {
        snprintf(name, sizeof(name), "stress-part-%d", i);
        assert(ui_part_intern(name) == ids[i]);
        assert(strcmp(ui_part_name(ids[i]), name) == 0);
    }
    assert(ui_part_intern("tab") == tab);
}

static void cache_matches_string_api_test(void)
{
    struct css_sheet sheet_a = {0};
    struct css_sheet sheet_b = {0};
    assert(css_parse(&sheet_a, SHEET_A));
    assert(css_parse(&sheet_b, SHEET_B));

    struct ui_surface surface = {0};
    ui_surface_init(&surface, NULL, NULL);
    ui_surface_set_sheet(&surface, &sheet_a);

    struct ui_box root = {0};
    struct ui_tabbar tabbar = {0};
    struct ui_list list = {0};
    struct ui_label label = {0};
    struct ui_input input = {0};
    struct ui_scrollview scrollview = {0};
    ui_box_init(&root);
    ui_widget_set_style_identity(&root.widget, "panel", NULL, "panel");
    ui_tabbar_init(&tabbar);
    ui_widget_set_style_identity(&tabbar.widget, "tab-bar", NULL, "tab-bar");
    ui_list_init(&list);
    ui_label_init(&label);
    ui_input_init(&input);
    ui_scrollview_init(&scrollview);
    assert(ui_widget_add_child(&root.widget, &tabbar.widget));
    assert(ui_widget_add_child(&root.widget, &list.widget));
    assert(ui_widget_add_child(&root.widget, &label.widget));
    assert(ui_widget_add_child(&root.widget, &input.widget));
    assert(ui_widget_add_child(&root.widget, &scrollview.widget));
    assert(ui_surface_set_root(&surface, &root.widget));

    static const char *const TAB_PARTS[] = { "tab", "tab active", "tab-close" };
    static const char *const MENU_PARTS[] = {
        "menu-item", "menu-label", "menu-separator",
    };
    static const char *const LABEL_PARTS[] = { "label-selection", "label-link" };
    static const char *const INPUT_PARTS[] = {
        "text-input-text", "text-input-selection",
    };
    static const char *const SCROLL_PARTS[] = { "scrollbar", "scrollbar-thumb" };
    const struct probe probes[] = {
        { &tabbar.widget, TAB_PARTS, sizeof(TAB_PARTS) / sizeof(TAB_PARTS[0]) },
        { &list.widget, MENU_PARTS, sizeof(MENU_PARTS) / sizeof(MENU_PARTS[0]) },
        { &label.widget, LABEL_PARTS, sizeof(LABEL_PARTS) / sizeof(LABEL_PARTS[0]) },
        { &input.widget, INPUT_PARTS, sizeof(INPUT_PARTS) / sizeof(INPUT_PARTS[0]) },
        { &scrollview.widget, SCROLL_PARTS, sizeof(SCROLL_PARTS) / sizeof(SCROLL_PARTS[0]) },
    };
    const size_t probe_count = sizeof(probes) / sizeof(probes[0]);

    struct css_style active =
        ui_widget_part(&tabbar.widget, ui_part_intern("tab active"), 0);
    assert(active.has_bg_color && active.bg_color == 0xFF444444);
    struct css_style tab_text =
        ui_widget_part(&tabbar.widget, ui_part_intern("tab"), 0);
    assert(tab_text.has_text_color && tab_text.text_color == 0xFF0A0B0C);
    expect_all_probes_match(probes, probe_count);

    ui_part_id menu_item = ui_part_intern("menu-item");
    struct css_style item = ui_widget_part(&list.widget, menu_item, 0);
    expect_all_probes_match(probes, probe_count);
    assert(item.has_border_color && item.border_color == 0xFF123456);

    struct css_style item_before_sheet_b = item;
    ui_surface_set_sheet(&surface, &sheet_b);
    expect_all_probes_match(probes, probe_count);
    assert(item_before_sheet_b.border_color == 0xFF123456);
    active = ui_widget_part(&tabbar.widget, ui_part_intern("tab active"), 0);
    assert(active.bg_color == 0xFF445544);
    item = ui_widget_part(&list.widget, menu_item, 0);
    assert(item.border_color == 0xFF654321);
    tab_text = ui_widget_part(&tabbar.widget, ui_part_intern("tab"), 0);
    assert(tab_text.text_color == 0xFF0D0E0F);

    ui_widget_set_style_identity(&root.widget, "other", NULL, "box");
    expect_all_probes_match(probes, probe_count);
    item = ui_widget_part(&list.widget, menu_item, 0);
    assert(!item.has_border_color);
    ui_widget_set_style_identity(&root.widget, "panel", NULL, "panel");
    expect_all_probes_match(probes, probe_count);
    item = ui_widget_part(&list.widget, menu_item, 0);
    assert(item.has_border_color && item.border_color == 0xFF654321);

    ui_widget_set_style_identity(&tabbar.widget, "tab-bar-x", NULL, "tab-bar");
    expect_all_probes_match(probes, probe_count);
    tab_text = ui_widget_part(&tabbar.widget, ui_part_intern("tab"), 0);
    assert(tab_text.text_color == 0xFF223322);
    ui_widget_set_style_identity(&tabbar.widget, "tab-bar", NULL, "tab-bar");
    tab_text = ui_widget_part(&tabbar.widget, ui_part_intern("tab"), 0);
    assert(tab_text.text_color == 0xFF0D0E0F);
    expect_all_probes_match(probes, probe_count);

    assert(ui_widget_remove_child(&root.widget, &list.widget));
    expect_parts_match_fresh(&probes[1]);
    assert(ui_widget_add_child(&root.widget, &list.widget));
    expect_all_probes_match(probes, probe_count);

    struct css_style no_class = ui_widget_part_style(&list.widget, NULL, 0);
    struct css_style zero_handle = ui_widget_part(&list.widget, 0, 0);
    assert(memcmp(&zero_handle, &no_class, sizeof(no_class)) == 0);
    struct css_style null_widget = ui_widget_part(NULL, menu_item, 0);
    struct css_style empty = {0};
    assert(memcmp(&null_widget, &empty, sizeof(empty)) == 0);

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root.widget);
    css_sheet_clear(&sheet_a);
    css_sheet_clear(&sheet_b);
}

static void expect_abort(void (*fn)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

static void name_unknown_handle(void)
{
    ui_part_name((ui_part_id)UINT32_MAX);
}

static void part_unknown_handle(void)
{
    struct ui_box box = {0};
    ui_box_init(&box);
    ui_widget_part(&box.widget, (ui_part_id)UINT32_MAX, 0);
}

static void unknown_handles_abort_test(void)
{
    expect_abort(name_unknown_handle);
    expect_abort(part_unknown_handle);
}

int main(void)
{
    intern_handles_test();
    cache_matches_string_api_test();
    unknown_handles_abort_test();
    return 0;
}
