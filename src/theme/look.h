#ifndef LOOK_LOOK_H
#define LOOK_LOOK_H

#include <stdbool.h>

struct css_sheet;
struct wsap_vtcore_theme;

struct look_theme {
    char *name;
    char *label;
    char *css_path;
    char *conf_path;
};

struct look;

/*
 * Creates and initializes a theme manager.
 * Applies the initial `theme` name or file path over `base` colors.
 * Returns a pointer to the new instance, or NULL on allocation failure.
 */
struct look *look_create(const struct wsap_vtcore_theme *base, const char *theme);

/*
 * Frees a theme manager and its loaded appearances.
 */
void look_destroy(struct look *look);

/*
 * Sets a callback function invoked when active theme appearance changes.
 */
void look_set_listener(struct look *look, void (*changed)(void *ud), void *ud);

/*
 * Returns the CSS style sheet of the active theme appearance.
 */
const struct css_sheet *look_sheet(const struct look *look);

/*
 * Returns the terminal color palette of the active theme appearance.
 */
const struct wsap_vtcore_theme *look_palette(const struct look *look);

/*
 * Returns the number of discovered themes in the catalog.
 * Scans theme directories if not yet scanned.
 */
int look_theme_count(struct look *look);

/*
 * Finds theme metadata at catalog index `idx`.
 * Returns a pointer to theme info, or NULL if `idx` is out of bounds.
 */
const struct look_theme *look_theme_at(struct look *look, int idx);

/*
 * Rescans theme directories and refreshes catalog entries.
 * Cancels any active theme preview.
 * Returns the updated theme count.
 */
int look_rescan(struct look *look);

/*
 * Temporarily activates theme at catalog index `idx` for preview.
 * Returns true if preview activated, or false on error.
 */
bool look_preview(struct look *look, int idx);

/*
 * Returns the catalog index of the active preview theme, or -1 if none is active.
 */
int look_previewed(const struct look *look);

/*
 * Commits the active preview theme as the permanent appearance.
 */
void look_commit(struct look *look);

/*
 * Cancels the active preview theme and restores the committed appearance.
 */
void look_cancel(struct look *look);

/*
 * Loads and commits a theme by name or CSS file path.
 * Updates base palette colors and notifies listeners.
 * Returns true on success, or false if loading failed.
 */
bool look_apply(struct look *look, const struct wsap_vtcore_theme *base, const char *theme);

/*
 * Returns the name of the committed theme, or an empty string if unnamed.
 */
const char *look_committed_name(const struct look *look);

/*
 * Checks if the theme at index `idx` matches the committed theme name.
 * Returns true if names match, or false otherwise.
 */
bool look_is_committed(struct look *look, int idx);

#endif
