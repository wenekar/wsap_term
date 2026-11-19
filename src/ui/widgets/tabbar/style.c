#include "ui/widgets/tabbar/internal.h"

#include "ui/css.h"
#include "ui/widget_ops.h"

static struct tb_parts parts;

const struct tb_parts *tb_part(void)
{
    uint32_t epoch = ui_part_epoch();
    if (parts.epoch == epoch)
        return &parts;
    parts.epoch = epoch;
    parts.bar = ui_part_intern("tab-bar");
    parts.tab = ui_part_intern("tab");
    parts.tab_active = ui_part_intern("tab active");
    parts.tab_drop = ui_part_intern("tab drop-preview");
    parts.drop_mark = ui_part_intern("tab-drop-mark");
    parts.tab_close = ui_part_intern("tab-close");
    parts.tab_new = ui_part_intern("tab-new");
    parts.tab_sep = ui_part_intern("tab-separator");
    parts.tab_preview = ui_part_intern("tab-preview");
    return &parts;
}

struct css_style tb_style(const struct ui_tabbar *tb, ui_part_id part,
                          unsigned pseudo)
{
    return ui_widget_part(&tb->widget, part, pseudo);
}

int tb_css_height(const struct ui_tabbar *tb)
{
    if (!tb)
        return 0;
    struct css_style bar = tb_style(tb, tb_part()->bar, CSS_PSEUDO_NONE);
    return ui_ctx_size(tb_ctx(tb), bar.height, 0);
}
