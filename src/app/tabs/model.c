#include <stdlib.h>
#include <string.h>

#include "app/tabs/model.h"
#include "terminal/terminal.h"

/* The index of the tab with this ID, or -1. A new tab goes to the end with
 * a larger ID, and a removal keeps the order. The table is thus in ID
 * order. */
static int index_of(const struct app_tab_model *m, app_tab_id id)
{
    int lo = 0, hi = m ? m->count - 1 : -1;
    while (id && lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        app_tab_id at = m->tabs[mid].id;
        if (at == id)
            return mid;
        if (at < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

app_tab_id app_tab_model_add(struct app_tab_model *m, struct terminal *content)
{
    if (!m || !content || m->last_id == UINT32_MAX) {
        terminal_destroy(content);
        return 0;
    }
    if (m->count == m->cap) {
        int cap = m->cap ? m->cap * 2 : 8;
        struct app_tab *tabs = realloc(m->tabs, (size_t)cap * sizeof(*tabs));
        if (!tabs) {
            terminal_destroy(content);
            return 0;
        }
        m->tabs = tabs;
        m->cap = cap;
    }
    app_tab_id id = ++m->last_id;
    m->tabs[m->count++] = (struct app_tab){ .id = id, .content = content };
    return id;
}

struct app_tab *app_tab_model_get(const struct app_tab_model *m, app_tab_id id)
{
    int i = index_of(m, id);
    return i < 0 ? NULL : &m->tabs[i];
}

struct app_tab *app_tab_model_find(const struct app_tab_model *m,
                                   const struct terminal *content)
{
    for (int i = 0; m && content && i < m->count; i++)
        if (m->tabs[i].content == content)
            return &m->tabs[i];
    return NULL;
}

void app_tab_model_destroy(struct app_tab_model *m, app_tab_id id)
{
    int i = index_of(m, id);
    if (i < 0)
        return;
    struct app_tab gone = m->tabs[i];
    memmove(&m->tabs[i], &m->tabs[i + 1],
            (size_t)(m->count - i - 1) * sizeof(*m->tabs));
    m->count--;
    /* Remove the entry first, so a report during the destruction finds no
     * tab. */
    free(gone.label);
    terminal_destroy(gone.content);
}

void app_tab_model_fini(struct app_tab_model *m)
{
    if (!m)
        return;
    /* A drag that did not end leaves its tab here. It goes with the rest. */
    while (m->count > 0)
        app_tab_model_destroy(m, m->tabs[m->count - 1].id);
    free(m->tabs);
    /* Keep the ID counter, so the table never gives an ID twice. */
    *m = (struct app_tab_model){ .last_id = m->last_id };
}
