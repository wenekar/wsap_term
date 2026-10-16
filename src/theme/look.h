#ifndef LOOK_LOOK_H
#define LOOK_LOOK_H

#include <stdbool.h>

struct css_sheet;
struct wsap_vtcore_theme;

/* A theme is NAME.css (chrome) and/or NAME-term.conf (palette, plus an
 * optional 'label = Display Name'), found in the user theme directory
 * first, then the system ones. */
struct look_theme {
    char *name;
    char *label;
    char *css_path;
    char *conf_path;
};

/* The app's appearance: a committed theme and an optional preview over
 * it. Every theme is built over the same base palette, so a theme without
 * a palette of its own shows the base. The listener runs whenever the
 * active palette changes; the owner then gives it to every terminal. */
struct look;

/* theme names a theme, by name or label, or is a path to a .css file;
 * NULL or empty keeps the base palette and the built-in sheet. */
struct look *look_create(const struct wsap_vtcore_theme *base, const char *theme);
void look_destroy(struct look *look);

/* Runs after the active appearance changes. */
void look_set_listener(struct look *look, void (*changed)(void *ud), void *ud);

const struct css_sheet *look_sheet(const struct look *look);
/* The terminal colors of the active appearance. */
const struct wsap_vtcore_theme *look_palette(const struct look *look);

/* The first call scans the theme directories. */
int look_theme_count(struct look *look);
const struct look_theme *look_theme_at(struct look *look, int idx);
/* Cancels any preview and scans again. Built themes stay cached until
 * their files change. */
int look_rescan(struct look *look);

/* Shows theme idx until commit or cancel. */
bool look_preview(struct look *look, int idx);
/* The previewed theme's index, or -1. */
int look_previewed(const struct look *look);
void look_commit(struct look *look);
void look_cancel(struct look *look);
/* Rebuilds the committed appearance from settings. Keeps the listener. */
bool look_apply(struct look *look, const struct wsap_vtcore_theme *base, const char *theme);
/* Empty when the committed look did not come from the theme list. */
const char *look_committed_name(const struct look *look);
/* Whether theme idx of the list is the committed look. */
bool look_is_committed(struct look *look, int idx);

#endif
