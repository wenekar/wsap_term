#define _GNU_SOURCE

#include "editor/encoding.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The byte order marks, in the byte order of the file. */
static const char bom_utf8[] = "\xEF\xBB\xBF";
static const char bom_utf16_le[] = "\xFF\xFE";
static const char bom_utf16_be[] = "\xFE\xFF";

/* The printable characters of Windows-1252 in the range 0x80 to 0x9F. A zero
 * entry keeps the Latin-1 code point, which also keeps the byte on a save. */
static const uint16_t windows_1252_high[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
};

const char *editor_encoding_name(enum editor_encoding encoding)
{
    switch (encoding) {
    case EDITOR_ENCODING_UTF16_LE:
        return "UTF-16LE";
    case EDITOR_ENCODING_UTF16_BE:
        return "UTF-16BE";
    case EDITOR_ENCODING_LATIN1:
        return "ISO-8859-1";
    case EDITOR_ENCODING_WINDOWS_1252:
        return "Windows-1252";
    case EDITOR_ENCODING_UTF8:
        break;
    }
    return "UTF-8";
}

/* Reads one UTF-8 sequence. Returns its byte count, or 0 when the sequence is
 * not valid. */
static size_t utf8_decode_one(const char *bytes, size_t length, uint32_t *code)
{
    const unsigned char *b = (const unsigned char *)bytes;
    if (length == 0)
        return 0;
    uint32_t lead = b[0];
    if (lead < 0x80) {
        *code = lead;
        return 1;
    }
    size_t count;
    uint32_t value;
    uint32_t minimum;
    if ((lead & 0xE0) == 0xC0) {
        count = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        count = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        count = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (length < count)
        return 0;
    for (size_t i = 1; i < count; i++) {
        if ((b[i] & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (b[i] & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        return 0;
    *code = value;
    return count;
}

/* Writes one code point as UTF-8. Returns the byte count. */
static size_t utf8_encode_one(char *out, uint32_t code)
{
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (code >> 18));
    out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[3] = (char)(0x80 | (code & 0x3F));
    return 4;
}

bool editor_encoding_utf8_valid(const char *bytes, size_t length)
{
    if (!bytes)
        return length == 0;
    size_t offset = 0;
    while (offset < length) {
        uint32_t code;
        size_t used = utf8_decode_one(bytes + offset, length - offset, &code);
        if (used == 0)
            return false;
        offset += used;
    }
    return true;
}

static bool has_prefix(const char *bytes, size_t length, const char *prefix,
                       size_t prefix_length)
{
    return length >= prefix_length && memcmp(bytes, prefix, prefix_length) == 0;
}

/* Counts the NUL bytes in the last position of a pair. UTF-16 text that is
 * mostly ASCII has a NUL in most odd positions. */
static bool looks_like_utf16_le(const char *bytes, size_t length, size_t skip)
{
    size_t zero = 0;
    size_t pairs = 0;
    for (size_t i = skip + 1; i < length; i += 2) {
        pairs++;
        if (bytes[i] == '\0')
            zero++;
    }
    return pairs > 0 && zero * 2 > pairs;
}

static bool looks_like_utf16_be(const char *bytes, size_t length, size_t skip)
{
    size_t zero = 0;
    size_t pairs = 0;
    for (size_t i = skip; i + 1 < length; i += 2) {
        pairs++;
        if (bytes[i] == '\0')
            zero++;
    }
    return pairs > 0 && zero * 2 > pairs;
}

struct editor_encoding_info editor_encoding_detect(const char *bytes, size_t length)
{
    struct editor_encoding_info info = {EDITOR_ENCODING_UTF8, false};
    if (!bytes || length == 0)
        return info;
    if (has_prefix(bytes, length, bom_utf8, sizeof(bom_utf8) - 1)) {
        info.bom = true;
        return info;
    }
    if (has_prefix(bytes, length, bom_utf16_le, sizeof(bom_utf16_le) - 1)) {
        info.encoding = EDITOR_ENCODING_UTF16_LE;
        info.bom = true;
        return info;
    }
    if (has_prefix(bytes, length, bom_utf16_be, sizeof(bom_utf16_be) - 1)) {
        info.encoding = EDITOR_ENCODING_UTF16_BE;
        info.bom = true;
        return info;
    }
    if (looks_like_utf16_le(bytes, length, 0)) {
        info.encoding = EDITOR_ENCODING_UTF16_LE;
        return info;
    }
    if (looks_like_utf16_be(bytes, length, 0)) {
        info.encoding = EDITOR_ENCODING_UTF16_BE;
        return info;
    }
    if (editor_encoding_utf8_valid(bytes, length))
        return info;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        if (byte >= 0x80 && byte <= 0x9F &&
            windows_1252_high[byte - 0x80] != 0) {
            info.encoding = EDITOR_ENCODING_WINDOWS_1252;
            return info;
        }
    }
    info.encoding = EDITOR_ENCODING_LATIN1;
    return info;
}

/* A byte buffer that grows as the conversion adds bytes. */
struct byte_buffer {
    char *bytes;
    size_t length;
    size_t capacity;
};

static bool buffer_reserve(struct byte_buffer *buffer, size_t extra)
{
    if (buffer->length + extra + 1 <= buffer->capacity)
        return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < buffer->length + extra + 1) {
        if (capacity > SIZE_MAX / 2) {
            errno = ENOMEM;
            return false;
        }
        capacity *= 2;
    }
    char *grown = realloc(buffer->bytes, capacity);
    if (!grown) {
        errno = ENOMEM;
        return false;
    }
    buffer->bytes = grown;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_put(struct byte_buffer *buffer, uint32_t code)
{
    if (!buffer_reserve(buffer, 4))
        return false;
    buffer->length += utf8_encode_one(buffer->bytes + buffer->length, code);
    return true;
}

/* Reads the code point at `offset` of a UTF-16 buffer. A surrogate that has no
 * pair becomes U+FFFD. Returns the byte count that it used. */
static size_t utf16_read(const unsigned char *bytes, size_t length, size_t offset,
                         bool little, uint32_t *code)
{
    uint32_t unit = little ? (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1] << 8)
                           : ((uint32_t)bytes[offset] << 8) | (uint32_t)bytes[offset + 1];
    if (unit >= 0xD800 && unit <= 0xDBFF && offset + 3 < length) {
        uint32_t low = little
                           ? (uint32_t)bytes[offset + 2] |
                                 ((uint32_t)bytes[offset + 3] << 8)
                           : ((uint32_t)bytes[offset + 2] << 8) |
                                 (uint32_t)bytes[offset + 3];
        if (low >= 0xDC00 && low <= 0xDFFF) {
            *code = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            return 4;
        }
    }
    if (unit >= 0xD800 && unit <= 0xDFFF)
        *code = 0xFFFD;
    else
        *code = unit;
    return 2;
}

static char *decode_utf16(const char *bytes, size_t length, bool little,
                          size_t *out_length)
{
    if (length % 2 != 0) {
        errno = EINVAL;
        return NULL;
    }
    struct byte_buffer buffer = {0};
    const unsigned char *in = (const unsigned char *)bytes;
    for (size_t offset = 0; offset + 1 < length;) {
        uint32_t code;
        offset += utf16_read(in, length, offset, little, &code);
        if (!buffer_put(&buffer, code)) {
            free(buffer.bytes);
            return NULL;
        }
    }
    if (!buffer.bytes) {
        buffer.bytes = malloc(1);
        if (!buffer.bytes)
            return NULL;
    }
    buffer.bytes[buffer.length] = '\0';
    if (out_length)
        *out_length = buffer.length;
    return buffer.bytes;
}

static char *decode_single_byte(const char *bytes, size_t length,
                                enum editor_encoding encoding, size_t *out_length)
{
    struct byte_buffer buffer = {0};
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)bytes[i];
        uint32_t code = byte;
        if (encoding == EDITOR_ENCODING_WINDOWS_1252 && byte >= 0x80 && byte <= 0x9F) {
            uint16_t mapped = windows_1252_high[byte - 0x80];
            if (mapped != 0)
                code = mapped;
        }
        if (!buffer_put(&buffer, code)) {
            free(buffer.bytes);
            return NULL;
        }
    }
    if (!buffer.bytes) {
        buffer.bytes = malloc(1);
        if (!buffer.bytes)
            return NULL;
    }
    buffer.bytes[buffer.length] = '\0';
    if (out_length)
        *out_length = buffer.length;
    return buffer.bytes;
}

static char *decode_utf8(const char *bytes, size_t length, size_t *out_length)
{
    struct byte_buffer buffer = {0};
    for (size_t offset = 0; offset < length;) {
        uint32_t code;
        size_t used = utf8_decode_one(bytes + offset, length - offset, &code);
        if (used == 0) {
            code = 0xFFFD;
            used = 1;
        }
        if (!buffer_put(&buffer, code)) {
            free(buffer.bytes);
            return NULL;
        }
        offset += used;
    }
    if (!buffer.bytes) {
        buffer.bytes = malloc(1);
        if (!buffer.bytes)
            return NULL;
    }
    buffer.bytes[buffer.length] = '\0';
    if (out_length)
        *out_length = buffer.length;
    return buffer.bytes;
}

char *editor_encoding_decode(const char *bytes, size_t length,
                             struct editor_encoding_info info, size_t *out_length)
{
    if (!bytes && length > 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!bytes)
        bytes = "";
    if (info.bom) {
        if (info.encoding == EDITOR_ENCODING_UTF8 &&
            has_prefix(bytes, length, bom_utf8, sizeof(bom_utf8) - 1)) {
            bytes += sizeof(bom_utf8) - 1;
            length -= sizeof(bom_utf8) - 1;
        } else if (info.encoding == EDITOR_ENCODING_UTF16_LE &&
                   has_prefix(bytes, length, bom_utf16_le, sizeof(bom_utf16_le) - 1)) {
            bytes += sizeof(bom_utf16_le) - 1;
            length -= sizeof(bom_utf16_le) - 1;
        } else if (info.encoding == EDITOR_ENCODING_UTF16_BE &&
                   has_prefix(bytes, length, bom_utf16_be, sizeof(bom_utf16_be) - 1)) {
            bytes += sizeof(bom_utf16_be) - 1;
            length -= sizeof(bom_utf16_be) - 1;
        }
    }
    switch (info.encoding) {
    case EDITOR_ENCODING_UTF16_LE:
        return decode_utf16(bytes, length, true, out_length);
    case EDITOR_ENCODING_UTF16_BE:
        return decode_utf16(bytes, length, false, out_length);
    case EDITOR_ENCODING_LATIN1:
    case EDITOR_ENCODING_WINDOWS_1252:
        return decode_single_byte(bytes, length, info.encoding, out_length);
    case EDITOR_ENCODING_UTF8:
        break;
    }
    return decode_utf8(bytes, length, out_length);
}

/* Writes UTF-16 in one byte order. A code point that is UTF-16 too wide becomes
 * U+FFFD. */
static bool encode_utf16(struct byte_buffer *buffer, uint32_t code, bool little)
{
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        code = 0xFFFD;
    if (!buffer_reserve(buffer, 4))
        return false;
    if (code >= 0x10000) {
        uint32_t value = code - 0x10000;
        uint32_t units[2] = {0xD800 + (value >> 10), 0xDC00 + (value & 0x3FF)};
        for (int i = 0; i < 2; i++) {
            unsigned char high = (unsigned char)(units[i] >> 8);
            unsigned char low = (unsigned char)(units[i] & 0xFF);
            buffer->bytes[buffer->length++] = (char)(little ? low : high);
            buffer->bytes[buffer->length++] = (char)(little ? high : low);
        }
        return true;
    }
    buffer->bytes[buffer->length++] = (char)(little ? (code & 0xFF) : (code >> 8));
    buffer->bytes[buffer->length++] = (char)(little ? (code >> 8) : (code & 0xFF));
    return true;
}

/* The single-byte code of a code point. A code point that the encoding does
 * not hold becomes '?'. */
static unsigned char single_byte_of(uint32_t code, enum editor_encoding encoding)
{
    if (code < 0x80)
        return (unsigned char)code;
    if (code <= 0xFF) {
        if (encoding != EDITOR_ENCODING_WINDOWS_1252 || code < 0x80 || code > 0x9F)
            return (unsigned char)code;
    }
    if (encoding == EDITOR_ENCODING_WINDOWS_1252) {
        for (size_t i = 0; i < 32; i++) {
            if (windows_1252_high[i] != 0 && windows_1252_high[i] == code)
                return (unsigned char)(0x80 + i);
        }
    }
    return '?';
}

char *editor_encoding_encode(const char *utf8, size_t length,
                             struct editor_encoding_info info, size_t *out_length)
{
    if (!utf8 && length > 0) {
        errno = EINVAL;
        return NULL;
    }
    if (!utf8)
        utf8 = "";
    struct byte_buffer buffer = {0};
    size_t prefix = 0;
    if (info.bom) {
        if (info.encoding == EDITOR_ENCODING_UTF8)
            prefix = sizeof(bom_utf8) - 1;
        else if (info.encoding == EDITOR_ENCODING_UTF16_LE)
            prefix = sizeof(bom_utf16_le) - 1;
        else if (info.encoding == EDITOR_ENCODING_UTF16_BE)
            prefix = sizeof(bom_utf16_be) - 1;
    }
    if (prefix > 0) {
        if (!buffer_reserve(&buffer, prefix)) {
            free(buffer.bytes);
            return NULL;
        }
        const char *mark = info.encoding == EDITOR_ENCODING_UTF8 ? bom_utf8
                            : info.encoding == EDITOR_ENCODING_UTF16_LE ? bom_utf16_le
                                                                       : bom_utf16_be;
        memcpy(buffer.bytes, mark, prefix);
        buffer.length = prefix;
    }
    for (size_t offset = 0; offset < length;) {
        uint32_t code = 0xFFFD;
        size_t used = utf8_decode_one(utf8 + offset, length - offset, &code);
        if (used == 0)
            used = 1;
        offset += used;
        bool ok;
        switch (info.encoding) {
        case EDITOR_ENCODING_UTF16_LE:
            ok = encode_utf16(&buffer, code, true);
            break;
        case EDITOR_ENCODING_UTF16_BE:
            ok = encode_utf16(&buffer, code, false);
            break;
        case EDITOR_ENCODING_LATIN1:
        case EDITOR_ENCODING_WINDOWS_1252:
            ok = buffer_reserve(&buffer, 1);
            if (ok)
                buffer.bytes[buffer.length++] =
                    (char)single_byte_of(code, info.encoding);
            break;
        case EDITOR_ENCODING_UTF8:
        default:
            ok = buffer_put(&buffer, code);
            break;
        }
        if (!ok) {
            free(buffer.bytes);
            return NULL;
        }
    }
    if (!buffer.bytes) {
        buffer.bytes = malloc(1);
        if (!buffer.bytes)
            return NULL;
    }
    buffer.bytes[buffer.length] = '\0';
    if (out_length)
        *out_length = buffer.length;
    return buffer.bytes;
}
