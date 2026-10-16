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

struct look *look_create(const struct wsap_vtcore_theme *base, const char *theme);
void look_destroy(struct look *look);

void look_set_listener(struct look *look, void (*changed)(void *ud), void *ud);

const struct css_sheet *look_sheet(const struct look *look);
const struct wsap_vtcore_theme *look_palette(const struct look *look);

int look_theme_count(struct look *look);
const struct look_theme *look_theme_at(struct look *look, int idx);
int look_rescan(struct look *look);

bool look_preview(struct look *look, int idx);
int look_previewed(const struct look *look);
void look_commit(struct look *look);
void look_cancel(struct look *look);
bool look_apply(struct look *look, const struct wsap_vtcore_theme *base, const char *theme);
const char *look_committed_name(const struct look *look);
bool look_is_committed(struct look *look, int idx);

#endif
