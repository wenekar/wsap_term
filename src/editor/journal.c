#define _GNU_SOURCE

#include "editor/journal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "editor/text.h"

#define EDITOR_JOURNAL_MAGIC "WSAPEDJ1"
#define EDITOR_JOURNAL_MAGIC_SIZE 8
/* The record header holds the kind, the payload length, and the checksum. */
#define EDITOR_JOURNAL_RECORD_HEADER 9

enum editor_journal_kind {
    EDITOR_JOURNAL_SESSION = 1,
    EDITOR_JOURNAL_EDIT = 2,
    EDITOR_JOURNAL_CURSOR = 3,
};

struct editor_journal {
    int fd;
    char *path;
};

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--)
        value = (value << 8) | bytes[i];
    return value;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/* The record checksum finds a torn write at the end of the journal. */
static uint32_t journal_crc(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

struct payload {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    bool ok;
};

static void payload_add(struct payload *payload, const void *bytes, size_t length)
{
    if (!payload->ok)
        return;
    if (length > SIZE_MAX - payload->length) {
        payload->ok = false;
        return;
    }
    size_t need = payload->length + length;
    if (need > payload->capacity) {
        size_t capacity = payload->capacity ? payload->capacity : 64;
        while (capacity < need) {
            if (capacity > SIZE_MAX / 2) {
                payload->ok = false;
                return;
            }
            capacity *= 2;
        }
        uint8_t *grown = realloc(payload->bytes, capacity);
        if (!grown) {
            payload->ok = false;
            return;
        }
        payload->bytes = grown;
        payload->capacity = capacity;
    }
    memcpy(payload->bytes + payload->length, bytes, length);
    payload->length = need;
}

static void payload_u8(struct payload *payload, uint8_t value)
{
    payload_add(payload, &value, 1);
}

static void payload_u32(struct payload *payload, uint32_t value)
{
    uint8_t bytes[4];
    write_u32(bytes, value);
    payload_add(payload, bytes, sizeof(bytes));
}

static void payload_u64(struct payload *payload, uint64_t value)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++)
        bytes[i] = (uint8_t)(value >> (8 * i));
    payload_add(payload, bytes, sizeof(bytes));
}

static void payload_free(struct payload *payload)
{
    free(payload->bytes);
    *payload = (struct payload){0};
}

static bool write_all(int fd, const void *bytes, size_t length)
{
    const char *data = bytes;
    size_t done = 0;
    while (done < length) {
        ssize_t wrote = write(fd, data + done, length - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += (size_t)wrote;
    }
    return true;
}

static bool journal_write(struct editor_journal *journal, uint8_t kind,
                          const struct payload *payload)
{
    if (payload->length > UINT32_MAX)
        return false;
    size_t size = EDITOR_JOURNAL_RECORD_HEADER + payload->length;
    uint8_t *frame = malloc(size);
    if (!frame)
        return false;
    frame[0] = kind;
    write_u32(frame + 1, (uint32_t)payload->length);
    write_u32(frame + 5, journal_crc(payload->bytes, payload->length));
    if (payload->length)
        memcpy(frame + EDITOR_JOURNAL_RECORD_HEADER, payload->bytes, payload->length);
    bool ok = write_all(journal->fd, frame, size);
    free(frame);
    return ok;
}

static bool journal_session_record(struct editor_journal *journal,
                                   const struct editor_session *session)
{
    size_t path_length = session->path ? strlen(session->path) : 0;
    if (path_length > UINT32_MAX)
        return false;
    struct payload payload = { .ok = true };
    payload_u8(&payload, session->base.present ? 1 : 0);
    payload_u32(&payload, (uint32_t)path_length);
    payload_u64(&payload, session->base_hash);
    payload_u64(&payload, session->base.device);
    payload_u64(&payload, session->base.inode);
    payload_u64(&payload, session->base.size);
    payload_u64(&payload, (uint64_t)session->base.mtime_ns);
    if (path_length)
        payload_add(&payload, session->path, path_length);
    bool ok = payload.ok && journal_write(journal, EDITOR_JOURNAL_SESSION, &payload);
    payload_free(&payload);
    return ok;
}

struct editor_journal *editor_journal_create(const char *path,
                                             const struct editor_session *session)
{
    if (!path || !session)
        return NULL;
    struct editor_journal *journal = calloc(1, sizeof(*journal));
    if (!journal)
        return NULL;
    journal->fd = -1;
    journal->path = strdup(path);
    if (!journal->path) {
        free(journal);
        return NULL;
    }
    journal->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (journal->fd < 0) {
        free(journal->path);
        free(journal);
        return NULL;
    }
    if (!write_all(journal->fd, EDITOR_JOURNAL_MAGIC, EDITOR_JOURNAL_MAGIC_SIZE) ||
        !journal_session_record(journal, session) || fsync(journal->fd) < 0) {
        editor_journal_destroy(journal, true);
        return NULL;
    }
    return journal;
}

bool editor_journal_edit(struct editor_journal *journal, uint64_t offset,
                         uint64_t remove, const char *insert,
                         size_t insert_length)
{
    if (!journal || !insert)
        return false;
    struct payload payload = { .ok = true };
    payload_u64(&payload, offset);
    payload_u64(&payload, remove);
    payload_u64(&payload, (uint64_t)insert_length);
    if (insert_length)
        payload_add(&payload, insert, insert_length);
    bool ok = payload.ok && journal_write(journal, EDITOR_JOURNAL_EDIT, &payload);
    payload_free(&payload);
    return ok;
}

bool editor_journal_cursor(struct editor_journal *journal, uint64_t caret,
                           uint64_t anchor)
{
    if (!journal)
        return false;
    struct payload payload = { .ok = true };
    payload_u64(&payload, caret);
    payload_u64(&payload, anchor);
    bool ok = payload.ok && journal_write(journal, EDITOR_JOURNAL_CURSOR, &payload);
    payload_free(&payload);
    return ok;
}

bool editor_journal_sync(struct editor_journal *journal)
{
    if (!journal || journal->fd < 0)
        return false;
    while (fsync(journal->fd) < 0) {
        if (errno != EINTR)
            return false;
    }
    return true;
}

void editor_journal_destroy(struct editor_journal *journal, bool remove)
{
    if (!journal)
        return;
    if (journal->fd >= 0)
        close(journal->fd);
    if (remove && journal->path)
        unlink(journal->path);
    free(journal->path);
    free(journal);
}

struct reader {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
    bool ok;
};

static const uint8_t *reader_take(struct reader *reader, size_t length)
{
    if (!reader->ok || length > reader->length - reader->offset) {
        reader->ok = false;
        return NULL;
    }
    const uint8_t *bytes = reader->bytes + reader->offset;
    reader->offset += length;
    return bytes;
}

static uint8_t reader_u8(struct reader *reader)
{
    const uint8_t *bytes = reader_take(reader, 1);
    return bytes ? bytes[0] : 0;
}

static uint32_t reader_u32(struct reader *reader)
{
    const uint8_t *bytes = reader_take(reader, 4);
    return bytes ? read_u32(bytes) : 0;
}

static uint64_t reader_u64(struct reader *reader)
{
    const uint8_t *bytes = reader_take(reader, 8);
    return bytes ? read_u64(bytes) : 0;
}

struct record {
    uint8_t kind;
    const uint8_t *payload;
    size_t length;
};

/* Reads the record at `offset`. Returns false at the end of the file, or when
 * the record is incomplete. `truncated` reports the incomplete record. */
static bool next_record(const uint8_t *bytes, size_t length, size_t *offset,
                        struct record *record, bool *truncated)
{
    if (*offset == length)
        return false;
    if (length - *offset < EDITOR_JOURNAL_RECORD_HEADER) {
        *truncated = true;
        *offset = length;
        return false;
    }
    const uint8_t *header = bytes + *offset;
    uint32_t payload_length = read_u32(header + 1);
    if (payload_length > length - *offset - EDITOR_JOURNAL_RECORD_HEADER) {
        *truncated = true;
        *offset = length;
        return false;
    }
    const uint8_t *payload = header + EDITOR_JOURNAL_RECORD_HEADER;
    if (read_u32(header + 5) != journal_crc(payload, payload_length)) {
        *truncated = true;
        *offset = length;
        return false;
    }
    record->kind = header[0];
    record->payload = payload;
    record->length = payload_length;
    *offset += EDITOR_JOURNAL_RECORD_HEADER + payload_length;
    return true;
}

struct replay_edit {
    uint64_t offset;
    uint64_t remove;
    uint64_t insert_length;
    const uint8_t *insert;
};

static bool read_session(const struct record *record, struct editor_session *session)
{
    struct reader reader = {
        .bytes = record->payload,
        .length = record->length,
        .ok = true,
    };
    uint8_t flags = reader_u8(&reader);
    uint32_t path_length = reader_u32(&reader);
    session->base_hash = reader_u64(&reader);
    session->base.device = reader_u64(&reader);
    session->base.inode = reader_u64(&reader);
    session->base.size = reader_u64(&reader);
    session->base.mtime_ns = (int64_t)reader_u64(&reader);
    const uint8_t *path = path_length ? reader_take(&reader, path_length) : NULL;
    if (!reader.ok || (path_length && !path))
        return false;
    session->base.present = (flags & 1u) != 0;
    if (!path_length)
        return true;
    session->path = malloc((size_t)path_length + 1);
    if (!session->path)
        return false;
    memcpy(session->path, path, path_length);
    session->path[path_length] = '\0';
    return true;
}

static bool add_edit(struct replay_edit **edits, size_t *count, size_t *capacity,
                     struct reader *reader)
{
    uint64_t offset = reader_u64(reader);
    uint64_t remove = reader_u64(reader);
    uint64_t insert_length = reader_u64(reader);
    const uint8_t *insert = NULL;
    if (insert_length) {
        if (insert_length > reader->length - reader->offset) {
            reader->ok = false;
            return false;
        }
        insert = reader_take(reader, (size_t)insert_length);
    }
    if (!reader->ok || (insert_length && !insert))
        return false;
    if (*count == *capacity) {
        size_t grown_capacity = *capacity ? *capacity * 2 : 16;
        struct replay_edit *grown =
            realloc(*edits, grown_capacity * sizeof(*grown));
        if (!grown)
            return false;
        *edits = grown;
        *capacity = grown_capacity;
    }
    (*edits)[*count] = (struct replay_edit){
        .offset = offset,
        .remove = remove,
        .insert_length = insert_length,
        .insert = insert,
    };
    (*count)++;
    return true;
}

enum editor_journal_result editor_journal_replay(const char *path,
                                                 struct editor_recovery *recovery)
{
    if (!path || !recovery)
        return EDITOR_JOURNAL_ERROR;
    *recovery = (struct editor_recovery){0};
    size_t length = 0;
    char *bytes = editor_file_read(path, &length);
    if (!bytes)
        return errno == ENOENT ? EDITOR_JOURNAL_NONE : EDITOR_JOURNAL_ERROR;
    if (length < EDITOR_JOURNAL_MAGIC_SIZE ||
        memcmp(bytes, EDITOR_JOURNAL_MAGIC, EDITOR_JOURNAL_MAGIC_SIZE) != 0) {
        free(bytes);
        return EDITOR_JOURNAL_NONE;
    }
    struct editor_session session = {0};
    struct replay_edit *edits = NULL;
    size_t edit_count = 0;
    size_t edit_capacity = 0;
    uint64_t caret = 0;
    uint64_t anchor = 0;
    bool truncated = false;
    size_t offset = EDITOR_JOURNAL_MAGIC_SIZE;
    struct record record;
    if (!next_record((const uint8_t *)bytes, length, &offset, &record, &truncated) ||
        record.kind != EDITOR_JOURNAL_SESSION || !read_session(&record, &session)) {
        free(bytes);
        return EDITOR_JOURNAL_NONE;
    }
    while (!truncated &&
           next_record((const uint8_t *)bytes, length, &offset, &record, &truncated)) {
        struct reader reader = {
            .bytes = record.payload,
            .length = record.length,
            .ok = true,
        };
        if (record.kind == EDITOR_JOURNAL_EDIT) {
            if (!add_edit(&edits, &edit_count, &edit_capacity, &reader)) {
                truncated = true;
                break;
            }
            continue;
        }
        if (record.kind == EDITOR_JOURNAL_CURSOR) {
            caret = reader_u64(&reader);
            anchor = reader_u64(&reader);
            if (!reader.ok) {
                truncated = true;
                break;
            }
            continue;
        }
        truncated = true;
        break;
    }
    enum editor_journal_result result = EDITOR_JOURNAL_OK;
    char *base = NULL;
    size_t base_length = 0;
    if (session.base.present) {
        base = session.path ? editor_file_read(session.path, &base_length) : NULL;
        if (!base) {
            result = EDITOR_JOURNAL_BASE_MISSING;
            goto finish;
        }
        if (editor_file_hash(base, base_length) != session.base_hash) {
            result = EDITOR_JOURNAL_BASE_CHANGED;
            goto finish;
        }
    }
    struct editor_text *text = editor_text_create();
    if (!text) {
        result = EDITOR_JOURNAL_ERROR;
        goto finish;
    }
    if (!editor_text_set(text, base ? base : "", base_length)) {
        editor_text_destroy(text);
        result = EDITOR_JOURNAL_ERROR;
        goto finish;
    }
    for (size_t i = 0; i < edit_count; i++) {
        const struct replay_edit *edit = &edits[i];
        if (!editor_text_replace(text, edit->offset, edit->remove,
                                 (const char *)edit->insert,
                                 (size_t)edit->insert_length)) {
            truncated = true;
            break;
        }
        recovery->edits++;
    }
    recovery->text = editor_text_dup(text, &recovery->length);
    editor_text_destroy(text);
    if (!recovery->text) {
        result = EDITOR_JOURNAL_ERROR;
        goto finish;
    }
finish:
    if (result != EDITOR_JOURNAL_ERROR) {
        recovery->path = session.path;
        session.path = NULL;
        recovery->base = session.base;
        recovery->base_hash = session.base_hash;
        recovery->caret = caret;
        recovery->anchor = anchor;
        recovery->truncated = truncated;
    }
    free(session.path);
    free(base);
    free(edits);
    free(bytes);
    return result;
}

void editor_recovery_destroy(struct editor_recovery *recovery)
{
    if (!recovery)
        return;
    free(recovery->path);
    free(recovery->text);
    *recovery = (struct editor_recovery){0};
}
