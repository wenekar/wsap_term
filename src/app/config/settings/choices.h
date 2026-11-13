#ifndef APP_CONFIG_SETTINGS_CHOICES_H
#define APP_CONFIG_SETTINGS_CHOICES_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/widgets/dropdown.h"

struct look;

/*
 * A source tells the builder where the choice list of a setting comes from.
 * SETTINGS_CHOICE_SPEC reads the choices of the option key from the config
 * spec. SETTINGS_CHOICE_HINTING does the same, but drops the values that the
 * current antialiasing mode does not allow.
 */
enum settings_choice_source {
    SETTINGS_CHOICE_SPEC = 0,
    SETTINGS_CHOICE_FONT,
    SETTINGS_CHOICE_LOCALE,
    SETTINGS_CHOICE_THEME,
    SETTINGS_CHOICE_HINTING,
};

/*
 * A choice list owns the strings that a provider returned and the heading that
 * it synthesizes. Spec choice labels, theme labels, and literals stay borrowed
 * until ui_dropdown_set_items copies them.
 * The zero value is a valid empty list. settings_choices_destroy restores it.
 */
struct settings_choices {
    struct ui_dropdown_item *items;
    size_t count;
    char **strings;
    size_t string_count;
    char *heading;
};

/* On success the new list replaces the old list of *choices. A failure leaves
 * *choices unchanged. */
bool settings_choices_build(struct settings_choices *choices,
                            enum settings_choice_source source,
                            const char *key, struct look *look,
                            const char *antialias);
void settings_choices_destroy(struct settings_choices *choices);

#endif
