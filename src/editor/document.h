#ifndef EDITOR_DOCUMENT_H
#define EDITOR_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "editor/encoding.h"
#include "editor/journal.h"
#include "editor/text.h"

struct editor_document;

struct editor_document *editor_document_create(void);
void editor_document_destroy(struct editor_document *document);

/* Loads the file at `path`. A missing file gives an empty document. */
bool editor_document_open(struct editor_document *document, const char *path);
/* Loads the file again and discards the changes. */
bool editor_document_reload(struct editor_document *document);

const char *editor_document_path(const struct editor_document *document);
const struct editor_text *editor_document_text(const struct editor_document *document);

/* How the file holds the text. The document text stays UTF-8, and a save
 * converts it back. A load sets it from the byte order mark or the bytes. */
struct editor_encoding_info editor_document_encoding(const struct editor_document *document);
void editor_document_set_encoding(struct editor_document *document,
                                 struct editor_encoding_info encoding);

/* Replaces `remove` bytes at `offset` with `insert_length` bytes from
 * `insert`. */
bool editor_document_replace(struct editor_document *document, uint64_t offset,
                             uint64_t remove, const char *insert,
                             size_t insert_length);
bool editor_document_insert(struct editor_document *document, uint64_t offset,
                            const char *bytes, size_t length);
bool editor_document_delete(struct editor_document *document, uint64_t offset,
                            uint64_t length);

/* Ends the current undo group. Call it before a command that is not typing. */
void editor_document_break_undo(struct editor_document *document);
bool editor_document_undo(struct editor_document *document);
bool editor_document_redo(struct editor_document *document);
bool editor_document_can_undo(const struct editor_document *document);
bool editor_document_can_redo(const struct editor_document *document);
/* True when the text differs from the last load or save. */
bool editor_document_modified(const struct editor_document *document);

/* Writes the text to the file, atomically. */
bool editor_document_save(struct editor_document *document);
/* True when the file changed on disk since the last load or save. */
bool editor_document_disk_changed(const struct editor_document *document);

/* Writes a recovery journal for the document. The journal keeps the file until
 * `editor_document_journal_commit` removes it. */
bool editor_document_journal_attach(struct editor_document *document,
                                    const char *path);
/* Closes the journal. Its file stays for the next start. */
void editor_document_journal_detach(struct editor_document *document);
/* Closes the journal and removes its file. */
void editor_document_journal_commit(struct editor_document *document);
bool editor_document_journal_cursor(struct editor_document *document,
                                    uint64_t caret, uint64_t anchor);
/* False when the journal stopped writing because of an error. */
bool editor_document_journal_ok(const struct editor_document *document);

/* Restores the document from a journal. `caret` and `anchor` are optional.
 * The result is `EDITOR_JOURNAL_OK` when the document holds the text. */
enum editor_journal_result editor_document_restore(struct editor_document *document,
                                                   const char *path,
                                                   uint64_t *caret,
                                                   uint64_t *anchor);

#endif
