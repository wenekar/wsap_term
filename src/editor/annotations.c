#define _GNU_SOURCE

#include "editor/annotations.h"

#include <stdlib.h>
#include <string.h>

struct editor_diagnostics {
    struct editor_diagnostic *items;
    size_t count;
    size_t capacity;
};

static char *string_copy(const char *text)
{
    if (!text)
        return NULL;
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

struct editor_diagnostics *editor_diagnostics_create(void)
{
    return calloc(1, sizeof(struct editor_diagnostics));
}

static void diagnostic_free(struct editor_diagnostic *diagnostic)
{
    free((char *)diagnostic->source);
    free((char *)diagnostic->message);
    diagnostic->source = NULL;
    diagnostic->message = NULL;
}

void editor_diagnostics_clear(struct editor_diagnostics *diagnostics)
{
    if (!diagnostics)
        return;
    for (size_t i = 0; i < diagnostics->count; i++)
        diagnostic_free(&diagnostics->items[i]);
    diagnostics->count = 0;
}

void editor_diagnostics_destroy(struct editor_diagnostics *diagnostics)
{
    if (!diagnostics)
        return;
    editor_diagnostics_clear(diagnostics);
    free(diagnostics->items);
    free(diagnostics);
}

bool editor_diagnostics_add(struct editor_diagnostics *diagnostics,
                            const struct editor_diagnostic *diagnostic)
{
    if (!diagnostics || !diagnostic)
        return false;
    if (diagnostics->count == diagnostics->capacity) {
        size_t capacity = diagnostics->capacity ? diagnostics->capacity * 2 : 16;
        struct editor_diagnostic *grown =
            realloc(diagnostics->items, capacity * sizeof(*grown));
        if (!grown)
            return false;
        diagnostics->items = grown;
        diagnostics->capacity = capacity;
    }
    struct editor_diagnostic copy = *diagnostic;
    copy.source = string_copy(diagnostic->source);
    if (diagnostic->source && !copy.source)
        return false;
    copy.message = string_copy(diagnostic->message);
    if (diagnostic->message && !copy.message) {
        free((char *)copy.source);
        return false;
    }
    diagnostics->items[diagnostics->count++] = copy;
    return true;
}

size_t editor_diagnostics_count(const struct editor_diagnostics *diagnostics)
{
    return diagnostics ? diagnostics->count : 0;
}

bool editor_diagnostics_get(const struct editor_diagnostics *diagnostics, size_t index,
                            struct editor_diagnostic *diagnostic)
{
    if (!diagnostics || index >= diagnostics->count)
        return false;
    if (diagnostic)
        *diagnostic = diagnostics->items[index];
    return true;
}

bool editor_diagnostics_at(const struct editor_diagnostics *diagnostics, uint64_t offset,
                           struct editor_diagnostic *diagnostic)
{
    if (!diagnostics)
        return false;
    for (size_t i = 0; i < diagnostics->count; i++) {
        const struct editor_diagnostic *item = &diagnostics->items[i];
        if (offset >= item->range.start &&
            offset - item->range.start < item->range.length) {
            if (diagnostic)
                *diagnostic = *item;
            return true;
        }
    }
    return false;
}

size_t editor_diagnostics_count_severity(const struct editor_diagnostics *diagnostics,
                                         enum editor_severity severity)
{
    if (!diagnostics)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < diagnostics->count; i++) {
        if (diagnostics->items[i].severity == severity)
            count++;
    }
    return count;
}

struct editor_folds {
    struct editor_fold_range *items;
    size_t count;
    size_t capacity;
};

struct editor_folds *editor_folds_create(void)
{
    return calloc(1, sizeof(struct editor_folds));
}

void editor_folds_clear(struct editor_folds *folds)
{
    if (!folds)
        return;
    for (size_t i = 0; i < folds->count; i++)
        free((char *)folds->items[i].label);
    folds->count = 0;
}

void editor_folds_destroy(struct editor_folds *folds)
{
    if (!folds)
        return;
    editor_folds_clear(folds);
    free(folds->items);
    free(folds);
}

bool editor_folds_add(struct editor_folds *folds, uint32_t start_line,
                      uint32_t end_line, const char *label)
{
    if (!folds || start_line > end_line)
        return false;
    if (folds->count == folds->capacity) {
        size_t capacity = folds->capacity ? folds->capacity * 2 : 16;
        struct editor_fold_range *grown =
            realloc(folds->items, capacity * sizeof(*grown));
        if (!grown)
            return false;
        folds->items = grown;
        folds->capacity = capacity;
    }
    char *copy = string_copy(label);
    if (label && !copy)
        return false;
    folds->items[folds->count++] = (struct editor_fold_range){
        .start_line = start_line,
        .end_line = end_line,
        .label = copy,
    };
    return true;
}

size_t editor_folds_count(const struct editor_folds *folds)
{
    return folds ? folds->count : 0;
}

bool editor_folds_get(const struct editor_folds *folds, size_t index,
                      struct editor_fold_range *range)
{
    if (!folds || index >= folds->count)
        return false;
    if (range)
        *range = folds->items[index];
    return true;
}

bool editor_folds_hidden(const struct editor_folds *folds, uint32_t line)
{
    if (!folds)
        return false;
    for (size_t i = 0; i < folds->count; i++) {
        const struct editor_fold_range *range = &folds->items[i];
        if (line > range->start_line && line <= range->end_line)
            return true;
    }
    return false;
}

static int fold_compare(const void *a, const void *b)
{
    const struct editor_fold_range *left = a;
    const struct editor_fold_range *right = b;
    if (left->start_line != right->start_line)
        return left->start_line < right->start_line ? -1 : 1;
    if (left->end_line != right->end_line)
        return left->end_line > right->end_line ? -1 : 1;
    return 0;
}

void editor_folds_normalize(struct editor_folds *folds)
{
    if (!folds || folds->count < 2)
        return;
    qsort(folds->items, folds->count, sizeof(*folds->items), fold_compare);
    size_t kept = 0;
    for (size_t i = 0; i < folds->count; i++) {
        struct editor_fold_range *range = &folds->items[i];
        bool contained = false;
        for (size_t j = 0; j < kept; j++) {
            if (range->end_line <= folds->items[j].end_line) {
                contained = true;
                break;
            }
        }
        if (contained) {
            free((char *)range->label);
            continue;
        }
        folds->items[kept++] = *range;
    }
    folds->count = kept;
}
