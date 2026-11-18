#ifndef EDITOR_ANNOTATIONS_H
#define EDITOR_ANNOTATIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A byte range of the text. Folding uses the line range of the file. */
struct editor_range {
    uint64_t start;
    uint64_t length;
};

/* How bad a diagnostic is. The value orders the severities. */
enum editor_severity {
    EDITOR_SEVERITY_ERROR = 0,
    EDITOR_SEVERITY_WARNING,
    EDITOR_SEVERITY_INFO,
    EDITOR_SEVERITY_HINT,
};

/* One problem of the text, such as a compiler error. */
struct editor_diagnostic {
    struct editor_range range;
    enum editor_severity severity;
    /* The source of the message, such as "clang". It can be NULL. */
    const char *source;
    const char *message;
};

/* A set of diagnostics. The set copies the strings that it is given. */
struct editor_diagnostics;

struct editor_diagnostics *editor_diagnostics_create(void);
void editor_diagnostics_destroy(struct editor_diagnostics *diagnostics);

/* Removes all diagnostics. */
void editor_diagnostics_clear(struct editor_diagnostics *diagnostics);
/* Adds one diagnostic. Returns false on failure. */
bool editor_diagnostics_add(struct editor_diagnostics *diagnostics,
                            const struct editor_diagnostic *diagnostic);
size_t editor_diagnostics_count(const struct editor_diagnostics *diagnostics);
/* Reads one diagnostic. Returns false past the last one. */
bool editor_diagnostics_get(const struct editor_diagnostics *diagnostics, size_t index,
                            struct editor_diagnostic *diagnostic);
/* Finds the first diagnostic that holds `offset`. Returns false when none
 * does. */
bool editor_diagnostics_at(const struct editor_diagnostics *diagnostics, uint64_t offset,
                           struct editor_diagnostic *diagnostic);
/* The number of diagnostics of one severity. */
size_t editor_diagnostics_count_severity(const struct editor_diagnostics *diagnostics,
                                         enum editor_severity severity);

/* One folded region. The first line stays and the lines inside it hide. */
struct editor_fold_range {
    uint32_t start_line;
    uint32_t end_line;
    /* The text that gives the region a name, such as a function name. */
    const char *label;
};

struct editor_folds;

struct editor_folds *editor_folds_create(void);
void editor_folds_destroy(struct editor_folds *folds);
void editor_folds_clear(struct editor_folds *folds);
/* Adds one fold range. `start_line` must not be past `end_line`. */
bool editor_folds_add(struct editor_folds *folds, uint32_t start_line,
                      uint32_t end_line, const char *label);
size_t editor_folds_count(const struct editor_folds *folds);
bool editor_folds_get(const struct editor_folds *folds, size_t index,
                      struct editor_fold_range *range);
/* True when the line is inside a fold range and is not its first line. */
bool editor_folds_hidden(const struct editor_folds *folds, uint32_t line);
/* Sorts the ranges by start line and drops a range that another range holds. */
void editor_folds_normalize(struct editor_folds *folds);

#endif
