#ifndef EDITOR_UNDO_H
#define EDITOR_UNDO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "editor/text.h"

struct editor_undo;

/* How an edit joins the undo history. */
enum editor_edit_kind {
    /* Text typed at the caret. Adjacent typing joins one undo group. */
    EDITOR_EDIT_TYPING = 0,
    /* One step of a delete run. Adjacent steps join one undo group. */
    EDITOR_EDIT_DELETE,
    /* A command. It starts a new undo group. */
    EDITOR_EDIT_COMMAND,
};

struct editor_undo *editor_undo_create(void);
void editor_undo_destroy(struct editor_undo *undo);
void editor_undo_clear(struct editor_undo *undo);

/* Replaces `remove` bytes at `offset` with `insert_length` bytes from `insert`
 * and records the edit in the history. The caller keeps the arguments valid
 * during the call only. */
bool editor_undo_replace(struct editor_undo *undo, struct editor_text *text,
                         enum editor_edit_kind kind, uint64_t offset,
                         uint64_t remove, const char *insert,
                         size_t insert_length);

bool editor_undo_undo(struct editor_undo *undo, struct editor_text *text);
bool editor_undo_redo(struct editor_undo *undo, struct editor_text *text);
/* Ends the current undo group. The next edit starts a new group. */
void editor_undo_break(struct editor_undo *undo);
bool editor_undo_can_undo(const struct editor_undo *undo);
bool editor_undo_can_redo(const struct editor_undo *undo);

/* Records that the text now matches the file. */
void editor_undo_mark_saved(struct editor_undo *undo);
/* Records that the text differs from the file. */
void editor_undo_mark_modified(struct editor_undo *undo);
bool editor_undo_modified(const struct editor_undo *undo);

#endif
