#include <stdio.h>

#include "app/style.h"
#include "app_css.h"
#include "ui/css.h"
#include "util/name.h"

static struct css_sheet app_sheet;
static bool app_sheet_ready;

const struct css_sheet *app_style_sheet(void)
{
    if (!app_sheet_ready) {
        css_init_default(&app_sheet);
        if (!css_parse(&app_sheet, kAppCss))
            fprintf(stderr, "[%s] cannot parse the app stylesheet\n", app_name());
        app_sheet_ready = true;
    }
    return &app_sheet;
}
