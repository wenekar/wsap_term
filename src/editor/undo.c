#include "editor/undo.h"

#include <stdlib.h>
#include <string.h>

/* A very long typing run stops joining one group after this many edits. The
 * history then holds bounded memory for each group. */
#define EDITOR_UNDO_GROUP_MAX 1024

struct editor_change {
    uint64_t offset;
    char *insert;
    size_t insert_length;
    char *remove;
    size_t remove_length;
};

struct editor_group {
    struct editor_change *changes;
    int count;
    int capacity;
    enum editor_edit_kind kind;
    /* A break or a command stops the group from taking more edits. */
    bool closed;
};

struct editor_undo {
    struct editor_group *groups;
    int count;
    int capacity;
    /* The number of groups that the text holds. The other groups are the redo
     * part of the history. */
    int applied;
    /* The `applied` value that matches the file, or -1 when no history state
     * matches it. */
    int saved;
};

static void change_free(struct editor_change *change)
{
    free(change->insert);
    free(change->remove);
    *change = (struct editor_change){0};
}

static void group_free(struct editor_group *group)
{
    for (int i = 0; i < group->count; i++)
        change_free(&group->changes[i]);
    free(group->changes);
    *group = (struct editor_group){0};
}

static bool change_copy(struct editor_change *change, uint64_t offset,
                        const char *remove, size_t remove_length,
                        const char *insert, size_t insert_length)
{
    *change = (struct editor_change){ .offset = offset };
    if (insert_length > 0) {
        change->insert = malloc(insert_length);
        if (!change->insert)
            return false;
        memcpy(change->insert, insert, insert_length);
        change->insert_length = insert_length;
    }
    if (remove_length > 0) {
        change->remove = malloc(remove_length);
        if (!change->remove) {
            change_free(change);
            return false;
        }
        memcpy(change->remove, remove, remove_length);
        change->remove_length = remove_length;
    }
    return true;
}

static bool group_add(struct editor_group *group, uint64_t offset,
                      const char *remove, size_t remove_length,
                      const char *insert, size_t insert_length)
{
    if (group->count == group->capacity) {
        int capacity = group->capacity ? group->capacity * 2 : 8;
        struct editor_change *grown =
            realloc(group->changes, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        group->changes = grown;
        group->capacity = capacity;
    }
    struct editor_change *change = &group->changes[group->count];
    if (!change_copy(change, offset, remove, remove_length, insert, insert_length))
        return false;
    group->count++;
    return true;
}

/* Returns true when an edit continues the run of `group`. */
static bool change_joins(const struct editor_group *group, enum editor_edit_kind kind,
                         uint64_t offset, uint64_t remove_length,
                         size_t insert_length)
{
    if (group->closed || group->count == 0)
        return false;
    const struct editor_change *last = &group->changes[group->count - 1];
    if (kind == EDITOR_EDIT_TYPING) {
        return insert_length > 0 && remove_length == 0 && last->remove_length == 0 &&
               last->insert_length > 0 &&
               offset == last->offset + last->insert_length;
    }
    if (kind == EDITOR_EDIT_DELETE) {
        return insert_length == 0 && last->insert_length == 0 && remove_length > 0 &&
               last->remove_length > 0 &&
               (offset + remove_length == last->offset || offset == last->offset);
    }
    return false;
}

/* Drops the redo part of the history. */
static void undo_drop_redo(struct editor_undo *undo)
{
    for (int i = undo->applied; i < undo->count; i++)
        group_free(&undo->groups[i]);
    undo->count = undo->applied;
    if (undo->saved > undo->applied)
        undo->saved = -1;
}

static bool undo_record(struct editor_undo *undo, enum editor_edit_kind kind,
                        uint64_t offset, const char *remove, size_t remove_length,
                        const char *insert, size_t insert_length)
{
    undo_drop_redo(undo);
    if (undo->applied > 0) {
        struct editor_group *last = &undo->groups[undo->applied - 1];
        if (last->kind == kind && last->count < EDITOR_UNDO_GROUP_MAX &&
            change_joins(last, kind, offset, remove_length, insert_length))
            return group_add(last, offset, remove, remove_length, insert, insert_length);
    }
    if (undo->count == undo->capacity) {
        int capacity = undo->capacity ? undo->capacity * 2 : 16;
        struct editor_group *grown =
            realloc(undo->groups, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        undo->groups = grown;
        undo->capacity = capacity;
    }
    struct editor_group *group = &undo->groups[undo->count];
    *group = (struct editor_group){ .kind = kind };
    if (!group_add(group, offset, remove, remove_length, insert, insert_length)) {
        *group = (struct editor_group){0};
        return false;
    }
    undo->count++;
    undo->applied = undo->count;
    return true;
}

/* Removes the change that the last `undo_record` added. */
static void undo_forget(struct editor_undo *undo)
{
    if (undo->applied == 0)
        return;
    struct editor_group *group = &undo->groups[undo->applied - 1];
    if (group->count == 0)
        return;
    change_free(&group->changes[--group->count]);
    if (group->count == 0) {
        group_free(group);
        undo->count--;
        undo->applied = undo->count;
    }
}

struct editor_undo *editor_undo_create(void)
{
    return calloc(1, sizeof(struct editor_undo));
}

void editor_undo_destroy(struct editor_undo *undo)
{
    if (!undo)
        return;
    editor_undo_clear(undo);
    free(undo->groups);
    free(undo);
}

void editor_undo_clear(struct editor_undo *undo)
{
    if (!undo)
        return;
    for (int i = 0; i < undo->count; i++)
        group_free(&undo->groups[i]);
    undo->count = 0;
    undo->applied = 0;
    undo->saved = 0;
}

bool editor_undo_replace(struct editor_undo *undo, struct editor_text *text,
                         enum editor_edit_kind kind, uint64_t offset,
                         uint64_t remove, const char *insert,
                         size_t insert_length)
{
    if (!undo || !text)
        return false;
    uint64_t total = editor_text_length(text);
    if (offset > total || remove > total - offset)
        return false;
    if (remove == 0 && insert_length == 0)
        return true;
    char *removed = NULL;
    if (remove > 0) {
        removed = malloc((size_t)remove);
        if (!removed)
            return false;
        editor_text_copy(text, offset, remove, removed);
    }
    if (!undo_record(undo, kind, offset, removed, (size_t)remove, insert,
                     insert_length)) {
        free(removed);
        return false;
    }
    free(removed);
    if (!editor_text_replace(text, offset, remove, insert, insert_length)) {
        undo_forget(undo);
        return false;
    }
    return true;
}

bool editor_undo_undo(struct editor_undo *undo, struct editor_text *text)
{
    if (!undo || !text || undo->applied == 0)
        return false;
    struct editor_group *group = &undo->groups[undo->applied - 1];
    for (int i = group->count - 1; i >= 0; i--) {
        struct editor_change *change = &group->changes[i];
        if (!editor_text_replace(text, change->offset, change->insert_length,
                                  change->remove, change->remove_length))
            return false;
    }
    undo->applied--;
    return true;
}

bool editor_undo_redo(struct editor_undo *undo, struct editor_text *text)
{
    if (!undo || !text || undo->applied >= undo->count)
        return false;
    struct editor_group *group = &undo->groups[undo->applied];
    for (int i = 0; i < group->count; i++) {
        struct editor_change *change = &group->changes[i];
        if (!editor_text_replace(text, change->offset, change->remove_length,
                                  change->insert, change->insert_length))
            return false;
    }
    undo->applied++;
    return true;
}

void editor_undo_break(struct editor_undo *undo)
{
    if (!undo || undo->applied == 0)
        return;
    undo->groups[undo->applied - 1].closed = true;
}

bool editor_undo_can_undo(const struct editor_undo *undo)
{
    return undo && undo->applied > 0;
}

bool editor_undo_can_redo(const struct editor_undo *undo)
{
    return undo && undo->applied < undo->count;
}

void editor_undo_mark_saved(struct editor_undo *undo)
{
    if (undo)
        undo->saved = undo->applied;
}

void editor_undo_mark_modified(struct editor_undo *undo)
{
    if (undo)
        undo->saved = -1;
}

bool editor_undo_modified(const struct editor_undo *undo)
{
    return undo && (undo->saved < 0 || undo->saved != undo->applied);
}
