#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/ui.h"

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

struct oom_fixture {
    struct ui_surface surface;
    struct ui_input in;
    char copied[128];
};

static void keep_copied(void *userdata, const char *text)
{
    struct oom_fixture *f = userdata;
    size_t len = strlen(text);
    assert(len < sizeof(f->copied));
    memcpy(f->copied, text, len + 1);
}

static void setup_history(struct oom_fixture *f)
{
    *f = (struct oom_fixture){0};
    static const struct ui_surface_host host = { .copy_text = keep_copied };
    ui_surface_init(&f->surface, &host, f);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_input_init(&f->in);
    assert(ui_input_set_text(&f->in, "aa bb cc"));
    assert(ui_surface_set_root(&f->surface, ui_input_widget(&f->in)));
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 200, .y = 24 });
    ui_surface_focus(&f->surface, ui_input_widget(&f->in));

    assert(ui_input_insert_text(&f->in, " 1"));
    assert(ui_input_insert_text(&f->in, " 2"));
    assert(ui_input_command(&f->in, UI_EDIT_UNDO));
    assert(!strcmp(ui_input_text(&f->in), "aa bb cc 1"));
    assert(ui_input_can_command(&f->in, UI_EDIT_UNDO));
    assert(ui_input_can_command(&f->in, UI_EDIT_REDO));
}

static void teardown(struct oom_fixture *f)
{
    ui_surface_destroy(&f->surface);
    ui_widget_destroy(ui_input_widget(&f->in));
}

static unsigned long arm_and_measure(void)
{
    atomic_store_explicit(&alloc_count, 0, memory_order_relaxed);
    atomic_store_explicit(&fail_on, 0, memory_order_relaxed);
    atomic_store_explicit(&armed, 1, memory_order_relaxed);
    return 0;
}

static void arm_fail_at(unsigned long k)
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

static void verify_dropped_history_sweep(
    void (*prepare)(struct oom_fixture *f),
    bool (*run)(struct oom_fixture *f),
    const char *before_text,
    const char *after_text)
{
    struct oom_fixture base;
    prepare(&base);
    arm_and_measure();
    bool base_ok = run(&base);
    unsigned long total = disarm();
    assert(base_ok);
    assert(!strcmp(ui_input_text(&base.in), after_text));
    teardown(&base);

#ifndef SANITIZED
    assert(total > 0);
    bool saw_dropped = false;
    for (unsigned long k = 1; k <= total; k++) {
        struct oom_fixture f;
        prepare(&f);
        arm_fail_at(k);
        bool ok = run(&f);
        disarm();

        const char *got = ui_input_text(&f.in);
        if (!strcmp(got, before_text)) {
            assert(ui_input_can_command(&f.in, UI_EDIT_UNDO));
            assert(ui_input_can_command(&f.in, UI_EDIT_REDO));
        } else {
            assert(ok);
            assert(!strcmp(got, after_text));
            assert(!ui_input_can_command(&f.in, UI_EDIT_UNDO));
            assert(!ui_input_can_command(&f.in, UI_EDIT_REDO));
            saw_dropped = true;
            /* Edits after an OOM history drop can record history again. */
            assert(ui_input_insert_text(&f.in, "+"));
            assert(ui_input_can_command(&f.in, UI_EDIT_UNDO));
        }
        teardown(&f);
    }
    assert(saw_dropped);
#else
    (void)total;
    (void)before_text;
#endif
}

static bool op_insert_bang(struct oom_fixture *f)
{
    return ui_input_insert_text(&f->in, "!");
}

static bool op_delete_back(struct oom_fixture *f)
{
    return ui_input_command(&f->in, UI_EDIT_DELETE_BACK);
}

static bool op_clear(struct oom_fixture *f)
{
    ui_input_clear(&f->in);
    return true;
}

static bool op_kill_word(struct oom_fixture *f)
{
    return ui_input_command(&f->in, UI_EDIT_KILL_WORD);
}

static void prepare_kill_test(struct oom_fixture *f)
{
    setup_history(f);
    assert(!strcmp(ui_input_text(&f->in), "aa bb cc 1"));
    assert(ui_input_command(&f->in, UI_EDIT_DELETE_BACK));
    assert(ui_input_command(&f->in, UI_EDIT_DELETE_BACK));
    assert(ui_input_insert_text(&f->in, " dd"));
    assert(ui_input_command(&f->in, UI_EDIT_UNDO));
    assert(!strcmp(ui_input_text(&f->in), "aa bb cc"));
}

static void prepare_transpose_test(struct oom_fixture *f)
{
    *f = (struct oom_fixture){0};
    ui_surface_init(&f->surface, &(const struct ui_surface_host){0}, NULL);
    ui_input_init(&f->in);
    assert(ui_input_set_text(&f->in, "a"));
    assert(ui_surface_set_root(&f->surface, ui_input_widget(&f->in)));
    assert(ui_input_insert_text(&f->in, "b\xc3\xa9"));
    assert(ui_input_insert_text(&f->in, "c"));
    assert(ui_input_command(&f->in, UI_EDIT_UNDO));
    assert(!strcmp(ui_input_text(&f->in), "ab\xc3\xa9"));
    assert(ui_input_can_command(&f->in, UI_EDIT_UNDO));
    assert(ui_input_can_command(&f->in, UI_EDIT_REDO));
}

static bool op_transpose(struct oom_fixture *f)
{
    return ui_input_command(&f->in, UI_EDIT_TRANSPOSE);
}

static void prepare_cut_test(struct oom_fixture *f)
{
    setup_history(f);
    ui_input_select_all(&f->in);
}

static bool op_cut(struct oom_fixture *f)
{
    return ui_input_command(&f->in, UI_EDIT_CUT);
}

static void undo_redo_oom_keeps_history_test(void)
{
    struct oom_fixture f;
    setup_history(&f);
    arm_fail_at(1);
    bool undo_ok = ui_input_command(&f.in, UI_EDIT_UNDO);
    disarm();

#ifndef SANITIZED
    assert(!undo_ok);
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc 1"));
    assert(ui_input_can_command(&f.in, UI_EDIT_UNDO));
    assert(ui_input_can_command(&f.in, UI_EDIT_REDO));

    assert(ui_input_command(&f.in, UI_EDIT_UNDO));
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc"));

    arm_fail_at(1);
    bool redo_ok = ui_input_command(&f.in, UI_EDIT_REDO);
    disarm();

    assert(!redo_ok);
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc"));
    assert(ui_input_can_command(&f.in, UI_EDIT_REDO));

    assert(ui_input_command(&f.in, UI_EDIT_REDO));
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc 1"));
#else
    (void)undo_ok;
#endif
    teardown(&f);
}

static void kill_oom_keeps_kill_buffer_test(void)
{
    struct oom_fixture f;
    setup_history(&f);
    assert(ui_input_command(&f.in, UI_EDIT_KILL_WORD));
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc "));

    arm_fail_at(1);
    assert(ui_input_command(&f.in, UI_EDIT_KILL_WORD));
    disarm();

#ifndef SANITIZED
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc "));
    assert(ui_input_command(&f.in, UI_EDIT_YANK));
    assert(!strcmp(ui_input_text(&f.in), "aa bb cc 1"));
#endif
    teardown(&f);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef SANITIZED
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    assert(real_malloc && real_realloc);
#endif

    verify_dropped_history_sweep(setup_history, op_insert_bang,
                                 "aa bb cc 1", "aa bb cc 1!");
    verify_dropped_history_sweep(setup_history, op_delete_back,
                                 "aa bb cc 1", "aa bb cc ");
    verify_dropped_history_sweep(setup_history, op_clear,
                                 "aa bb cc 1", "");
    verify_dropped_history_sweep(prepare_kill_test, op_kill_word,
                                 "aa bb cc", "aa bb ");
    verify_dropped_history_sweep(prepare_transpose_test, op_transpose,
                                 "ab\xc3\xa9", "a\xc3\xa9" "b");
    verify_dropped_history_sweep(prepare_cut_test, op_cut,
                                 "aa bb cc 1", "");
    undo_redo_oom_keeps_history_test();
    kill_oom_keeps_kill_buffer_test();

#ifdef SANITIZED
    puts("PASS: input OOM tests (fault injection skipped under sanitizer)");
#else
    puts("PASS: input OOM tests");
#endif
    return 0;
}
