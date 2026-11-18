#include "editor/text.h"

#include <stdlib.h>
#include <string.h>

/* A piece that holds many line breaks makes a line lookup scan many bytes.
 * A piece splits after this many line breaks, and after this many bytes at
 * the latest. */
#define EDITOR_PIECE_NEWLINES 64
#define EDITOR_PIECE_BYTES 8192

struct editor_buffer {
    char *bytes;
    uint64_t length;
    uint64_t capacity;
};

/* A run of bytes in one of the two buffers. The measurements make the line
 * index cheap: a subtree counts them without a scan. */
struct editor_piece {
    uint64_t start;
    uint64_t length;
    uint64_t newlines;
    uint64_t crlf;
    bool added;
    char first;
    char last;
};

/* A node of the weight-balanced tree. The byte counts balance the tree and
 * find a byte offset. */
struct editor_node {
    struct editor_piece piece;
    int height;
    uint64_t bytes;
    uint64_t newlines;
    uint64_t crlf;
    char first;
    char last;
    struct editor_node *left;
    struct editor_node *right;
};

struct editor_text {
    struct editor_node *root;
    /* The file bytes. A piece never changes them. */
    struct editor_buffer original;
    /* The bytes that insertions added. A piece points into it. */
    struct editor_buffer added;
};

static int node_height(const struct editor_node *node)
{
    return node ? node->height : 0;
}

static uint64_t node_bytes(const struct editor_node *node)
{
    return node ? node->bytes : 0;
}

static uint64_t node_newlines(const struct editor_node *node)
{
    return node ? node->newlines : 0;
}

static uint64_t node_crlf(const struct editor_node *node)
{
    return node ? node->crlf : 0;
}

static void node_update(struct editor_node *node)
{
    int left = node_height(node->left);
    int right = node_height(node->right);
    node->height = 1 + (left > right ? left : right);
    node->bytes = node->piece.length + node_bytes(node->left) + node_bytes(node->right);
    node->newlines = node->piece.newlines + node_newlines(node->left) +
                     node_newlines(node->right);
    node->crlf = node->piece.crlf + node_crlf(node->left) + node_crlf(node->right);
    if (node->left && node->left->last == '\r' && node->piece.first == '\n')
        node->crlf++;
    if (node->right && node->piece.last == '\r' && node->right->first == '\n')
        node->crlf++;
    node->first = node->left ? node->left->first : node->piece.first;
    node->last = node->right ? node->right->last : node->piece.last;
}

static struct editor_node *node_rotate_left(struct editor_node *node)
{
    struct editor_node *right = node->right;
    node->right = right->left;
    right->left = node;
    node_update(node);
    node_update(right);
    return right;
}

static struct editor_node *node_rotate_right(struct editor_node *node)
{
    struct editor_node *left = node->left;
    node->left = left->right;
    left->right = node;
    node_update(node);
    node_update(left);
    return left;
}

static struct editor_node *node_fix(struct editor_node *node)
{
    node_update(node);
    int balance = node_height(node->left) - node_height(node->right);
    if (balance > 1) {
        if (node_height(node->left->left) < node_height(node->left->right))
            node->left = node_rotate_left(node->left);
        return node_rotate_right(node);
    }
    if (balance < -1) {
        if (node_height(node->right->right) < node_height(node->right->left))
            node->right = node_rotate_right(node->right);
        return node_rotate_left(node);
    }
    return node;
}

/* Joins two trees. Every byte of `left` comes before every byte of `right`. */
static struct editor_node *node_merge(struct editor_node *left,
                                      struct editor_node *right)
{
    if (!left)
        return right;
    if (!right)
        return left;
    if (node_height(left) >= node_height(right)) {
        left->right = node_merge(left->right, right);
        return node_fix(left);
    }
    right->left = node_merge(left, right->left);
    return node_fix(right);
}

static void node_free(struct editor_node *node)
{
    if (!node)
        return;
    node_free(node->left);
    node_free(node->right);
    free(node);
}

static const char *source_data(const struct editor_text *text, bool added)
{
    const struct editor_buffer *buffer = added ? &text->added : &text->original;
    return buffer->bytes;
}

static void piece_measure(struct editor_piece *piece, const char *bytes,
                          bool added, uint64_t start, uint64_t length)
{
    uint64_t newlines = 0;
    uint64_t crlf = 0;
    const char *data = bytes + start;
    for (uint64_t i = 0; i < length; i++) {
        if (data[i] == '\n') {
            newlines++;
            if (i > 0 && data[i - 1] == '\r')
                crlf++;
        }
    }
    piece->start = start;
    piece->length = length;
    piece->newlines = newlines;
    piece->crlf = crlf;
    piece->added = added;
    piece->first = data[0];
    piece->last = data[length - 1];
}

static struct editor_node *node_create(const struct editor_text *text, bool added,
                                       uint64_t start, uint64_t length)
{
    struct editor_node *node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    piece_measure(&node->piece, source_data(text, added), added, start, length);
    node_update(node);
    return node;
}

static uint64_t chunk_length(const char *bytes, uint64_t length)
{
    uint64_t limit = length < EDITOR_PIECE_BYTES ? length : EDITOR_PIECE_BYTES;
    uint64_t newlines = 0;
    for (uint64_t i = 0; i < limit; i++) {
        if (bytes[i] == '\n' && ++newlines == EDITOR_PIECE_NEWLINES)
            return i + 1;
    }
    return limit;
}

/* Builds a tree for `length` bytes of one buffer. */
static struct editor_node *tree_build(const struct editor_text *text, bool added,
                                      uint64_t start, uint64_t length)
{
    struct editor_node *root = NULL;
    uint64_t done = 0;
    while (done < length) {
        uint64_t chunk = chunk_length(source_data(text, added) + start + done,
                                      length - done);
        struct editor_node *node = node_create(text, added, start + done, chunk);
        if (!node) {
            node_free(root);
            return NULL;
        }
        root = node_merge(root, node);
        done += chunk;
    }
    return root;
}

/* Splits the tree into the bytes before `at` and the bytes from `at`.
 * A failure leaves `node` unchanged and reports the whole tree as `left`. */
static bool node_split(const struct editor_text *text, struct editor_node *node,
                       uint64_t at, struct editor_node **left,
                       struct editor_node **right)
{
    if (!node) {
        *left = NULL;
        *right = NULL;
        return true;
    }
    uint64_t left_bytes = node_bytes(node->left);
    uint64_t piece_end = left_bytes + node->piece.length;
    if (at < left_bytes) {
        struct editor_node *a;
        struct editor_node *b;
        if (!node_split(text, node->left, at, &a, &b)) {
            *left = node;
            *right = NULL;
            return false;
        }
        node->left = b;
        *left = a;
        *right = node_fix(node);
        return true;
    }
    if (at > piece_end) {
        struct editor_node *a;
        struct editor_node *b;
        if (!node_split(text, node->right, at - piece_end, &a, &b)) {
            *left = node;
            *right = NULL;
            return false;
        }
        node->right = a;
        *left = node_fix(node);
        *right = b;
        return true;
    }
    if (at == left_bytes) {
        struct editor_node *left_tree = node->left;
        struct editor_node *right_tree = node->right;
        node->left = NULL;
        node->right = NULL;
        node_update(node);
        *left = left_tree;
        *right = node_merge(node, right_tree);
        return true;
    }
    if (at == piece_end) {
        struct editor_node *left_tree = node->left;
        struct editor_node *right_tree = node->right;
        node->left = NULL;
        node->right = NULL;
        node_update(node);
        *left = node_merge(left_tree, node);
        *right = right_tree;
        return true;
    }
    /* The byte offset is inside the piece. */
    uint64_t cut = at - left_bytes;
    struct editor_node *tail = node_create(text, node->piece.added,
                                           node->piece.start + cut,
                                           node->piece.length - cut);
    if (!tail) {
        *left = node;
        *right = NULL;
        return false;
    }
    struct editor_node *head_left = node->left;
    struct editor_node *tail_right = node->right;
    piece_measure(&node->piece, source_data(text, node->piece.added),
                  node->piece.added, node->piece.start, cut);
    node->left = NULL;
    node->right = NULL;
    node_update(node);
    *left = node_merge(head_left, node);
    *right = node_merge(tail, tail_right);
    return true;
}

static uint8_t text_byte(const struct editor_text *text, uint64_t offset)
{
    uint8_t byte = 0;
    editor_text_copy(text, offset, 1, (char *)&byte);
    return byte;
}

static bool find_newline(const struct editor_text *text,
                         const struct editor_node *node, uint64_t base,
                         uint64_t index, uint64_t *position)
{
    if (!node)
        return false;
    uint64_t left = node_bytes(node->left);
    uint64_t before = node_newlines(node->left);
    if (index <= before)
        return find_newline(text, node->left, base, index, position);
    if (index <= before + node->piece.newlines) {
        uint64_t remaining = index - before;
        const char *bytes = source_data(text, node->piece.added) + node->piece.start;
        for (uint64_t i = 0; i < node->piece.length; i++) {
            if (bytes[i] == '\n' && --remaining == 0) {
                *position = base + left + i;
                return true;
            }
        }
        return false;
    }
    return find_newline(text, node->right, base + left + node->piece.length,
                        index - before - node->piece.newlines, position);
}

/* Counts the line breaks in the bytes before `limit`. */
static uint64_t count_newlines(const struct editor_text *text,
                               const struct editor_node *node, uint64_t base,
                               uint64_t limit)
{
    if (!node || limit <= base)
        return 0;
    uint64_t left = node_bytes(node->left);
    uint64_t piece_start = base + left;
    uint64_t piece_end = piece_start + node->piece.length;
    if (limit <= piece_start)
        return count_newlines(text, node->left, base, limit);
    uint64_t count = node_newlines(node->left);
    uint64_t stop = limit < piece_end ? limit : piece_end;
    const char *bytes = source_data(text, node->piece.added) + node->piece.start;
    for (uint64_t i = piece_start; i < stop; i++) {
        if (bytes[i - piece_start] == '\n')
            count++;
    }
    if (limit <= piece_end)
        return count;
    return count + count_newlines(text, node->right, piece_end, limit);
}

static void copy_node(const struct editor_text *text, const struct editor_node *node,
                      uint64_t base, uint64_t offset, uint64_t end, char *bytes,
                      uint64_t *written)
{
    if (!node)
        return;
    uint64_t piece_start = base + node_bytes(node->left);
    uint64_t piece_end = piece_start + node->piece.length;
    if (node->left && base < end && piece_start > offset)
        copy_node(text, node->left, base, offset, end, bytes, written);
    if (piece_end > offset && piece_start < end) {
        uint64_t from = offset > piece_start ? offset : piece_start;
        uint64_t to = end < piece_end ? end : piece_end;
        const char *data = source_data(text, node->piece.added) + node->piece.start +
                           (from - piece_start);
        memcpy(bytes + *written, data, (size_t)(to - from));
        *written += to - from;
    }
    if (node->right && piece_end < end)
        copy_node(text, node->right, piece_end, offset, end, bytes, written);
}

static bool buffer_append(struct editor_buffer *buffer, const char *bytes,
                          uint64_t length)
{
    if (length > UINT64_MAX - buffer->length)
        return false;
    uint64_t need = buffer->length + length;
    if (need > buffer->capacity) {
        uint64_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < need) {
            if (capacity > UINT64_MAX / 2)
                return false;
            capacity *= 2;
        }
        if (capacity > SIZE_MAX)
            return false;
        char *grown = realloc(buffer->bytes, (size_t)capacity);
        if (!grown)
            return false;
        buffer->bytes = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->bytes + buffer->length, bytes, (size_t)length);
    buffer->length = need;
    return true;
}

struct editor_text *editor_text_create(void)
{
    return calloc(1, sizeof(struct editor_text));
}

void editor_text_destroy(struct editor_text *text)
{
    if (!text)
        return;
    node_free(text->root);
    free(text->original.bytes);
    free(text->added.bytes);
    free(text);
}

bool editor_text_set(struct editor_text *text, const char *bytes, size_t length)
{
    if (!text)
        return false;
    node_free(text->root);
    text->root = NULL;
    free(text->original.bytes);
    text->original = (struct editor_buffer){0};
    free(text->added.bytes);
    text->added = (struct editor_buffer){0};
    if (length == 0)
        return true;
    if (!bytes)
        return false;
    if (!buffer_append(&text->original, bytes, length))
        return false;
    text->root = tree_build(text, false, 0, length);
    return text->root != NULL;
}

uint64_t editor_text_length(const struct editor_text *text)
{
    return text ? node_bytes(text->root) : 0;
}

uint64_t editor_text_newline_count(const struct editor_text *text)
{
    return text ? node_newlines(text->root) : 0;
}

uint64_t editor_text_crlf_count(const struct editor_text *text)
{
    return text ? node_crlf(text->root) : 0;
}

enum editor_line_ending editor_text_line_ending(const struct editor_text *text)
{
    return editor_text_crlf_count(text) > 0 ? EDITOR_LINE_CRLF : EDITOR_LINE_LF;
}

uint32_t editor_text_line_count(const struct editor_text *text)
{
    uint64_t count = editor_text_newline_count(text) + 1;
    return count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
}

bool editor_text_insert(struct editor_text *text, uint64_t offset, const char *bytes,
                        size_t length)
{
    if (!text)
        return false;
    if (offset > editor_text_length(text))
        return false;
    if (length == 0)
        return true;
    if (!bytes)
        return false;
    uint64_t start = text->added.length;
    if (!buffer_append(&text->added, bytes, length))
        return false;
    struct editor_node *tree = tree_build(text, true, start, length);
    if (!tree)
        return false;
    struct editor_node *left;
    struct editor_node *right;
    if (!node_split(text, text->root, offset, &left, &right))
        return false;
    text->root = node_merge(node_merge(left, tree), right);
    return true;
}

bool editor_text_delete(struct editor_text *text, uint64_t offset, uint64_t length)
{
    if (!text)
        return false;
    uint64_t total = editor_text_length(text);
    if (offset > total || length > total - offset)
        return false;
    if (length == 0)
        return true;
    struct editor_node *left;
    struct editor_node *rest;
    if (!node_split(text, text->root, offset, &left, &rest))
        return false;
    struct editor_node *middle;
    struct editor_node *right;
    if (!node_split(text, rest, length, &middle, &right)) {
        text->root = node_merge(left, rest);
        return false;
    }
    node_free(middle);
    text->root = node_merge(left, right);
    return true;
}

bool editor_text_replace(struct editor_text *text, uint64_t offset, uint64_t remove,
                         const char *bytes, size_t length)
{
    if (!text)
        return false;
    uint64_t total = editor_text_length(text);
    if (offset > total || remove > total - offset)
        return false;
    if (remove == 0 && length == 0)
        return true;
    if (!editor_text_insert(text, offset, bytes, length))
        return false;
    /* The remove range moves right by the inserted length. The range is
     * inside the text, so the delete cannot fail. */
    editor_text_delete(text, offset + length, remove);
    return true;
}

uint64_t editor_text_copy(const struct editor_text *text, uint64_t offset,
                          uint64_t length, char *bytes)
{
    if (!text || !bytes || length == 0)
        return 0;
    uint64_t total = editor_text_length(text);
    if (offset >= total)
        return 0;
    if (length > total - offset)
        length = total - offset;
    uint64_t written = 0;
    copy_node(text, text->root, 0, offset, offset + length, bytes, &written);
    return written;
}

char *editor_text_dup(const struct editor_text *text, size_t *length)
{
    uint64_t total = editor_text_length(text);
    if (total >= SIZE_MAX)
        return NULL;
    char *copy = malloc((size_t)total + 1);
    if (!copy)
        return NULL;
    editor_text_copy(text, 0, total, copy);
    copy[total] = '\0';
    if (length)
        *length = (size_t)total;
    return copy;
}

bool editor_text_line(const struct editor_text *text, uint32_t index,
                      struct editor_line *line)
{
    if (!text)
        return false;
    uint64_t total = editor_text_length(text);
    uint64_t count = editor_text_newline_count(text) + 1;
    if (index >= count)
        return false;
    uint64_t start = 0;
    if (index > 0) {
        uint64_t newline;
        if (!find_newline(text, text->root, 0, index, &newline))
            return false;
        start = newline + 1;
    }
    uint64_t end = total;
    if ((uint64_t)index + 1 < count) {
        uint64_t newline;
        if (!find_newline(text, text->root, 0, (uint64_t)index + 1, &newline))
            return false;
        end = newline + 1;
    }
    if (!line)
        return true;
    line->start = start;
    line->end = end;
    uint64_t length = end - start;
    if ((uint64_t)index + 1 < count) {
        if (length >= 2 && text_byte(text, end - 2) == '\r')
            length -= 2;
        else if (length >= 1)
            length -= 1;
    }
    line->length = length;
    return true;
}

bool editor_text_locate(const struct editor_text *text, uint64_t offset,
                        uint32_t *line, uint64_t *column)
{
    if (!text || offset > editor_text_length(text))
        return false;
    uint64_t before = count_newlines(text, text->root, 0, offset);
    uint64_t start = 0;
    if (before > 0) {
        uint64_t newline;
        if (!find_newline(text, text->root, 0, before, &newline))
            return false;
        start = newline + 1;
    }
    if (line)
        *line = before > UINT32_MAX ? UINT32_MAX : (uint32_t)before;
    if (column)
        *column = offset - start;
    return true;
}
