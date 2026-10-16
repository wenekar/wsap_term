#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/css.h"

static void test_compound_beats_single_both_orders(void)
{
    struct css_sheet s1 = {0}, s2 = {0};
    assert(css_parse(&s1, ".menu-item.disabled { color: #111111; }"
                          ".menu-item { color: #222222; }"));
    assert(css_parse(&s2, ".menu-item { color: #222222; }"
                          ".menu-item.disabled { color: #111111; }"));
    struct css_style st;
    css_style_resolve(&s1, "menu-item disabled", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF111111);
    css_style_resolve(&s2, "menu-item disabled", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF111111);
    css_style_resolve(&s2, "menu-item", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.text_color == 0xFF222222);
    struct css_sheet s3 = {0};
    assert(css_parse(&s3, ".a { color: #111111; } .b { color: #333333; }"));
    css_style_resolve(&s3, "a b", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.text_color == 0xFF333333);
    css_sheet_clear(&s3);
    css_sheet_clear(&s2);
    css_sheet_clear(&s1);
}

static void test_disabled_checked_pseudo(void)
{
    struct css_sheet s = {0};
    assert(css_parse(&s,
        ".item { color: #ffffff; }"
        ".item:disabled { color: #555555; }"
        ".item:checked { color: #00ff00; }"));
    struct css_style st;
    css_style_resolve(&s, "item", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.text_color == 0xFFFFFFFF);
    css_style_resolve(&s, "item", NULL, NULL, CSS_PSEUDO_DISABLED, &st);
    assert(st.text_color == 0xFF555555);
    css_style_resolve(&s, "item", NULL, NULL, CSS_PSEUDO_CHECKED, &st);
    assert(st.text_color == 0xFF00FF00);
    css_style_resolve(&s, "item", NULL, NULL, CSS_PSEUDO_HOVER, &st);
    assert(st.text_color == 0xFFFFFFFF);
    css_sheet_clear(&s);
}

static void test_descendant_selectors(void)
{
    struct css_sheet s = {0};
    assert(css_parse(&s,
        ".menu-item { color: #aaaaaa; }"
        ".context-menu .menu-item { color: #bbbbbb; }"));
    struct css_style st;
    css_style_resolve(&s, "menu-item", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.text_color == 0xFFAAAAAA);
    css_style_resolve_ex(&s, "menu-item", NULL, NULL, CSS_PSEUDO_NONE,
                         "context-menu", &st);
    assert(st.text_color == 0xFFBBBBBB);
    css_style_resolve_ex(&s, "menu-item", NULL, NULL, CSS_PSEUDO_NONE,
                         "other-root", &st);
    assert(st.text_color == 0xFFAAAAAA);

    struct css_sheet s2 = {0};
    assert(css_parse(&s2, ".c { width: 1px; } .a .b .c { width: 9px; }"));
    css_style_resolve_ex(&s2, "c", NULL, NULL, CSS_PSEUDO_NONE, "a\nb", &st);
    assert(st.width.val == 9.0f);
    css_style_resolve_ex(&s2, "c", NULL, NULL, CSS_PSEUDO_NONE, "b\na", &st);
    assert(st.width.val == 1.0f);
    css_style_resolve_ex(&s2, "c", NULL, NULL, CSS_PSEUDO_NONE, "a\nx\nb", &st);
    assert(st.width.val == 9.0f);
    css_sheet_clear(&s2);
    css_sheet_clear(&s);
}

static void test_named_colors(void)
{
    struct css_sheet s = {0};
    assert(css_parse(&s,
        ".a { background: transparent; color: white; border-color: grey; }"
        ".b { background: darkgray; }"));
    struct css_style st;
    css_style_resolve(&s, "a", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_bg_color && st.bg_color == 0x00000000);
    assert(st.has_text_color && st.text_color == 0xFFFFFFFF);
    assert(st.has_border_color && st.border_color == 0xFF808080);
    css_style_resolve(&s, "b", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_bg_color && st.bg_color == 0xFFA9A9A9);
    css_sheet_clear(&s);
}

static void test_truncation_and_overflow_warn_but_survive(void)
{
    struct css_sheet s = {0};
    assert(css_parse(&s,
        ".this-selector-name-is-way-too-long-to-fit { width: 10px; }"
        ".ok { a-property-name-that-is-far-too-long: 1px; unknown-prop: 2px;"
        " color: #123456789012345678901234567890123456789012345678901234567890;"
        " width: 7px; }"));
    struct css_style st;
    css_style_resolve(&s, "ok", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.width.val == 7.0f);

    enum { RULES = 1000 };
    static char buf[RULES * 32];
    size_t pos = 0;
    for (int i = 0; i < RULES; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        ".r%04d { width: %dpx; }", i, i);
    struct css_sheet big = {0};
    assert(css_parse(&big, buf));
    css_style_resolve(&big, "r0999", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.width.val == 999.0f);

    char sel[256];
    memset(sel, 'x', sizeof(sel));
    sel[0] = '.';
    sel[sizeof(sel) - 1] = '\0';
    char rule[320];
    snprintf(rule, sizeof(rule), "%s { width: 3px; }", sel);
    assert(css_parse(&big, rule));
    css_style_resolve(&big, sel + 1, NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.width.val == 3.0f);
    css_sheet_clear(&big);
    css_sheet_clear(&s);
}

int main(void)
{
    test_compound_beats_single_both_orders();
    test_disabled_checked_pseudo();
    test_descendant_selectors();
    test_named_colors();
    test_truncation_and_overflow_warn_but_survive();
    printf("PASS: CSS selector tests\n");
    return 0;
}
