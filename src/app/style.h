#ifndef APP_STYLE_H
#define APP_STYLE_H

struct css_sheet;

/* Returns the sheet of assets/ui/app.css, layered over the UI core rules.
   The sheet lives for the process. */
const struct css_sheet *app_style_sheet(void);

#endif
