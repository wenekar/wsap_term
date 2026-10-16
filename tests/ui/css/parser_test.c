#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "ui/css.h"

static void test_css_parsing(void)
{
    struct css_sheet sheet = {0};
    const char *css =
        ".tab-bar {\n"
        "    height: 36px;\n"
        "    background: #181825;\n"
        "    flex-direction: row;\n"
        "    padding: 4px 8px;\n"
        "    gap: 4px;\n"
        "}\n"
        ".tab {\n"
        "    background-color: #1e1e2e;\n"
        "    color: #a6adc8;\n"
        "    border-radius: 6px;\n"
        "    padding: 0 12px;\n"
        "}\n"
        ".tab:hover {\n"
        "    background-color: #313244;\n"
        "    color: #cdd6f4;\n"
        "}\n"
        ".tab.active {\n"
        "    background-color: #45475a;\n"
        "    color: #ffffff;\n"
        "}\n";

    assert(css_parse(&sheet, css));
    assert(sheet.rule_count == 4);

    struct css_style bar_style;
    css_style_resolve(&sheet, "tab-bar", NULL, NULL, CSS_PSEUDO_NONE, &bar_style);
    assert(bar_style.height.val == 36.0f);
    assert(bar_style.height.unit == CSS_UNIT_PX);
    assert(bar_style.flex_direction == CSS_FLEX_ROW);
    assert(bar_style.gap.val == 4.0f);
    assert(bar_style.has_bg_color && bar_style.bg_color == 0xFF181825);
    assert(bar_style.padding.top.val == 4.0f);
    assert(bar_style.padding.right.val == 8.0f);

    struct css_style tab_idle;
    css_style_resolve(&sheet, "tab", NULL, NULL, CSS_PSEUDO_NONE, &tab_idle);
    assert(tab_idle.has_bg_color && tab_idle.bg_color == 0xFF1E1E2E);
    assert(tab_idle.border_radius.val == 6.0f);

    struct css_style tab_hover;
    css_style_resolve(&sheet, "tab", NULL, NULL, CSS_PSEUDO_HOVER, &tab_hover);
    assert(tab_hover.has_bg_color && tab_hover.bg_color == 0xFF313244);
    assert(tab_hover.has_text_color && tab_hover.text_color == 0xFFCDD6F4);

    struct css_style tab_act;
    css_style_resolve(&sheet, "tab active", NULL, NULL, CSS_PSEUDO_NONE, &tab_act);
    assert(tab_act.has_bg_color && tab_act.bg_color == 0xFF45475A);
    assert(tab_act.has_text_color && tab_act.text_color == 0xFFFFFFFF);
    css_sheet_clear(&sheet);
}

static void test_nonfinite_declarations_do_not_override(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".item { width: 42px; height: 10px; opacity: 0.5; }"
        ".item { width: nan; height: infpx; opacity: NaN; }"));
    struct css_style style;
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(style.width.unit == CSS_UNIT_PX && style.width.val == 42.0f);
    assert(style.height.unit == CSS_UNIT_PX && style.height.val == 10.0f);
    assert(style.opacity == 0.5f);
    css_sheet_clear(&sheet);
}

static void test_invalid_declarations_do_not_override(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".item { width: 42px; height: 10px; min-width: 5px; gap: 2px; background: #123456; }"
        ".item { width: 42pxextra; height: 100xyz; min-width: 20remfoo; gap: 8chbar; background: not-a-color; }"
        ".item { width: 42furlongs; }"
        ".item { width: nonsense; }"));
    struct css_style style;
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(style.width.unit == CSS_UNIT_PX && style.width.val == 42.0f);
    assert(style.height.unit == CSS_UNIT_PX && style.height.val == 10.0f);
    assert(style.min_width.unit == CSS_UNIT_PX && style.min_width.val == 5.0f);
    assert(style.gap.unit == CSS_UNIT_PX && style.gap.val == 2.0f);
    assert(style.has_bg_color && style.bg_color == 0xFF123456);
    css_sheet_clear(&sheet);
}

/* Every spelling a theme can use for a cursor has to reach the style. An
 * unknown one is dropped silently, so "none" used to leave the default. */
static void test_every_cursor_keyword(void)
{
    struct css_sheet sheet = {0};
    const char *names[] = { "auto", "default", "arrow", "text", "ibeam",
                            "pointer", "clickable", "link", "hand", "none" };
    size_t count = sizeof(names) / sizeof(names[0]);
    char css[512];
    int off = 0;
    for (size_t i = 0; i < count; i++)
        off += snprintf(css + off, sizeof(css) - (size_t)off,
                        ".k%zu { cursor: %s; }\n", i, names[i]);
    assert(css_parse(&sheet, css));

    for (size_t i = 0; i < count; i++) {
        char cls[16];
        snprintf(cls, sizeof(cls), "k%zu", i);
        struct css_style style;
        css_style_resolve(&sheet, cls, NULL, NULL, CSS_PSEUDO_NONE, &style);
        assert(style.has_cursor);
    }
    /* "none" is the one that has no other spelling and no other producer. */
    struct css_style none_style;
    css_style_resolve(&sheet, "k9", NULL, NULL, CSS_PSEUDO_NONE, &none_style);
    assert(none_style.cursor == UI_CURSOR_NONE);
    css_sheet_clear(&sheet);
}

static void test_tag_selectors_and_specificity(void)
{
    struct css_sheet sheet = {0};
    const char *css =
        "button { width: 100px; height: 30px; background: #111111; color: #aaaaaa; cursor: pointer; }\n"
        ".primary { background: #222222; color: #ffffff; }\n"
        "#special { width: 200px; background: #333333; }\n";

    assert(css_parse(&sheet, css));
    assert(sheet.rule_count == 3);

    struct css_style btn_style;
    css_style_resolve(&sheet, NULL, NULL, "button", CSS_PSEUDO_NONE, &btn_style);
    assert(btn_style.width.unit == CSS_UNIT_PX && btn_style.width.val == 100.0f);
    assert(btn_style.height.unit == CSS_UNIT_PX && btn_style.height.val == 30.0f);
    assert(btn_style.has_bg_color && btn_style.bg_color == 0xFF111111);
    assert(btn_style.has_text_color && btn_style.text_color == 0xFFAAAAAA);
    assert(btn_style.has_cursor && btn_style.cursor == UI_CURSOR_POINTER);

    struct css_style pri_btn;
    css_style_resolve(&sheet, "primary", NULL, "button", CSS_PSEUDO_NONE, &pri_btn);
    assert(pri_btn.width.val == 100.0f);
    assert(pri_btn.height.val == 30.0f);
    assert(pri_btn.has_bg_color && pri_btn.bg_color == 0xFF222222);
    assert(pri_btn.has_text_color && pri_btn.text_color == 0xFFFFFFFF);

    struct css_style special_btn;
    css_style_resolve(&sheet, "primary", "special", "button", CSS_PSEUDO_NONE, &special_btn);
    assert(special_btn.width.val == 200.0f);
    assert(special_btn.height.val == 30.0f);
    assert(special_btn.has_bg_color && special_btn.bg_color == 0xFF333333);
    assert(special_btn.has_text_color && special_btn.text_color == 0xFFFFFFFF);
    css_sheet_clear(&sheet);
}

static void test_transition_duration_parsing(void)
{
    struct css_sheet sheet = {0};
    const char *css =
        ".context-menu { transition-duration: 150ms; }\n"
        ".side-menu { animation-duration: 0.2s; }\n"
        ".other { transition: opacity 300ms; }\n";

    assert(css_parse(&sheet, css));
    struct css_style st1, st2, st3;
    css_style_resolve(&sheet, "context-menu", NULL, NULL, CSS_PSEUDO_NONE, &st1);
    assert(st1.has_transition_duration);
    assert(st1.transition_duration == 150.0f);

    css_style_resolve(&sheet, "side-menu", NULL, NULL, CSS_PSEUDO_NONE, &st2);
    assert(st2.has_transition_duration);
    assert(st2.transition_duration == 200.0f);

    css_style_resolve(&sheet, "other", NULL, NULL, CSS_PSEUDO_NONE, &st3);
    assert(st3.has_transition_duration);
    assert(st3.transition_duration == 300.0f);
    css_sheet_clear(&sheet);
}

static void test_box_shadow_parsing(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".a { box-shadow: 0 2px 6px #0000005A; }"
        ".b { box-shadow: 1px 2px; }"
        ".c { box-shadow: none; }"
        ".d { box-shadow: 4px; }"
        ".e { box-shadow: 0 2px 6px #0000005A; }"
        ".e { box-shadow: none; }"));

    struct css_style a, b, c, d, e;
    css_style_resolve(&sheet, "a", NULL, NULL, CSS_PSEUDO_NONE, &a);
    assert(a.has_shadow);
    assert(a.shadow_x.val == 0.0f);
    assert(a.shadow_y.val == 2.0f && a.shadow_y.unit == CSS_UNIT_PX);
    assert(a.shadow_blur.val == 6.0f && a.shadow_blur.unit == CSS_UNIT_PX);
    assert(a.shadow_color == 0x5A000000u);

    css_style_resolve(&sheet, "b", NULL, NULL, CSS_PSEUDO_NONE, &b);
    assert(b.has_shadow);
    assert(b.shadow_blur.unit == CSS_UNIT_NONE);

    css_style_resolve(&sheet, "c", NULL, NULL, CSS_PSEUDO_NONE, &c);
    assert(!c.has_shadow);

    css_style_resolve(&sheet, "d", NULL, NULL, CSS_PSEUDO_NONE, &d);
    assert(!d.has_shadow);

    css_style_resolve(&sheet, "e", NULL, NULL, CSS_PSEUDO_NONE, &e);
    assert(!e.has_shadow);
    css_sheet_clear(&sheet);
}

static void test_selector_comment_brace(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet, ".a, /* { */ .b { width: 1px; }"));
    assert(sheet.rule_count <= 1);
    css_sheet_clear(&sheet);
}

static void test_layout_property_flags(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".b { flex-direction: column; align-items: center; }"
        ".j { justify-content: end; }"));

    struct css_style b;
    css_style_resolve(&sheet, "b", NULL, NULL, CSS_PSEUDO_NONE, &b);
    assert(b.has_flex_direction && b.flex_direction == CSS_FLEX_COLUMN);
    assert(b.has_align_items && b.align_items == CSS_ALIGN_CENTER);
    assert(!b.has_justify_content);

    struct css_style j;
    css_style_resolve(&sheet, "j", NULL, NULL, CSS_PSEUDO_NONE, &j);
    assert(j.has_justify_content &&
           j.justify_content == CSS_JUSTIFY_END);
    assert(!j.has_flex_direction && !j.has_align_items);

    struct css_style unset;
    css_style_resolve(&sheet, "unset", NULL, NULL, CSS_PSEUDO_NONE, &unset);
    assert(!unset.has_flex_direction && !unset.has_align_items &&
           !unset.has_justify_content);
    css_sheet_clear(&sheet);
}

static void test_insets_and_named_colors(void)
{
    struct css_sheet sheet = {0};
    const char *css =
        ".p1 { padding: 10px; color: red; }\n"
        ".p2 { padding: 10px 20px; color: green; }\n"
        ".p3 { padding: 10px 20px 30px; color: blue; }\n"
        ".p4 { padding: 10px 20px 30px 40px; color: transparent; }\n"
        ".c1 { color: darkgray; background-color: silver; }\n"
        ".c2 { color: darkgrey; background-color: dimgrey; }\n";

    assert(css_parse(&sheet, css));

    struct css_style s1, s2, s3, s4, c1, c2;
    css_style_resolve(&sheet, "p1", NULL, NULL, CSS_PSEUDO_NONE, &s1);
    assert(s1.padding.top.val == 10.0f && s1.padding.right.val == 10.0f &&
           s1.padding.bottom.val == 10.0f && s1.padding.left.val == 10.0f);
    assert(s1.has_text_color && s1.text_color == 0xFFFF0000u);

    css_style_resolve(&sheet, "p2", NULL, NULL, CSS_PSEUDO_NONE, &s2);
    assert(s2.padding.top.val == 10.0f && s2.padding.right.val == 20.0f &&
           s2.padding.bottom.val == 10.0f && s2.padding.left.val == 20.0f);
    assert(s2.has_text_color && s2.text_color == 0xFF008000u);

    css_style_resolve(&sheet, "p3", NULL, NULL, CSS_PSEUDO_NONE, &s3);
    assert(s3.padding.top.val == 10.0f && s3.padding.right.val == 20.0f &&
           s3.padding.bottom.val == 30.0f && s3.padding.left.val == 20.0f);
    assert(s3.has_text_color && s3.text_color == 0xFF0000FFu);

    css_style_resolve(&sheet, "p4", NULL, NULL, CSS_PSEUDO_NONE, &s4);
    assert(s4.padding.top.val == 10.0f && s4.padding.right.val == 20.0f &&
           s4.padding.bottom.val == 30.0f && s4.padding.left.val == 40.0f);
    assert(s4.has_text_color && s4.text_color == 0x00000000u);

    css_style_resolve(&sheet, "c1", NULL, NULL, CSS_PSEUDO_NONE, &c1);
    assert(c1.has_text_color && c1.text_color == 0xFFA9A9A9u);
    assert(c1.has_bg_color && c1.bg_color == 0xFFC0C0C0u);

    css_style_resolve(&sheet, "c2", NULL, NULL, CSS_PSEUDO_NONE, &c2);
    assert(c2.has_text_color && c2.text_color == 0xFFA9A9A9u);
    assert(c2.has_bg_color && c2.bg_color == 0xFF696969u);

    css_sheet_clear(&sheet);
}

int main(void)
{
    test_css_parsing();
    test_insets_and_named_colors();
    test_layout_property_flags();
    test_selector_comment_brace();
    test_invalid_declarations_do_not_override();
    test_nonfinite_declarations_do_not_override();
    test_tag_selectors_and_specificity();
    test_every_cursor_keyword();
    test_transition_duration_parsing();
    test_box_shadow_parsing();
    printf("PASS: CSS parser tests\n");
    return 0;
}
