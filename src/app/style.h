#ifndef APP_STYLE_H
#define APP_STYLE_H

struct css_sheet;

/* Returns the sheet of the app rules in assets/ui/app.css. The rules are
   layered over the UI core rules. The sheet lives for the process. */
const struct css_sheet *app_style_sheet(void);

#endif
