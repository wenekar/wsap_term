#ifndef APP_TABS_INTERNAL_H
#define APP_TABS_INTERNAL_H

#include "app/tabs/tabs.h"
#include "ui/widgets/palette.h"
#include "ui/widgets/tab_preview.h"
#include "ui/widgets/tabbar.h"
#include "ui/widget_impl.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

/* One bar entry. A live entry owns its content. A closing entry keeps the
 * content and its label until the app retires it at a safe loop boundary. */
struct app_tab {
    struct terminal *content;
    char *label;
    uint64_t retire_at_ms;
};

struct app_tabs {
    struct window *window;
    struct app_tabs_spec spec;

    struct ui_widget root;
    struct ui_tabbar tabbar;
    struct ui_tab_preview preview;
    struct ui_palette palette;

    struct app_tab *tabs;
    struct ui_tab_label *labels;
    /* The content that the root holds, and its widget. */
    struct terminal *mounted_content;
    struct ui_widget *mounted;
    struct ui_image preview_image;
    void *preview_state;

    int count;
    int cap;
    /* The index of the active live entry, or -1. */
    int active;
    int labels_cap;
};

struct app_tabs *app_tabs_of(const struct window *window);

extern const int app_tabs_kind;
extern const struct window_drag_target APP_TABS_DRAG_TARGET;

static inline bool app_tab_is_live(const struct app_tab *entry)
{
    return entry && entry->content != NULL && entry->retire_at_ms == 0;
}

struct terminal *app_tabs_active_entry(const struct app_tabs *tabs);
int app_tabs_live_count(const struct app_tabs *tabs);
int app_tabs_find(const struct app_tabs *tabs, const struct terminal *content);

/* Sends the labels and the selection of the entry array to the bar. */
void app_tabs_publish(struct app_tabs *tabs);
/* Shows the active content in the root widget. */
void app_tabs_mount_content(struct app_tabs *tabs);
void app_tabs_update_title(struct app_tabs *tabs);

void app_tabs_restyle_contents(struct app_tabs *tabs);
void app_tabs_commit_geometry(struct app_tabs *tabs);
/* Starts content in the given directory. The call borrows the path. */
bool app_tabs_new_in(struct window *window, const char *cwd);

void app_tabs_widgets_init(struct app_tabs *tabs);
void app_tabs_actions_init(struct app_tabs *tabs);
struct window_ui_spec app_tabs_ui_spec(struct app_tabs *tabs);

void app_tabs_preview_init(struct app_tabs *tabs);
void app_tabs_preview_hover(struct app_tabs *tabs, int index);
void app_tabs_preview_drag(struct app_tabs *tabs, const struct ui_action *action);
bool app_tabs_preview_capture(struct app_tabs *tabs, int index, bool dragging);

bool app_tabs_drag_begin(struct window *window, int index,
                         struct vector2i grab);
void app_tabs_drag(struct window *window, enum yasl_drop_action event,
                   struct vector2i pos, void *ud);
void app_tabs_drop_hover(struct window *window, bool over, void *ud);
void app_tabs_drag_forget(struct window *window);

#endif
