# Editor document model

This design note describes the editor document model recorded in commit `e4894e0`. The editor and its tests are not part of `wsap_term` at commit `58c9d47`.

The design places the document model above the base library and below the UI library. The model does not depend on yasl, the terminal core, or UI headers.

## Text buffer

`editor_text` is a piece table.
Two buffers hold the bytes: the original file bytes and the bytes that
insertions add.
A piece points into one buffer.
A weight-balanced tree holds the pieces.
Each node counts its bytes, line breaks, and CRLF pairs.
An edit splits the tree at the byte offset and joins a new piece.
An insert and a delete cost O(log N).

A piece holds at most 64 line breaks and 8192 bytes.
A line lookup therefore scans one short piece.
`editor_text_line` reads one line of the line index.
`editor_text_locate` gives the line and the byte column of an offset.
`editor_text_crlf_count` counts CRLF pairs, also across a piece boundary.
`editor_text_line_ending` reports CRLF when the text holds one CRLF pair.

## Positions

`editor/position.h` converts a byte offset to an LSP position and back.
The column counts UTF-8 bytes, UTF-16 code units, or UTF-32 code points.
A code point above U+FFFF takes two UTF-16 units.
An offset inside a UTF-8 sequence is not a position, and a column inside a
surrogate pair is not an offset.
An invalid byte counts as one code point.

## Undo and redo

`editor_undo` is the command history.
One change holds the byte offset, the removed bytes, and the inserted bytes.
A group holds one or more changes.
Adjacent typing joins one group, and so does an adjacent delete run.
`editor_undo_break` ends a group.
`EDITOR_EDIT_COMMAND` always starts a new group.
A new edit drops the redo part of the history.
`editor_undo_modified` compares the applied group count with the save point.

## Files

`editor_file_save` writes a temporary file in the target directory.
It flushes the file, copies the mode of the target, and renames the file.
It then flushes the directory.
`editor_file_state_get` records the device, inode, size, and modification
time of a file.
`editor_file_check` reports a change, a missing file, or no change.

`editor_document` joins the text, the history, and the file state.
It opens, reloads, and saves the file.
It reports a change on disk.
It restores a document from a journal.

## Recovery journal

The journal is an append-only file.
Eight magic bytes start it.
Each record is a kind, a payload length, a CRC32 of the payload, and the
payload.
The record kinds are the session, an edit, and the cursor.
The session record holds the path, the file state, and the hash of the base
text.
An edit record holds the offset, the removed length, and the inserted bytes.
The cursor record holds the caret and the anchor.

Replay reads the journal and applies the complete records in order, on the
base text.
The base text is the file, or the empty text for an untitled document.
Replay stops at the first incomplete or corrupt record.
That prefix is the text after the last complete edit.
A clean commit removes the journal file.
A save starts a new journal from the new file.

## Scope

This layer holds no huge-file mode, no chunked or `mmap` backing, no
long-line viewport slicing, no binary detection, and no non-UTF-8 code page.
It holds no syntax highlighting, diagnostics, folding, bracket matching, or
multi-cursor editing.
The design leaves these features to a separate widget layer.
