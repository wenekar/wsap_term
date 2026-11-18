#define _GNU_SOURCE

#include "editor/document.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "editor/file.h"
#include "editor/undo.h"

struct editor_document {
    struct editor_text *text;
    struct editor_undo *undo;
    char *path;
    /* The file state and the base text hash at the last load or save. */
    struct editor_file_state base;
    uint64_t base_hash;
    struct editor_journal *journal;
    char *journal_path;
    bool journal_ok;
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

static bool set_path(struct editor_document *document, const char *path)
{
    char *copy = path ? string_copy(path) : NULL;
    if (path && !copy)
        return false;
    free(document->path);
    document->path = copy;
    return true;
}

static void journal_close(struct editor_document *document, bool remove)
{
    editor_journal_destroy(document->journal, remove);
    document->journal = NULL;
    document->journal_ok = false;
}

/* Starts a journal for the current base and text. */
static bool journal_start(struct editor_document *document, const char *path)
{
    journal_close(document, true);
    free(document->journal_path);
    document->journal_path = NULL;
    struct editor_session session = {
        .path = document->path,
        .base = document->base,
        .base_hash = document->base_hash,
    };
    document->journal = editor_journal_create(path, &session);
    if (!document->journal)
        return false;
    document->journal_path = string_copy(path);
    if (!document->journal_path) {
        journal_close(document, true);
        return false;
    }
    document->journal_ok = true;
    if (!editor_undo_modified(document->undo))
        return true;
    /* The text differs from the base text. One record makes the current text
     * recoverable. */
    size_t length = 0;
    char *text = editor_text_dup(document->text, &length);
    if (!text) {
        document->journal_ok = false;
        return false;
    }
    bool ok = editor_journal_edit(document->journal, 0, document->base.size, text,
                                  length);
    free(text);
    if (!ok)
        document->journal_ok = false;
    return ok;
}

struct editor_document *editor_document_create(void)
{
    struct editor_document *document = calloc(1, sizeof(*document));
    if (!document)
        return NULL;
    document->text = editor_text_create();
    document->undo = editor_undo_create();
    if (!document->text || !document->undo) {
        editor_document_destroy(document);
        return NULL;
    }
    document->base_hash = editor_file_hash("", 0);
    return document;
}

void editor_document_destroy(struct editor_document *document)
{
    if (!document)
        return;
    journal_close(document, false);
    free(document->journal_path);
    editor_text_destroy(document->text);
    editor_undo_destroy(document->undo);
    free(document->path);
    free(document);
}

bool editor_document_open(struct editor_document *document, const char *path)
{
    if (!document || !path)
        return false;
    size_t length = 0;
    char *bytes = editor_file_read(path, &length);
    if (!bytes && errno != ENOENT)
        return false;
    const char *text = bytes ? bytes : "";
    if (!editor_text_set(document->text, text, length)) {
        free(bytes);
        return false;
    }
    bool ok = set_path(document, path);
    if (ok) {
        document->base = (struct editor_file_state){0};
        if (bytes)
            editor_file_state_get(path, &document->base);
        document->base_hash = editor_file_hash(text, length);
        editor_undo_clear(document->undo);
    }
    free(bytes);
    if (ok && document->journal_path) {
        char *journal_path = string_copy(document->journal_path);
        if (journal_path) {
            journal_start(document, journal_path);
            free(journal_path);
        }
    }
    return ok;
}

bool editor_document_reload(struct editor_document *document)
{
    if (!document || !document->path)
        return false;
    char *path = string_copy(document->path);
    if (!path)
        return false;
    bool ok = editor_document_open(document, path);
    free(path);
    return ok;
}

const char *editor_document_path(const struct editor_document *document)
{
    return document ? document->path : NULL;
}

const struct editor_text *editor_document_text(const struct editor_document *document)
{
    return document ? document->text : NULL;
}

bool editor_document_replace(struct editor_document *document, uint64_t offset,
                             uint64_t remove, const char *insert,
                             size_t insert_length)
{
    if (!document)
        return false;
    uint64_t total = editor_text_length(document->text);
    if (offset > total || remove > total - offset)
        return false;
    if (!editor_undo_replace(document->undo, document->text, EDITOR_EDIT_TYPING,
                             offset, remove, insert, insert_length))
        return false;
    if (document->journal && document->journal_ok &&
        !editor_journal_edit(document->journal, offset, remove, insert, insert_length))
        document->journal_ok = false;
    return true;
}

bool editor_document_insert(struct editor_document *document, uint64_t offset,
                            const char *bytes, size_t length)
{
    return editor_document_replace(document, offset, 0, bytes, length);
}

bool editor_document_delete(struct editor_document *document, uint64_t offset,
                            uint64_t length)
{
    if (!document)
        return false;
    uint64_t total = editor_text_length(document->text);
    if (offset > total || length > total - offset)
        return false;
    if (!editor_undo_replace(document->undo, document->text, EDITOR_EDIT_DELETE,
                             offset, length, NULL, 0))
        return false;
    if (document->journal && document->journal_ok &&
        !editor_journal_edit(document->journal, offset, length, NULL, 0))
        document->journal_ok = false;
    return true;
}

void editor_document_break_undo(struct editor_document *document)
{
    if (document)
        editor_undo_break(document->undo);
}

bool editor_document_undo(struct editor_document *document)
{
    return document && editor_undo_undo(document->undo, document->text);
}

bool editor_document_redo(struct editor_document *document)
{
    return document && editor_undo_redo(document->undo, document->text);
}

bool editor_document_can_undo(const struct editor_document *document)
{
    return document && editor_undo_can_undo(document->undo);
}

bool editor_document_can_redo(const struct editor_document *document)
{
    return document && editor_undo_can_redo(document->undo);
}

bool editor_document_modified(const struct editor_document *document)
{
    return document && editor_undo_modified(document->undo);
}

bool editor_document_save(struct editor_document *document)
{
    if (!document || !document->path)
        return false;
    size_t length = 0;
    char *text = editor_text_dup(document->text, &length);
    if (!text)
        return false;
    struct editor_file_state state;
    bool ok = editor_file_save(document->path, text, length, &state);
    if (ok) {
        document->base = state;
        document->base_hash = editor_file_hash(text, length);
        editor_undo_mark_saved(document->undo);
        if (document->journal_path) {
            char *journal_path = string_copy(document->journal_path);
            if (journal_path) {
                journal_start(document, journal_path);
                free(journal_path);
            } else {
                document->journal_ok = false;
            }
        }
    }
    free(text);
    return ok;
}

bool editor_document_disk_changed(const struct editor_document *document)
{
    if (!document || !document->path)
        return false;
    return editor_file_check(&document->base, document->path) !=
           EDITOR_FILE_UNCHANGED;
}

bool editor_document_journal_attach(struct editor_document *document,
                                    const char *path)
{
    if (!document || !path)
        return false;
    return journal_start(document, path);
}

void editor_document_journal_detach(struct editor_document *document)
{
    if (!document)
        return;
    journal_close(document, false);
    free(document->journal_path);
    document->journal_path = NULL;
}

void editor_document_journal_commit(struct editor_document *document)
{
    if (!document)
        return;
    journal_close(document, true);
    free(document->journal_path);
    document->journal_path = NULL;
}

bool editor_document_journal_cursor(struct editor_document *document,
                                    uint64_t caret, uint64_t anchor)
{
    if (!document || !document->journal || !document->journal_ok)
        return false;
    if (!editor_journal_cursor(document->journal, caret, anchor)) {
        document->journal_ok = false;
        return false;
    }
    return true;
}

bool editor_document_journal_ok(const struct editor_document *document)
{
    return document && (!document->journal || document->journal_ok);
}

enum editor_journal_result editor_document_restore(struct editor_document *document,
                                                   const char *path,
                                                   uint64_t *caret,
                                                   uint64_t *anchor)
{
    if (!document || !path)
        return EDITOR_JOURNAL_ERROR;
    struct editor_recovery recovery = {0};
    enum editor_journal_result result = editor_journal_replay(path, &recovery);
    if (result != EDITOR_JOURNAL_OK) {
        editor_recovery_destroy(&recovery);
        return result;
    }
    if (!editor_text_set(document->text, recovery.text, recovery.length) ||
        !set_path(document, recovery.path)) {
        editor_recovery_destroy(&recovery);
        return EDITOR_JOURNAL_ERROR;
    }
    document->base = (struct editor_file_state){0};
    if (document->path)
        editor_file_state_get(document->path, &document->base);
    document->base_hash = recovery.base_hash;
    editor_undo_clear(document->undo);
    if (editor_file_hash(recovery.text, recovery.length) != recovery.base_hash)
        editor_undo_mark_modified(document->undo);
    if (caret)
        *caret = recovery.caret;
    if (anchor)
        *anchor = recovery.anchor;
    if (document->journal_path) {
        char *journal_path = string_copy(document->journal_path);
        if (journal_path) {
            journal_start(document, journal_path);
            free(journal_path);
        }
    }
    editor_recovery_destroy(&recovery);
    return EDITOR_JOURNAL_OK;
}
