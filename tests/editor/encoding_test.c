#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "editor/document.h"
#include "editor/encoding.h"

static char *make_directory(void)
{
    static char path[] = "/tmp/editor-encoding-XXXXXX";
    char *directory = mkdtemp(path);
    assert(directory);
    return directory;
}

static void write_file(const char *path, const char *bytes, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, bytes, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static char *read_file(const char *path, size_t *length)
{
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    char *bytes = malloc((size_t)st.st_size + 1);
    assert(bytes);
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, bytes + done, (size_t)st.st_size - done);
        assert(got > 0);
        done += (size_t)got;
    }
    bytes[done] = '\0';
    assert(close(fd) == 0);
    if (length)
        *length = done;
    return bytes;
}

static void test_utf8_bom(void)
{
    const char bytes[] = "\xEF\xBB\xBFhello\n";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    assert(info.encoding == EDITOR_ENCODING_UTF8);
    assert(info.bom);
    size_t length = 0;
    char *text = editor_encoding_decode(bytes, sizeof(bytes) - 1, info, &length);
    assert(text);
    assert(length == 6);
    assert(memcmp(text, "hello\n", 6) == 0);
    free(text);
}

/* A UTF-16LE file with a byte order mark opens as UTF-8 and saves back to the
 * same bytes. */
static void test_utf16le_bom_round_trip(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/utf16le.txt", directory);
    const char bytes[] = "\xFF\xFEh\0i\0\n\0\xAC\x20";
    write_file(path, bytes, sizeof(bytes) - 1);

    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    struct editor_encoding_info info = editor_document_encoding(document);
    assert(info.encoding == EDITOR_ENCODING_UTF16_LE);
    assert(info.bom);

    const struct editor_text *text = editor_document_text(document);
    assert(editor_text_length(text) == 6);
    char buffer[8] = {0};
    assert(editor_text_copy(text, 0, 6, buffer) == 6);
    assert(memcmp(buffer, "hi\n\xE2\x82\xAC", 6) == 0);

    /* A change and a save keep the encoding and the mark. */
    assert(editor_document_insert(document, 1, "X", 1));
    assert(editor_document_save(document));
    size_t length = 0;
    char *saved = read_file(path, &length);
    const char expected[] = "\xFF\xFEh\0X\0i\0\n\0\xAC\x20";
    assert(length == sizeof(expected) - 1);
    assert(memcmp(saved, expected, length) == 0);
    free(saved);
    editor_document_destroy(document);
}

static void test_utf16be_bom(void)
{
    const char bytes[] = "\xFE\xFF\0h\0i";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    assert(info.encoding == EDITOR_ENCODING_UTF16_BE);
    size_t length = 0;
    char *text = editor_encoding_decode(bytes, sizeof(bytes) - 1, info, &length);
    assert(text && length == 2 && memcmp(text, "hi", 2) == 0);
    free(text);

    size_t written = 0;
    char *encoded = editor_encoding_encode("hi", 2, info, &written);
    assert(encoded && written == sizeof(bytes) - 1);
    assert(memcmp(encoded, bytes, written) == 0);
    free(encoded);
}

/* 0x93 is a left double quote in Windows-1252. It is a control code in
 * Latin-1. */
static void test_windows_1252_byte(void)
{
    const char bytes[] = "a\x93q\x94 \x80";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    assert(info.encoding == EDITOR_ENCODING_WINDOWS_1252);
    assert(!info.bom);
    size_t length = 0;
    char *text = editor_encoding_decode(bytes, sizeof(bytes) - 1, info, &length);
    assert(text);
    const char expected[] = "a\xE2\x80\x9Cq\xE2\x80\x9D \xE2\x82\xAC";
    assert(length == sizeof(expected) - 1);
    assert(memcmp(text, expected, length) == 0);
    free(text);
}

static void test_latin1_byte(void)
{
    const char bytes[] = "caf\xE9";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    assert(info.encoding == EDITOR_ENCODING_LATIN1);
    size_t length = 0;
    char *text = editor_encoding_decode(bytes, sizeof(bytes) - 1, info, &length);
    assert(text);
    const char expected[] = "caf\xC3\xA9";
    assert(length == sizeof(expected) - 1);
    assert(memcmp(text, expected, length) == 0);
    free(text);
}

static void test_latin1_no_bom_on_save(const char *directory)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/latin1.txt", directory);
    write_file(path, "caf\xE9\n", 5);
    struct editor_document *document = editor_document_create();
    assert(document);
    assert(editor_document_open(document, path));
    assert(editor_document_encoding(document).encoding == EDITOR_ENCODING_LATIN1);
    assert(editor_document_save(document));
    size_t length = 0;
    char *saved = read_file(path, &length);
    assert(length == 5);
    assert(memcmp(saved, "caf\xE9\n", 5) == 0);
    free(saved);
    editor_document_destroy(document);
}

/* A plain ASCII file stays UTF-8 and gains no mark. */
static void test_utf8_default(void)
{
    const char bytes[] = "plain\n";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    assert(info.encoding == EDITOR_ENCODING_UTF8);
    assert(!info.bom);
    assert(strcmp(editor_encoding_name(info.encoding), "UTF-8") == 0);
}

/* A byte that is not ASCII or UTF-8 selects UTF-16 when the text has a NUL in
 * every other position. */
static void test_utf16le_without_bom(void)
{
    const char bytes[] = "h\0i\0";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    assert(info.encoding == EDITOR_ENCODING_UTF16_LE);
    assert(!info.bom);
    size_t length = 0;
    char *text = editor_encoding_decode(bytes, sizeof(bytes) - 1, info, &length);
    assert(text && length == 2 && memcmp(text, "hi", 2) == 0);
    free(text);
}

/* A surrogate that has no pair becomes the replacement character. */
static void test_utf16_lone_surrogate(void)
{
    const char bytes[] = "\xFF\xFE\0\xD8x\0";
    struct editor_encoding_info info = editor_encoding_detect(bytes, sizeof(bytes) - 1);
    size_t length = 0;
    char *text = editor_encoding_decode(bytes, sizeof(bytes) - 1, info, &length);
    assert(text);
    const char expected[] = "\xEF\xBF\xBDx";
    assert(length == sizeof(expected) - 1);
    assert(memcmp(text, expected, length) == 0);
    free(text);
}

/* A code point that Latin-1 cannot hold becomes '?'. */
static void test_latin1_lossy_encode(void)
{
    struct editor_encoding_info info = {EDITOR_ENCODING_LATIN1, false};
    size_t length = 0;
    char *encoded = editor_encoding_encode("\xE2\x82\xAC", 3, info, &length);
    assert(encoded && length == 1 && encoded[0] == '?');
    free(encoded);
}

static void test_utf8_validity(void)
{
    assert(editor_encoding_utf8_valid("ok", 2));
    assert(editor_encoding_utf8_valid("\xE2\x82\xAC", 3));
    assert(!editor_encoding_utf8_valid("\xFF", 1));
    assert(!editor_encoding_utf8_valid("\xE2\x82", 2));
    assert(!editor_encoding_utf8_valid("\xC0\xAF", 2));
}

int main(void)
{
    char *directory = make_directory();
    test_utf8_bom();
    test_utf16le_bom_round_trip(directory);
    test_utf16be_bom();
    test_windows_1252_byte();
    test_latin1_byte();
    test_latin1_no_bom_on_save(directory);
    test_utf8_default();
    test_utf16le_without_bom();
    test_utf16_lone_surrogate();
    test_latin1_lossy_encode();
    test_utf8_validity();
    printf("editor encoding tests passed\n");
    return 0;
}
