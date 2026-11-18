#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "ui/css.h"

struct css_messages {
    int count;
    enum css_diagnostic_kind kind[16];
    size_t line[16], column[16];
    char property[16][32], value[16][32];
};

static void record_css_message(const struct css_diagnostic *message, void *userdata)
{
    struct css_messages *messages = userdata;
    assert(messages->count < 16);
    int i = messages->count++;
    messages->kind[i] = message->kind;
    messages->line[i] = message->line;
    messages->column[i] = message->column;
    snprintf(messages->property[i], sizeof(messages->property[i]), "%s", message->property);
    snprintf(messages->value[i], sizeof(messages->value[i]), "%s", message->value);
}

static void test_declaration_diagnostics(void)
{
    struct css_sheet sheet = {0};
    struct css_messages messages = {0};
    const char *css = ".item {\n  width: 12px;\n  widht: 50px;\n"
                      "  color: nope;\n  padding: 4px invalid;\n}";
    assert(css_parse_with_diagnostics(&sheet, css, record_css_message, &messages));
    assert(messages.count == 3);
    assert(messages.kind[0] == CSS_DIAGNOSTIC_UNKNOWN_PROPERTY);
    assert(messages.line[0] == 3 && messages.column[0] == 3);
    assert(!strcmp(messages.property[0], "widht") && !strcmp(messages.value[0], "50px"));
    assert(messages.kind[1] == CSS_DIAGNOSTIC_INVALID_VALUE);
    assert(messages.line[1] == 4 && !strcmp(messages.property[1], "color"));
    assert(messages.kind[2] == CSS_DIAGNOSTIC_INVALID_VALUE);
    assert(messages.line[2] == 5 && !strcmp(messages.property[2], "padding"));
    struct css_style style;
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(style.width.unit == CSS_UNIT_PX && style.width.val == 12.0f);
    assert(!style.has_text_color);
    css_sheet_clear(&sheet);

    assert(css_parse_with_diagnostics(&sheet,
        ".item { padding: 5px; padding: 1px invalid; }", NULL, NULL));
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(style.padding.top.unit == CSS_UNIT_PX && style.padding.top.val == 5.0f);
    css_sheet_clear(&sheet);

    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ".item {\n  bad declaration;\n  width: 8px;\n", record_css_message, &messages));
    assert(messages.count == 2);
    assert(messages.kind[0] == CSS_DIAGNOSTIC_INVALID_SYNTAX);
    assert(messages.line[0] == 2 && messages.column[0] == 3);
    assert(messages.kind[1] == CSS_DIAGNOSTIC_INVALID_SYNTAX);
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(style.width.unit == CSS_UNIT_PX && style.width.val == 8.0f);
    css_sheet_clear(&sheet);

    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ".item:hovre, .other { color: red; }", record_css_message, &messages));
    assert(messages.count == 1);
    assert(messages.kind[0] == CSS_DIAGNOSTIC_UNSUPPORTED_SELECTOR);
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(!style.has_text_color);
    css_style_resolve(&sheet, "other", NULL, NULL, CSS_PSEUDO_NONE, &style);
    assert(style.has_text_color && style.text_color == 0xffff0000u);
    css_sheet_clear(&sheet);
}

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

static void test_every_cursor_keyword(void)
{
    struct css_sheet sheet = {0};
    const char *names[] = { "auto", "default", "arrow", "text", "ibeam",
                            "pointer", "clickable", "link", "hand", "none",
                            "crosshair", "move", "grab", "ew-resize",
                            "ns-resize", "nwse-resize", "nesw-resize" };
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
    struct css_style none_style;
    css_style_resolve(&sheet, "k9", NULL, NULL, CSS_PSEUDO_NONE, &none_style);
    assert(none_style.cursor == UI_CURSOR_NONE);
    struct css_style resize_style;
    css_style_resolve(&sheet, "k13", NULL, NULL, CSS_PSEUDO_NONE, &resize_style);
    assert(resize_style.cursor == UI_CURSOR_RESIZE_EW);
    css_style_resolve(&sheet, "k14", NULL, NULL, CSS_PSEUDO_NONE, &resize_style);
    assert(resize_style.cursor == UI_CURSOR_RESIZE_NS);
    css_style_resolve(&sheet, "k15", NULL, NULL, CSS_PSEUDO_NONE, &resize_style);
    assert(resize_style.cursor == UI_CURSOR_RESIZE_NWSE);
    css_style_resolve(&sheet, "k16", NULL, NULL, CSS_PSEUDO_NONE, &resize_style);
    assert(resize_style.cursor == UI_CURSOR_RESIZE_NESW);
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
    struct css_messages messages = {0};
    assert(css_parse_with_diagnostics(&sheet,
        ".a, /* {, } */ .b { width: 1px; }", record_css_message, &messages));
    assert(messages.count == 0);
    struct css_style a, b;
    css_style_resolve(&sheet, "a", NULL, NULL, CSS_PSEUDO_NONE, &a);
    css_style_resolve(&sheet, "b", NULL, NULL, CSS_PSEUDO_NONE, &b);
    assert(a.width.unit == CSS_UNIT_PX && a.width.val == 1.0f);
    assert(b.width.unit == CSS_UNIT_PX && b.width.val == 1.0f);
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

/* Two spellings of one property reach the same field. The last one wins,
 * both inside a rule and across rules. */
static void test_property_spellings_share_a_field(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".a { background-color: #111111; background: #222222; }"
        ".b { border-width: 3px; border: 5px; }"
        ".c { transition: 250ms; animation-duration: 750ms; }"
        ".d { background: #333333; }"
        ".d { background-color: #444444; }"));
    struct css_style a, b, c, d;
    css_style_resolve(&sheet, "a", NULL, NULL, CSS_PSEUDO_NONE, &a);
    assert(a.has_bg_color && a.bg_color == 0xFF222222u);
    css_style_resolve(&sheet, "b", NULL, NULL, CSS_PSEUDO_NONE, &b);
    assert(b.border_width.unit == CSS_UNIT_PX && b.border_width.val == 5.0f);
    css_style_resolve(&sheet, "c", NULL, NULL, CSS_PSEUDO_NONE, &c);
    assert(c.has_transition_duration && c.transition_duration == 750.0f);
    css_style_resolve(&sheet, "d", NULL, NULL, CSS_PSEUDO_NONE, &d);
    assert(d.has_bg_color && d.bg_color == 0xFF444444u);
    css_sheet_clear(&sheet);
}

/* Verify :root custom properties, var() expansion, fallbacks, and overrides. */
static void test_root_variables_and_var_expansion(void)
{
    struct css_sheet sheet = {0};
    struct css_messages messages = {0};
    const char *css =
        ":root {\n"
        "  --accent: #3584E4;\n"
        "  --derived: var(--accent);\n"
        "  --shadow-c: #0000005A;\n"
        "  --radius: 8px;\n"
        "}\n"
        ".a, .b { border-radius: var(--radius); box-shadow: 0 2px 6px var(--shadow-c); }\n"
        ".c { color: var(--derived); background: var(--missing, #112233); }\n"
        ".d { color: var(--missing1, var(--missing2, #445566)); }\n"
        ".over { color: var(--accent); color: #123456; }\n"
        ".late { color: var(--late-fg); }\n"
        ":root { --late-fg: #778899; }\n";
    assert(css_parse_with_diagnostics(&sheet, css, record_css_message, &messages));
    assert(messages.count == 0);

    struct css_style st;
    css_style_resolve(&sheet, "a", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.border_radius.unit == CSS_UNIT_PX && st.border_radius.val == 8.0f);
    assert(st.has_shadow && st.shadow_color == 0x5A000000u && st.shadow_blur.val == 6.0f);

    css_style_resolve(&sheet, "b", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.border_radius.unit == CSS_UNIT_PX && st.border_radius.val == 8.0f);
    assert(st.has_shadow && st.shadow_color == 0x5A000000u);

    css_style_resolve(&sheet, "c", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF3584E4u);
    assert(st.has_bg_color && st.bg_color == 0xFF112233u);

    css_style_resolve(&sheet, "d", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF445566u);

    css_style_resolve(&sheet, "late", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF778899u);

    /* Updating :root re-evaluates .c and .d but leaves explicit #123456 intact. */
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --accent: #ABCDEF; --missing2: #654321; }",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_style_resolve(&sheet, "c", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFFABCDEFu);
    css_style_resolve(&sheet, "d", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF654321u);
    css_style_resolve(&sheet, "over", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF123456u);

    /* css_init_default rules also re-resolve cleanly on :root override. */
    css_sheet_clear(&sheet);
    css_init_default(&sheet);
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --accent: #224466; --surface: #102030; }",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_style_resolve(&sheet, "label-link", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF224466u);
    css_style_resolve(&sheet, "context-menu", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_bg_color && st.bg_color == 0xFF102030u);
    css_sheet_clear(&sheet);
}

/* Verify missing vars, cycles, and malformed var() syntax emit diagnostics cleanly. */
static void test_var_diagnostics_and_guards(void)
{
    struct css_sheet sheet = {0};
    struct css_messages messages = {0};
    const char *css =
        ":root { --a: var(--b); --b: var(--a); --self: var(--self); --: #111; width: 10px; }\n"
        ".x { color: #111111; color: var(--missing); }\n"
        ".y { color: var(--a, #999999); }\n"
        ".z { color: var(--self); background: var(--unclosed; }\n"
        ".w { color: var(nodash); width: var(--a); }\n";
    assert(css_parse_with_diagnostics(&sheet, css, record_css_message, &messages));
    assert(messages.count == 8);
    assert(messages.kind[0] == CSS_DIAGNOSTIC_INVALID_VALUE);
    assert(messages.kind[1] == CSS_DIAGNOSTIC_UNKNOWN_PROPERTY);
    assert(messages.kind[2] == CSS_DIAGNOSTIC_INVALID_VALUE); /* var(--a, #999999) cycle */
    assert(messages.kind[3] == CSS_DIAGNOSTIC_INVALID_VALUE); /* var(--self) */
    assert(messages.kind[4] == CSS_DIAGNOSTIC_INVALID_VALUE); /* var(--unclosed */
    assert(messages.kind[5] == CSS_DIAGNOSTIC_INVALID_VALUE); /* var(nodash) */
    assert(messages.kind[6] == CSS_DIAGNOSTIC_INVALID_VALUE); /* width: var(--a) */
    assert(messages.kind[7] == CSS_DIAGNOSTIC_INVALID_VALUE); /* end-of-parse var(--missing) */
    bool found_missing = false;
    for (int i = 0; i < messages.count; i++) {
        if (messages.line[i] == 2 && !strcmp(messages.value[i], "var(--missing)"))
            found_missing = true;
    }
    assert(found_missing);

    struct css_style st;
    css_style_resolve(&sheet, "x", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF111111u);
    css_style_resolve(&sheet, "y", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(!st.has_text_color);
    css_sheet_clear(&sheet);

    /* Updating an active :root variable to an invalid value reports and preserves prior style. */
    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --c: #112233; }\n.ok { color: var(--c); }",
        record_css_message, &messages));
    assert(messages.count == 0);
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --c: bogus; }",
        record_css_message, &messages));
    assert(messages.count == 1 && messages.kind[0] == CSS_DIAGNOSTIC_INVALID_VALUE);
    /* The diagnostic names the declaration line and the variable that failed. */
    assert(!strcmp(messages.property[0], "color"));
    assert(!strcmp(messages.value[0], "var(--c)"));
    css_style_resolve(&sheet, "ok", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF112233u);
    css_sheet_clear(&sheet);
}

/* Verify that a value waits for a later :root block, that a broken variable
 * reports once, and that the caps hold. */
static void test_var_edge_cases(void)
{
    struct css_sheet sheet = {0};
    struct css_messages messages = {0};
    struct css_style st;

    /* A pending var() does not erase an earlier valid var() unless it resolves. */
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --good: #112233; }\n"
        ".keep { color: var(--good); color: var(--missing); }\n"
        ".take { color: var(--good); color: var(--later); }\n"
        ":root { --later: #778899; }\n",
        record_css_message, &messages));
    assert(messages.count == 1 && !strcmp(messages.value[0], "var(--missing)"));
    css_style_resolve(&sheet, "keep", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF112233u);
    css_style_resolve(&sheet, "take", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF778899u);

    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet, ":root { --good: #445566; }",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_style_resolve(&sheet, "keep", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF445566u);
    css_style_resolve(&sheet, "take", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF778899u);
    css_sheet_clear(&sheet);

    /* A broken variable reports the declaration once; a later edit stays quiet. */
    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --a: 4px; --b: 8px; --c: #112233; }\n"
        ".pad { padding: var(--a) var(--b); }\n"
        ".fg { color: var(--c); }\n",
        record_css_message, &messages));
    assert(messages.count == 0);

    assert(css_parse_with_diagnostics(&sheet, ":root { --b: bogus; }",
        record_css_message, &messages));
    assert(messages.count == 1 && !strcmp(messages.property[0], "padding"));
    assert(!strcmp(messages.value[0], "var(--a) var(--b)"));
    css_style_resolve(&sheet, "pad", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.padding.top.val == 4.0f && st.padding.right.val == 8.0f);

    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet, ":root { --unrelated: #abcdef; }",
        record_css_message, &messages));
    assert(messages.count == 0);

    assert(css_parse_with_diagnostics(&sheet, ":root { --a: 12px; }",
        record_css_message, &messages));
    assert(messages.count == 0);

    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --a: 6px; --b: 10px; --c: #223344; }",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_style_resolve(&sheet, "pad", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.padding.top.val == 6.0f && st.padding.right.val == 10.0f);

    assert(css_parse_with_diagnostics(&sheet, ":root { --a: 14px; --b: nope; }",
        record_css_message, &messages));
    assert(messages.count == 1 && !strcmp(messages.property[0], "padding"));
    css_sheet_clear(&sheet);

    /* Fallback expansion pops v->name, while self-fallback cycles still fail. */
    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --a: var(--missing); }\n"
        ".fb { color: var(--a, var(--a, #112233)); }\n",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_style_resolve(&sheet, "fb", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF112233u);

    assert(css_parse_with_diagnostics(&sheet,
        ":root { --e: var(--a, var(--e)); }\n"
        ".cyc { color: var(--e); }\n",
        record_css_message, &messages));
    assert(messages.count == 1 && messages.kind[0] == CSS_DIAGNOSTIC_INVALID_VALUE);
    css_style_resolve(&sheet, "cyc", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(!st.has_text_color);
    css_sheet_clear(&sheet);

    /* Exponential variable expansion hits the byte cap cleanly without failing OOM. */
    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root {\n"
        "  --v0: 1px;\n"
        "  --v1: var(--v0) var(--v0); --v2: var(--v1) var(--v1);\n"
        "  --v3: var(--v2) var(--v2); --v4: var(--v3) var(--v3);\n"
        "  --v5: var(--v4) var(--v4); --v6: var(--v5) var(--v5);\n"
        "  --v7: var(--v6) var(--v6); --v8: var(--v7) var(--v7);\n"
        "  --v9: var(--v8) var(--v8); --v10: var(--v9) var(--v9);\n"
        "  --v11: var(--v10) var(--v10); --v12: var(--v11) var(--v11);\n"
        "  --v13: var(--v12) var(--v12); --v14: var(--v13) var(--v13);\n"
        "  --v15: var(--v14) var(--v14); --v16: var(--v15) var(--v15);\n"
        "  --v17: var(--v16) var(--v16); --v18: var(--v17) var(--v17);\n"
        "}\n"
        ".bomb { width: 10px; width: var(--v18); }\n",
        record_css_message, &messages));
    assert(messages.count == 1 && messages.kind[0] == CSS_DIAGNOSTIC_INVALID_VALUE);
    css_style_resolve(&sheet, "bomb", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.width.unit == CSS_UNIT_PX && st.width.val == 10.0f);
    css_sheet_clear(&sheet);

    /* Preserving saw_dirty across missing fallback emits diagnostics when broken. */
    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --a: #112233; }\n"
        ".cls { color: var(--a, var(--still-missing)); }\n",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_style_resolve(&sheet, "cls", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF112233u);

    assert(css_parse_with_diagnostics(&sheet, ":root { --a: var(--missing2); }",
        record_css_message, &messages));
    assert(messages.count == 1 && messages.kind[0] == CSS_DIAGNOSTIC_INVALID_VALUE);
    css_style_resolve(&sheet, "cls", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF112233u);

    messages = (struct css_messages){0};
    assert(css_parse_with_diagnostics(&sheet, ":root { --unrelated: #445566; }",
        record_css_message, &messages));
    assert(messages.count == 0);
    css_sheet_clear(&sheet);

    /* Appending rules with no :root block evaluates against dst's existing :root. */
    struct css_sheet dst = {0};
    struct css_sheet src = {0};
    assert(css_parse(&dst, ":root { --accent: #89b4fa; }"));
    assert(css_parse(&src, ".panel { color: var(--accent, #000000); }"));
    assert(css_sheet_append(&dst, &src));
    css_style_resolve(&dst, "panel", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0xFF89B4FAu);
    css_sheet_clear(&src);
    css_sheet_clear(&dst);
}

/* A cycle reports the declaration line and the variable that closes it. */
static void test_cyclic_variable_diagnostic(void)
{
    struct css_sheet sheet = {0};
    struct css_messages messages = {0};
    assert(css_parse_with_diagnostics(&sheet,
        ":root { --a: var(--b); }\n"
        ":root { --b: var(--a); }\n"
        ".x {\n  color: var(--a);\n}\n",
        record_css_message, &messages));
    assert(messages.count == 1 && messages.kind[0] == CSS_DIAGNOSTIC_INVALID_VALUE);
    assert(messages.line[0] == 4 && messages.column[0] == 3);
    assert(!strcmp(messages.property[0], "color"));
    assert(!strcmp(messages.value[0], "var(--a)"));
    struct css_style st;
    css_style_resolve(&sheet, "x", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(!st.has_text_color);
    css_sheet_clear(&sheet);
}

/* A declaration before the :root block that defines its variable resolves. */
static void test_variable_defined_after_its_use(void)
{
    struct css_sheet sheet = {0};
    struct css_messages messages = {0};
    assert(css_parse_with_diagnostics(&sheet,
        ".a { width: var(--late); }\n"
        ":root { --late: 12px; }\n",
        record_css_message, &messages));
    assert(messages.count == 0);
    struct css_style st;
    css_style_resolve(&sheet, "a", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.width.unit == CSS_UNIT_PX && st.width.val == 12.0f);
    css_sheet_clear(&sheet);
}

/* `border` sets a width and a color, ignores the style keyword, and `none`
 * clears the width. A value with no width leaves the width alone. */
static void test_border_shorthand(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".a { border: 1px solid #ff0000; }"
        ".b { border: 2px dashed blue; }"
        ".c { border: 3px dotted; }"
        ".d { border: none; }"
        ".e { border-width: 4px; border: 5px solid #00ff00; }"
        ".f { border-width: 6px; border: solid #112233; }"
        ":root { --bw: 7px; }"
        ".g { border-width: var(--bw); border: solid #445566; }"
        ".h { border-width: 8px; }"
        ".i { border: solid #778899; }"));

    struct css_style a;
    css_style_resolve(&sheet, "a", NULL, NULL, CSS_PSEUDO_NONE, &a);
    assert(a.border_width.unit == CSS_UNIT_PX && a.border_width.val == 1.0f);
    assert(a.has_border_color && a.border_color == 0xFFFF0000u);

    struct css_style b;
    css_style_resolve(&sheet, "b", NULL, NULL, CSS_PSEUDO_NONE, &b);
    assert(b.border_width.val == 2.0f);
    assert(b.has_border_color && b.border_color == 0xFF0000FFu);

    struct css_style c;
    css_style_resolve(&sheet, "c", NULL, NULL, CSS_PSEUDO_NONE, &c);
    assert(c.border_width.val == 3.0f);

    struct css_style d;
    css_style_resolve(&sheet, "d", NULL, NULL, CSS_PSEUDO_NONE, &d);
    assert(d.border_width.unit == CSS_UNIT_PX && d.border_width.val == 0.0f);
    assert(!d.has_border_color);

    struct css_style e;
    css_style_resolve(&sheet, "e", NULL, NULL, CSS_PSEUDO_NONE, &e);
    assert(e.border_width.val == 5.0f);
    assert(e.has_border_color && e.border_color == 0xFF00FF00u);

    /* A color-only shorthand keeps an earlier width, in the same rule, in a
       rule below it, and in a declaration that holds var(). */
    struct css_style f;
    css_style_resolve(&sheet, "f", NULL, NULL, CSS_PSEUDO_NONE, &f);
    assert(f.border_width.unit == CSS_UNIT_PX && f.border_width.val == 6.0f);
    assert(f.has_border_color && f.border_color == 0xFF112233u);

    struct css_style g;
    css_style_resolve(&sheet, "g", NULL, NULL, CSS_PSEUDO_NONE, &g);
    assert(g.border_width.unit == CSS_UNIT_PX && g.border_width.val == 7.0f);
    assert(g.has_border_color && g.border_color == 0xFF445566u);

    struct css_style hi;
    css_style_resolve(&sheet, "h i", NULL, NULL, CSS_PSEUDO_NONE, &hi);
    assert(hi.border_width.unit == CSS_UNIT_PX && hi.border_width.val == 8.0f);
    assert(hi.has_border_color && hi.border_color == 0xFF778899u);
    css_sheet_clear(&sheet);
}

/* The UI core sheet holds no app rule. A sheet that a client appends holds
 * the app rules, and the variables of the core sheet reach them. */
static void test_appended_sheet_holds_app_rules(void)
{
    static const char *const app_classes[] = {
        "tab", "tab-bar", "tab-close", "tab-new", "tab-preview",
        "command-palette", "settings-form", "settings-row",
        "terminal-drop-target",
    };
    const size_t count = sizeof(app_classes) / sizeof(app_classes[0]);

    struct css_sheet paper = {0};
    css_init_default(&paper);
    for (size_t i = 0; i < count; i++) {
        struct css_style st;
        css_style_resolve(&paper, app_classes[i], NULL, NULL, CSS_PSEUDO_NONE, &st);
        assert(!st.has_bg_color && !st.has_text_color && !st.has_border_color);
        assert(!st.has_cursor && st.width.unit == CSS_UNIT_AUTO);
    }
    css_sheet_clear(&paper);

    struct css_sheet sheet = {0};
    css_init_default(&sheet);
    assert(css_parse(&sheet, ".tab { background: var(--surface); cursor: pointer; }"));
    struct css_style tab;
    css_style_resolve(&sheet, "tab", NULL, NULL, CSS_PSEUDO_NONE, &tab);
    assert(tab.has_bg_color && tab.bg_color == 0xFF1E1E2Eu);
    assert(tab.has_cursor && tab.cursor == UI_CURSOR_POINTER);
    css_sheet_clear(&sheet);
}


static void extended_properties_test(void)
{
    struct css_sheet sheet = {0};
    assert(css_parse(&sheet,
        ".item {"
        " font-family: \"DejaVu Sans\", monospace; font-size: 14px;"
        " font-weight: 600; font-style: italic; line-height: 1.5;"
        " overflow: hidden; position: relative; z-index: 7;"
        " top: 2px; right: 3px; bottom: 4px; left: 5px;"
        " padding-top: 1px; padding-right: 2px;"
        " padding-bottom: 3px; padding-left: 4px;"
        " margin-top: 5px; margin-right: 6px;"
        " margin-bottom: 7px; margin-left: 8px;"
        " border-color: rgb(1, 2, 3); border-width: 1px;"
        " border-top-width: 2px; border-left-width: 4px; }"
        ".alpha { color: rgba(10, 20, 30, 128); }"
        ".plain { width: 10px; }"));

    struct css_style st;
    css_style_resolve(&sheet, "item", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_font_family &&
           !strcmp(st.font_family, "\"DejaVu Sans\", monospace"));
    assert(st.has_font_size && st.font_size.unit == CSS_UNIT_PX &&
           st.font_size.val == 14.0f);
    assert(st.has_font_weight && st.font_weight == 600);
    assert(st.has_font_style && st.font_style == CSS_FONT_ITALIC);
    assert(st.has_line_height &&
           st.line_height.unit == CSS_UNIT_PERCENT &&
           st.line_height.val == 150.0f);
    assert(st.has_overflow && st.overflow == CSS_OVERFLOW_HIDDEN);
    assert(st.has_position && st.position == CSS_POSITION_RELATIVE);
    assert(st.has_z_index && st.z_index == 7);
    assert(st.inset[0].val == 2.0f && st.inset[1].val == 3.0f &&
           st.inset[2].val == 4.0f && st.inset[3].val == 5.0f);
    assert(st.padding.top.val == 1.0f && st.padding.right.val == 2.0f &&
           st.padding.bottom.val == 3.0f && st.padding.left.val == 4.0f);
    assert(st.margin.top.val == 5.0f && st.margin.right.val == 6.0f &&
           st.margin.bottom.val == 7.0f && st.margin.left.val == 8.0f);
    assert(st.has_border_color && st.border_color == 0xFF010203u);

    struct css_scale_context scale = { .dpi_scale = 1.0f, .cell_w = 8,
                                       .cell_h = 16 };
    assert(ui_css_border_edge(&st, &scale, 0) == 2);
    assert(ui_css_border_edge(&st, &scale, 1) == 1);
    assert(ui_css_border_edge(&st, &scale, 2) == 1);
    assert(ui_css_border_edge(&st, &scale, 3) == 4);
    assert(ui_css_font_size(&st, &scale, 12) == 14.0f);
    assert(ui_css_line_height(&st, &scale, 16) == 21.0f);
    assert(ui_css_font_weight(&st, 400) == 600);
    assert(ui_css_font_style(&st, CSS_FONT_NORMAL) == CSS_FONT_ITALIC);
    assert(ui_css_overflow(&st, CSS_OVERFLOW_VISIBLE) == CSS_OVERFLOW_HIDDEN);
    assert(ui_css_position(&st, CSS_POSITION_STATIC) == CSS_POSITION_RELATIVE);
    assert(ui_css_inset(&st, 0, &scale, 0) == 2);
    assert(ui_css_z_index(&st, 0) == 7);

    css_style_resolve(&sheet, "alpha", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(st.has_text_color && st.text_color == 0x800A141Eu);

    /* Every unset property keeps the C-side fallback. */
    css_style_resolve(&sheet, "plain", NULL, NULL, CSS_PSEUDO_NONE, &st);
    assert(ui_css_font_size(&st, &scale, 12) == 12.0f);
    assert(ui_css_line_height(&st, &scale, 16) == 16.0f);
    assert(ui_css_font_weight(&st, 400) == 400);
    assert(ui_css_font_style(&st, CSS_FONT_NORMAL) == CSS_FONT_NORMAL);
    assert(ui_css_overflow(&st, CSS_OVERFLOW_VISIBLE) == CSS_OVERFLOW_VISIBLE);
    assert(ui_css_position(&st, CSS_POSITION_STATIC) == CSS_POSITION_STATIC);
    assert(ui_css_inset(&st, 0, &scale, -1) == -1);
    assert(ui_css_z_index(&st, 0) == 0);
    assert(ui_css_border_edge(&st, &scale, 0) == 0);
    css_sheet_clear(&sheet);
}

int main(void)
{
    test_declaration_diagnostics();
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
    test_property_spellings_share_a_field();
    test_root_variables_and_var_expansion();
    test_var_diagnostics_and_guards();
    test_var_edge_cases();
    test_cyclic_variable_diagnostic();
    test_variable_defined_after_its_use();
    test_border_shorthand();
    extended_properties_test();
    test_appended_sheet_holds_app_rules();
    printf("PASS: CSS parser tests\n");
    return 0;
}
