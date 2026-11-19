#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"
#include "ui/debug.h"

/*
 * An update of a view fails when memory runs out. The test fails each
 * allocation of one update in turn. A failed update must leave each kept
 * widget with its old style identity and its old debug key.
 */

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif

static _Atomic unsigned long alloc_count;
static _Atomic unsigned long fail_on;
static _Atomic int armed;

#ifndef SANITIZED

static void *(*real_malloc)(size_t);
static void *(*real_realloc)(void *, size_t);

static bool consume_alloc(void)
{
    if (!atomic_load_explicit(&armed, memory_order_relaxed))
        return false;
    unsigned long n =
        atomic_fetch_add_explicit(&alloc_count, 1, memory_order_relaxed) + 1;
    return atomic_load_explicit(&fail_on, memory_order_relaxed) == n;
}

void *malloc(size_t n)
{
    if (!real_malloc)
        real_malloc = dlsym(RTLD_NEXT, "malloc");
    if (consume_alloc()) {
        errno = ENOMEM;
        return NULL;
    }
    return real_malloc(n);
}

/* calloc goes through malloc, so dlsym needs no calloc of its own. */
void *calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!real_realloc)
        real_realloc = dlsym(RTLD_NEXT, "realloc");
    if (consume_alloc()) {
        errno = ENOMEM;
        return NULL;
    }
    return real_realloc(p, n);
}

#endif

static void arm(unsigned long k)
{
    atomic_store_explicit(&alloc_count, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_on, k, memory_order_relaxed);
    atomic_store_explicit(&armed, 1, memory_order_relaxed);
}

static unsigned long disarm(void)
{
    atomic_store_explicit(&armed, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_on, 0, memory_order_relaxed);
    return atomic_load_explicit(&alloc_count, memory_order_relaxed);
}

static const struct ui_list_item ITEMS[] = {
    { .label = "One", .action = 1 },
    { .label = "Two", .action = 2 },
};

struct fixture {
    struct ui_node_desc kids[3];
    struct ui_node_desc root;
    struct ui_node_desc next_kids[3];
    struct ui_node_desc next;
};

/* The list comes first, so the update sets its class before the text of the
 * label and the button can fail. */
static void fixture_init(struct fixture *f)
{
    f->kids[0] = (struct ui_node_desc){
        .kind = UI_NODE_LIST, .key = "choices", .class_name = "list-a",
        .props.list = { .items = ITEMS, .count = 2 },
    };
    f->kids[1] = (struct ui_node_desc){
        .kind = UI_NODE_LABEL, .key = "status", .class_name = "label-a",
        .props.label = { .text = "Idle" },
    };
    f->kids[2] = (struct ui_node_desc){
        .kind = UI_NODE_BUTTON, .key = "go", .class_name = "button-a",
        .props.button = { .text = "Go" },
    };
    f->root = (struct ui_node_desc){
        .kind = UI_NODE_COLUMN, .key = "root", .class_name = "root-a",
        .children = f->kids, .child_count = 3,
    };
    memcpy(f->next_kids, f->kids, sizeof f->kids);
    f->next_kids[0].class_name = "list-b";
    f->next_kids[1].class_name = "label-b";
    f->next_kids[1].props.label.text = "Busy with a longer text";
    f->next_kids[2].class_name = "button-b";
    f->next_kids[2].props.button.text = "Run the command";
    f->next = f->root;
    f->next.class_name = "root-b";
    f->next.children = f->next_kids;
}

/* True when each kept widget has its old style identity. */
static bool identity_is_old(struct ui_view *view)
{
    const struct ui_widget *root = ui_view_root(view);
    return ui_debug_find(root, ".root-a") == root &&
           ui_debug_find(root, ".list-a menu-list") == ui_view_widget(view, "choices") &&
           ui_debug_find(root, ".label-a") == ui_view_widget(view, "status") &&
           ui_debug_find(root, ".button-a") == ui_view_widget(view, "go") &&
           !ui_debug_find(root, ".list-b menu-list") && !ui_debug_find(root, ".label-b") &&
           !ui_debug_find(root, ".button-b") && !ui_debug_find(root, ".root-b") &&
           ui_debug_find(root, "@status") == ui_view_widget(view, "status");
}

static void failed_update_restores_style_identity_test(void)
{
    struct fixture f;
    fixture_init(&f);
    struct ui_view *view = ui_view_mount(&f.root, NULL);
    assert(view && identity_is_old(view));
    arm(0);
    bool ok = ui_view_update(view, &f.next);
    unsigned long total = disarm();
    assert(ok);
    const struct ui_widget *root = ui_view_root(view);
    assert(ui_debug_find(root, ".list-b menu-list") == ui_view_widget(view, "choices"));
    ui_view_destroy(view);

#ifndef SANITIZED
    assert(total > 0);
    int failures = 0;
    for (unsigned long k = 1; k <= total; k++) {
        fixture_init(&f);
        view = ui_view_mount(&f.root, NULL);
        assert(view);
        arm(k);
        ok = ui_view_update(view, &f.next);
        disarm();
        if (!ok) {
            failures++;
            assert(identity_is_old(view));
        }
        /* The view works after a failed update. */
        assert(ui_view_update(view, &f.next));
        ui_view_destroy(view);
    }
    assert(failures > 0);
#else
    (void)total;
#endif
}

int main(void)
{
    failed_update_restores_style_identity_test();
    return 0;
}
