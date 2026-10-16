/*
 * The CSS index picks the rules that a lookup can match. Each case checks
 * the resolved style against fixed values. A lost rule or a rule applied
 * out of order changes the result.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#include "ui/css.h"

/* Far more rules than the lookup buffer of the index holds, so the
 * resolver takes its overflow path. */
#define WIDE_RULES 300
_Static_assert(WIDE_RULES % 6 == 0, "the cases below cycle through 2 and 3 rules");

static char wide_css[1 << 16];

static struct css_style resolve(const struct css_sheet *sheet, const char *cls,
                                const char *id, const char *tag,
                                unsigned pseudo, const char *path)
{
    struct css_style st;
    css_style_resolve_ex(sheet, cls, id, tag, pseudo, path, &st);
    return st;
}

/* The text color set by the winning rule, or 0 when no rule sets it. */
static uint32_t color(const struct css_sheet *sheet, const char *cls,
                      const char *id, const char *tag)
{
    struct css_style st = resolve(sheet, cls, id, tag, 0, NULL);
    return st.has_text_color ? st.text_color & 0xFFFFFFu : 0;
}

static bool px(struct css_value v, float val)
{
    return v.unit == CSS_UNIT_PX && v.val == val;
}

static void parse(struct css_sheet *sheet, const char *text)
{
    *sheet = (struct css_sheet){0};
    assert(css_parse(sheet, text));
}

static void tag_class_id_tiers_test(void)
{
    struct css_sheet s;
    parse(&s, "#main { color: #000003; } .a { color: #000002; } div { color: #000001; }");
    assert(color(&s, "a", "main", "div") == 3);
    assert(color(&s, "a", NULL, "div") == 2);
    assert(color(&s, NULL, NULL, "div") == 1);
    assert(color(&s, NULL, "main", NULL) == 3);
    assert(color(&s, "b", "other", "span") == 0);
    css_sheet_clear(&s);
}

static void class_rank_and_source_order_test(void)
{
    struct css_sheet s;
    parse(&s, ".a.b { color: #000002; } .a { color: #000001; }"
              ".c { color: #000003; } .b { gap: 4px; }");
    assert(color(&s, "a b", NULL, NULL) == 2);
    assert(color(&s, "b a", NULL, NULL) == 2);
    assert(color(&s, "a", NULL, NULL) == 1);
    assert(color(&s, "a a", NULL, NULL) == 1);
    /* Source order decides between equal ranks, not the order of the list. */
    assert(color(&s, "a c", NULL, NULL) == 3);
    assert(color(&s, "c a", NULL, NULL) == 3);
    struct css_style st = resolve(&s, "b", NULL, NULL, 0, NULL);
    assert(!st.has_text_color && px(st.gap, 4.0f));
    css_sheet_clear(&s);
}

static void groups_merge_in_apply_order_test(void)
{
    struct css_sheet s;
    parse(&s, ".b { color: #000001; } .a { color: #000002; gap: 5px; }"
              ".b { gap: 6px; } div { padding: 1px; } #x { margin: 2px; }");
    struct css_style st = resolve(&s, "a b", "x", "div", 0, NULL);
    assert(st.has_text_color && (st.text_color & 0xFFFFFFu) == 2);
    assert(px(st.gap, 6.0f));
    assert(px(st.padding.top, 1.0f));
    assert(px(st.margin.top, 2.0f));
    css_sheet_clear(&s);
}

/* A selector with an empty last compound has no key. Every lookup checks
 * it, in source order with the keyed rules. */
static void keyless_rules_test(void)
{
    struct css_sheet s;
    parse(&s, ". { color: #000009; gap: 9px; } .a { color: #000001; } # { margin: 4px; }");
    assert(color(&s, "a", NULL, NULL) == 1);
    assert(color(&s, "zzz", NULL, NULL) == 9);
    assert(color(&s, NULL, NULL, NULL) == 0);
    assert(px(resolve(&s, "a", NULL, NULL, 0, NULL).gap, 9.0f));
    assert(px(resolve(&s, NULL, "", NULL, 0, NULL).margin.top, 4.0f));
    css_sheet_clear(&s);

    parse(&s, ".a { color: #000001; } . { color: #000009; }");
    assert(color(&s, "a", NULL, NULL) == 9);
    css_sheet_clear(&s);
}

static void appended_sheets_test(void)
{
    struct css_sheet first, second, both = {0};
    parse(&first, ".a { color: #000001; }");
    parse(&second, ".a { color: #000002; } . { gap: 3px; }");
    assert(css_sheet_append(&both, &first));
    assert(css_sheet_append(&both, &second));
    struct css_style st = resolve(&both, "a", NULL, NULL, 0, NULL);
    assert(st.has_text_color && (st.text_color & 0xFFFFFFu) == 2);
    assert(px(st.gap, 3.0f));
    css_sheet_clear(&first);
    css_sheet_clear(&second);
    css_sheet_clear(&both);
}

static void append_rule(size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(wide_css + *len, sizeof(wide_css) - *len, fmt, ap);
    va_end(ap);
    assert(n > 0 && (size_t)n < sizeof(wide_css) - *len);
    *len += (size_t)n;
}

/* One class names more rules than the lookup buffer holds. The overflow
 * path must still visit the keyless, tag, and id groups, and apply all
 * rules in the same order as the normal path. */
static void overflow_one_group_test(void)
{
    size_t len = 0;
    append_rule(&len, ". { margin: %dpx; } div { padding: %dpx; }", 7, 3);
    append_rule(&len, "#w { border-width: %dpx; } .menu .k { border-radius: %dpx; }", 2, 6);
    for (int i = 0; i < WIDE_RULES; i++) {
        const char *sel = i % 3 == 0 ? ".k" : i % 3 == 1 ? ".k.j" : ".k:hover";
        append_rule(&len, "%s { gap: %dpx; color: #%06x; }", sel, i, i + 1);
    }
    append_rule(&len, ". { gap: %dpx; border-width: %dpx; }", 1000, 1);

    struct css_sheet s;
    parse(&s, wide_css);
    int last_k = WIDE_RULES - 3;
    int last_kj = WIDE_RULES - 2;
    int last_hover = WIDE_RULES - 1;

    struct css_style st = resolve(&s, "k", "w", "div", 0, NULL);
    assert(px(st.gap, 1000.0f));
    assert(st.has_text_color && (st.text_color & 0xFFFFFFu) == (uint32_t)last_k + 1);
    assert(px(st.margin.top, 7.0f));
    assert(px(st.padding.top, 3.0f));
    assert(px(st.border_width, 2.0f));
    assert(st.border_radius.unit != CSS_UNIT_PX);

    st = resolve(&s, "k j", NULL, NULL, 0, NULL);
    assert(px(st.gap, (float)last_kj));
    assert((st.text_color & 0xFFFFFFu) == (uint32_t)last_kj + 1);
    assert(px(st.border_width, 1.0f));

    st = resolve(&s, "j k", NULL, NULL, 0, NULL);
    assert(px(st.gap, (float)last_kj));

    st = resolve(&s, "k", NULL, NULL, CSS_PSEUDO_HOVER, NULL);
    assert(px(st.gap, 1000.0f));
    assert((st.text_color & 0xFFFFFFu) == (uint32_t)last_hover + 1);

    st = resolve(&s, "k", NULL, NULL, 0, "menu\n");
    assert(px(st.border_radius, 6.0f));

    st = resolve(&s, "j", NULL, NULL, 0, NULL);
    assert(px(st.gap, 1000.0f) && !st.has_text_color);
    css_sheet_clear(&s);
}

/* Two classes overflow the lookup buffer together. Their rules still
 * apply in source order. */
static void overflow_two_groups_test(void)
{
    size_t len = 0;
    for (int i = 0; i < WIDE_RULES; i++)
        append_rule(&len, "%s { gap: %dpx; }", i % 2 ? ".q" : ".p", i);

    struct css_sheet s;
    parse(&s, wide_css);
    assert(px(resolve(&s, "p q", NULL, NULL, 0, NULL).gap, (float)(WIDE_RULES - 1)));
    assert(px(resolve(&s, "q p", NULL, NULL, 0, NULL).gap, (float)(WIDE_RULES - 1)));
    assert(px(resolve(&s, "p", NULL, NULL, 0, NULL).gap, (float)(WIDE_RULES - 2)));
    css_sheet_clear(&s);
}

int main(void)
{
    tag_class_id_tiers_test();
    class_rank_and_source_order_test();
    groups_merge_in_apply_order_test();
    keyless_rules_test();
    appended_sheets_test();
    overflow_one_group_test();
    overflow_two_groups_test();
    puts("PASS: css index");
    return 0;
}
