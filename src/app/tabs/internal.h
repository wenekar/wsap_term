#ifndef APP_TABS_INTERNAL_H
#define APP_TABS_INTERNAL_H

#include "app/tabs/tabs.h"
#include "ui/widgets/palette.h"
#include "ui/widgets/tab_preview.h"
#include "ui/widgets/tabbar.h"
#include "ui/widget_impl.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

/* The view of the tab model in one window. The model owns the tabs and
 * their content. The view holds the tab IDs in bar order and borrows the
 * content widgets. */
struct app_tabs {
    struct window *window;
    struct app_tabs_spec spec;

    struct ui_widget root;
    struct ui_tabbar tabbar;
    struct ui_tab_preview preview;
    struct ui_palette palette;

    /* The tab IDs in bar order. A closing tab keeps its ID here until the
     * app retires it at a safe loop boundary. */
    app_tab_id *ids;
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

/* The model tab at a bar index, or NULL. The pointer is valid until the
 * next model add or destroy. */
struct app_tab *app_tabs_entry(const struct app_tabs *tabs, int index);
/* The content at a bar index, or NULL. */
struct terminal *app_tabs_content(const struct app_tabs *tabs, int index);
struct terminal *app_tabs_active_entry(const struct app_tabs *tabs);
int app_tabs_live_count(const struct app_tabs *tabs);
/* The bar index of a tab, or -1. */
int app_tabs_index_of(const struct app_tabs *tabs, app_tab_id id);

/* Takes a live tab out of the window. The tab stays in the model, and no
 * window holds it. The caller then puts it in a window or destroys it.
 * Returns 0 on failure. */
app_tab_id app_tabs_unbind(struct window *window, int index);
/* Puts a tab that no window holds in the window, at `slot`. A tab whose
 * process ended, or a closing tab, stays out. On failure the caller still
 * holds the tab. */
bool app_tabs_bind(struct window *window, app_tab_id id, int slot);
/* Gives the tabs of a window back to the model when the window goes. The
 * model destroys them. The tab of a running drag stays in the model, and no
 * window holds it. */
void app_tabs_release(struct app_tabs *tabs);

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
/* Drops the references of the running drag to a window that goes. */
void app_tabs_drag_forget(struct app_tab_model *model, struct window *window);

#endif
