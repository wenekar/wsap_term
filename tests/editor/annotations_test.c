#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "editor/annotations.h"

static void test_diagnostics(void)
{
    struct editor_diagnostics *diagnostics = editor_diagnostics_create();
    assert(diagnostics);
    assert(editor_diagnostics_count(diagnostics) == 0);

    char message[] = "unused variable";
    assert(editor_diagnostics_add(diagnostics,
                                  &(struct editor_diagnostic){
                                      .range = {10, 3},
                                      .severity = EDITOR_SEVERITY_WARNING,
                                      .source = "clang",
                                      .message = message,
                                  }));
    /* The set copies the message. */
    message[0] = 'X';
    assert(editor_diagnostics_add(diagnostics,
                                  &(struct editor_diagnostic){
                                      .range = {0, 5},
                                      .severity = EDITOR_SEVERITY_ERROR,
                                      .message = "syntax error",
                                  }));
    assert(editor_diagnostics_count(diagnostics) == 2);
    assert(editor_diagnostics_count_severity(diagnostics, EDITOR_SEVERITY_WARNING) == 1);
    assert(editor_diagnostics_count_severity(diagnostics, EDITOR_SEVERITY_ERROR) == 1);

    struct editor_diagnostic found;
    assert(editor_diagnostics_get(diagnostics, 0, &found));
    assert(strcmp(found.message, "unused variable") == 0);
    assert(strcmp(found.source, "clang") == 0);
    assert(!editor_diagnostics_get(diagnostics, 2, &found));

    /* A lookup by offset finds the range that holds it. */
    assert(editor_diagnostics_at(diagnostics, 11, &found));
    assert(found.severity == EDITOR_SEVERITY_WARNING);
    assert(!editor_diagnostics_at(diagnostics, 6, &found));

    editor_diagnostics_clear(diagnostics);
    assert(editor_diagnostics_count(diagnostics) == 0);
    editor_diagnostics_destroy(diagnostics);
}

static void test_folds(void)
{
    struct editor_folds *folds = editor_folds_create();
    assert(folds);
    assert(editor_folds_add(folds, 4, 9, "helper"));
    assert(editor_folds_add(folds, 12, 20, "main"));
    assert(editor_folds_add(folds, 9, 4, NULL) == false);
    assert(editor_folds_count(folds) == 2);

    /* The first line of a range stays. The lines inside hide. */
    assert(!editor_folds_hidden(folds, 4));
    assert(editor_folds_hidden(folds, 5));
    assert(editor_folds_hidden(folds, 9));
    assert(!editor_folds_hidden(folds, 10));

    struct editor_fold_range range;
    assert(editor_folds_get(folds, 1, &range));
    assert(range.start_line == 12 && range.end_line == 20);
    assert(strcmp(range.label, "main") == 0);
    assert(!editor_folds_get(folds, 2, &range));
    editor_folds_destroy(folds);
}

/* A range that another range holds is dropped. The order becomes the start
 * line. */
static void test_folds_normalize(void)
{
    struct editor_folds *folds = editor_folds_create();
    assert(folds);
    assert(editor_folds_add(folds, 10, 20, "outer"));
    assert(editor_folds_add(folds, 12, 14, "inner"));
    assert(editor_folds_add(folds, 1, 3, "first"));
    editor_folds_normalize(folds);
    assert(editor_folds_count(folds) == 2);
    struct editor_fold_range range;
    assert(editor_folds_get(folds, 0, &range));
    assert(strcmp(range.label, "first") == 0);
    assert(editor_folds_get(folds, 1, &range));
    assert(strcmp(range.label, "outer") == 0);
    editor_folds_destroy(folds);
}

int main(void)
{
    test_diagnostics();
    test_folds();
    test_folds_normalize();
    printf("editor annotations tests passed\n");
    return 0;
}
