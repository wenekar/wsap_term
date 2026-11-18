#ifndef EDITOR_JOURNAL_H
#define EDITOR_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "editor/file.h"

/* The state that a recovery journal starts from. */
struct editor_session {
    /* The document file, or NULL for an untitled document. */
    char *path;
    /* The state of the file when the session started. */
    struct editor_file_state base;
    /* The hash of the base text. */
    uint64_t base_hash;
};

struct editor_journal;

/* Creates the journal and writes the session record. */
struct editor_journal *editor_journal_create(const char *path,
                                             const struct editor_session *session);
/* Appends one edit record: `remove` bytes at `offset` became `insert`. */
bool editor_journal_edit(struct editor_journal *journal, uint64_t offset,
                         uint64_t remove, const char *insert,
                         size_t insert_length);
/* Appends the cursor position. */
bool editor_journal_cursor(struct editor_journal *journal, uint64_t caret,
                           uint64_t anchor);
/* Flushes the records to disk. A process crash keeps them without this call. */
bool editor_journal_sync(struct editor_journal *journal);
/* Closes the journal. The file stays on disk unless `remove` is true. */
void editor_journal_destroy(struct editor_journal *journal, bool remove);

enum editor_journal_result {
    EDITOR_JOURNAL_OK = 0,
    /* No journal, or no complete session record. */
    EDITOR_JOURNAL_NONE,
    /* The file that the session recorded is gone. */
    EDITOR_JOURNAL_BASE_MISSING,
    /* The file that the session recorded changed. */
    EDITOR_JOURNAL_BASE_CHANGED,
    /* A memory or a system call failed. */
    EDITOR_JOURNAL_ERROR,
};

struct editor_recovery {
    /* The document file, or NULL for an untitled document. */
    char *path;
    struct editor_file_state base;
    uint64_t base_hash;
    uint64_t caret;
    uint64_t anchor;
    /* The recovered text. It is NUL terminated. */
    char *text;
    size_t length;
    /* The number of edit records that it applied. */
    uint32_t edits;
    /* The last record was incomplete or corrupt. The text holds the complete
     * records before it. */
    bool truncated;
};

/* Replays the journal. It returns OK when the recovered text is complete. */
enum editor_journal_result editor_journal_replay(const char *path,
                                                 struct editor_recovery *recovery);
void editor_recovery_destroy(struct editor_recovery *recovery);

#endif
