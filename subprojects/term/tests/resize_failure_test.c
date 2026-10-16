#define _GNU_SOURCE
#include "term_api.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || \
      __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif

#ifndef SANITIZED
static bool fail_next_calloc;
static bool fail_reallocs;

void *calloc(size_t n, size_t size)
{
    static void *(*real_calloc)(size_t, size_t);
    if (!real_calloc)
        real_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    if (fail_next_calloc) {
        fail_next_calloc = false;
        return NULL;
    }
    return real_calloc(n, size);
}

void *realloc(void *p, size_t size)
{
    static void *(*real_realloc)(void *, size_t);
    if (!real_realloc)
        real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    if (fail_reallocs)
        return NULL;
    return real_realloc(p, size);
}
#endif

int main(void)
{
#ifdef SANITIZED
    puts("resize failure test skipped under sanitizer");
    return 0;
#else
    struct wsap_vtcore *term = tt_new(10, 2);
    assert(term);
    tt_write(term, "abc", 3);

    fail_next_calloc = true;
    bool resized = tt_resize(term, 20, 2);
    assert(!fail_next_calloc);

    assert(!resized);
    assert(tt_cols(term) == 10);
    assert(tt_rows(term) == 2);
    assert(tt_cell(term, 0, 0)->text == 'a');
    assert(tt_cell(term, 2, 0)->text == 'c');

    tt_write(term, "\r12345678901", 12);
    assert(tt_cursor_x(term) == 1);
    assert(tt_cursor_y(term) == 1);
    assert(tt_cell(term, 0, 1)->text == '1');

    resized = tt_resize(term, 20, 2);
    assert(resized);
    assert(tt_cols(term) == 20);
    assert(tt_rows(term) == 2);
    assert(tt_cell(term, 0, 0)->text == '1');
    wsap_vtcore_free(term);

    term = tt_new(10, 3);
    assert(term);
    const char margins[] = "\033[?69h\033[1;5s\033[H";
    tt_write(term, margins, sizeof(margins) - 1);
    fail_next_calloc = true;
    resized = tt_resize(term, 20, 3);
    assert(!fail_next_calloc);

    assert(!resized);
    assert(tt_cols(term) == 10);
    tt_write(term, "abcdefg", 7);
    assert(tt_cell(term, 4, 0)->text == 'e');
    assert(tt_cell(term, 5, 0)->text != 'f');
    assert(tt_cell(term, 0, 1)->text == 'f');
    wsap_vtcore_free(term);

    term = tt_new(10, 2);
    assert(term);
    tt_write(term, "normal", 6);
    fail_next_calloc = true;
    tt_write(term, "\033[?1049h", 8);
    assert(!fail_next_calloc);

    assert(!wsap_vtcore_modes(term).alternate);
    assert(tt_cell(term, 0, 0)->text == 'n');
    assert(tt_cell(term, 5, 0)->text == 'l');
    tt_write(term, "\033[?1049l", 8);
    assert(tt_cell(term, 0, 0)->text == 'n');
    wsap_vtcore_free(term);

    term = tt_new(20, 40);
    assert(term);
    for (int i = 0; i < 40; i++) {
        char line[32];
        int n = snprintf(line, sizeof line, "row %02d 0123456789\r\n", i);
        tt_write(term, line, (size_t)n);
    }
    resized = tt_resize(term, 21, 40);
    assert(resized);
    int history = tt_history(term);
    char *before = wsap_vtcore_history_text(term, 1000);
    assert(before);

    fail_reallocs = true;
    resized = tt_resize(term, 10, 40);
    fail_reallocs = false;
    assert(!resized);
    assert(tt_cols(term) == 21);
    assert(tt_rows(term) == 40);
    assert(tt_history(term) == history);
    char *after = wsap_vtcore_history_text(term, 1000);
    assert(after && !strcmp(before, after));
    free(before);
    free(after);

    resized = tt_resize(term, 10, 40);
    assert(resized);
    assert(tt_cols(term) == 10);
    wsap_vtcore_free(term);

    term = tt_new(8, 4);
    assert(term);
    const char four_rows[] = "A\r\nB\r\nC\r\nD";
    tt_write(term, four_rows, sizeof four_rows - 1);
    assert(tt_cursor_y(term) == 3);
    assert(tt_history(term) == 0);

    fail_next_calloc = true;
    resized = tt_resize(term, 8, 2);
    assert(!fail_next_calloc);
    assert(!resized);
    assert(tt_cols(term) == 8 && tt_rows(term) == 4);
    assert(tt_cell(term, 0, 0)->text == 'A');
    assert(tt_cell(term, 0, 1)->text == 'B');
    assert(tt_cell(term, 0, 2)->text == 'C');
    assert(tt_cell(term, 0, 3)->text == 'D');
    assert(tt_cursor_y(term) == 3);
    assert(tt_history(term) == 0);

    resized = tt_resize(term, 8, 2);
    assert(resized);
    assert(tt_cols(term) == 8 && tt_rows(term) == 2);
    assert(tt_cell(term, 0, 0)->text == 'C');
    assert(tt_cell(term, 0, 1)->text == 'D');
    assert(tt_cursor_y(term) == 1);
    assert(tt_history(term) == 2);
    wsap_vtcore_free(term);

    term = tt_new(8, 4);
    assert(term);
    tt_write(term, "\033[?1049h", 8);
    tt_write(term, four_rows, sizeof four_rows - 1);
    assert(wsap_vtcore_modes(term).alternate);
    assert(tt_cursor_y(term) == 3);
    fail_next_calloc = true;
    resized = tt_resize(term, 8, 2);
    assert(!fail_next_calloc);
    assert(!resized);
    assert(tt_rows(term) == 4 && tt_cursor_y(term) == 3);
    assert(tt_cell(term, 0, 0)->text == 'A');
    assert(tt_cell(term, 0, 3)->text == 'D');
    resized = tt_resize(term, 8, 2);
    assert(resized);
    assert(tt_rows(term) == 2 && tt_cursor_y(term) == 1);
    assert(tt_cell(term, 0, 0)->text == 'C');
    assert(tt_cell(term, 0, 1)->text == 'D');
    wsap_vtcore_free(term);

    puts("lazy screen allocation failures preserve observable terminal state");
    return 0;
#endif
}
