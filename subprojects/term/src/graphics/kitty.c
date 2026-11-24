#include "graphics.h"
#include "internal.h"
#include "osc/osc.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KITTY_CHUNK_MAX 4096

struct kitty_control {
    struct wsap_vtcore_graphics command;
    char action, medium, feature;
    uint32_t format, quiet, more;
    bool continuation, quiet_set, unsupported, both_ids;
    /* The control data, for a feature query reply. */
    const char *control;
    size_t control_len;
};

struct wsap_vtcore_kitty_upload {
    struct kitty_control control;
    char *data;
    size_t len, cap;
};

void wsap_vtcore_kitty_abort(struct wsap_vtcore *t)
{
    if (t->kitty_upload) {
        free(t->kitty_upload->data);
        free(t->kitty_upload);
        t->kitty_upload = NULL;
    }
}

static bool kitty_uint(const char *p, const char *end, uint32_t *out)
{
    uint32_t n = 0;
    if (p == end)
        return false;
    while (p < end) {
        unsigned digit = (unsigned char)*p++ - (unsigned)'0';
        if (digit > 9 || n > (UINT32_MAX - digit) / 10)
            return false;
        n = n * 10 + digit;
    }
    *out = n;
    return true;
}

static bool kitty_parse(struct kitty_control *k, const char *p, const char *end)
{
    struct wsap_vtcore_graphics *g = &k->command;
    /* Keys whose meaning depends on the action wait here. */
    uint32_t raw_c = 0, raw_r = 0, raw_s = 0, raw_v = 0, raw_z = 0;
    bool has_c = false, has_r = false, has_s = false, has_v = false, has_z = false, z_neg = false;
    uint64_t seen = 0;
    while (p < end) {
        unsigned key = (unsigned char)*p++;
        unsigned index;
        if (key >= 'a' && key <= 'z') index = key - 'a';
        else if (key >= 'A' && key <= 'Z') index = key - 'A' + 26;
        else return false;
        uint64_t bit = UINT64_C(1) << index;
        if (seen & bit || end - p < 2 || *p++ != '=')
            return false;
        seen |= bit;
        const char *v = p;
        while (p < end && *p != ',') p++;
        if (key == 'a' || key == 't' || key == 'd' || key == 'o') {
            if (p - v != 1) return false;
            if (key == 'a') k->action = *v;
            else if (key == 't') k->medium = *v;
            else if (key == 'd') g->delete_mode = *v;
            else if (*v == 'z') g->zlib = true;
            else k->unsupported = true;
        } else {
            uint32_t n;
            int *field = NULL;
            bool negative = key == 'z' && v < p && *v == '-';
            if (key == 'e') {
                if (p - v != 1) return false;
                k->feature = *v;
            } else if (!kitty_uint(v + negative, p, &n)) {
                return false;
            } else switch (key) {
            case 'f': k->format = n; break;
            case 'q': k->quiet_set = true; k->quiet = n; break;
            case 'm': k->more = n; break;
            case 'i': g->image_id = n; break;
            case 'I': g->image_number = n; break;
            case 'p': g->placement_id = n; break;
            case 'S': g->uncompressed_size = n; break;
            case 'x': field = &g->source_pos.x; break;
            case 'y': field = &g->source_pos.y; break;
            case 'w': field = &g->source_size.x; break;
            case 'h': field = &g->source_size.y; break;
            case 'X': field = &g->offset.x; break;
            case 'Y': field = &g->offset.y; break;
            case 'c': has_c = true; raw_c = n; break;
            case 'r': has_r = true; raw_r = n; break;
            case 's': has_s = true; raw_s = n; break;
            case 'v': has_v = true; raw_v = n; break;
            case 'C':
                if (n > 1) return false;
                g->keep_cursor = n == 1;
                break;
            case 'z':
                if (n > (uint32_t)INT32_MAX + negative) return false;
                has_z = true; z_neg = negative; raw_z = n;
                break;
            case 'N': break;
            default: k->unsupported = true; break;
            }
            if (field) {
                if (n > INT_MAX) return false;
                *field = (int)n;
            }
        }
        if (key != 'm' && key != 'q') k->continuation = false;
        if (p < end && ++p == end) return false;
    }
    k->both_ids = (seen & (UINT64_C(1) << ('i' - 'a'))) &&
                  (seen & (UINT64_C(1) << ('I' - 'A' + 26)));
    bool control = k->action == 'a';
    bool frames = k->action == 'f';
    if (has_c && !control && !frames && raw_c > INT_MAX) return false;
    if (has_r && !control && raw_r > INT_MAX) return false;
    if (has_c) {
        if (control) g->frame = raw_c;
        else if (frames) g->frame_target = raw_c;
        else g->placement_size.x = (int)raw_c;
    }
    if (has_r) {
        if (control) g->frame_target = raw_r;
        else g->placement_size.y = (int)raw_r;
    }
    if (has_s) {
        if (k->action == 'a') g->animation = raw_s;
        else g->size.x = (int)raw_s;
    }
    if (has_v) {
        if (control) g->loops = raw_v;
        else g->size.y = (int)raw_v;
    }
    if (frames && has_r)
        k->unsupported = true;
    if (has_z) {
        int32_t value = (int32_t)(z_neg ? -(int64_t)raw_z : raw_z);
        if (control || frames) g->frame_gap = value;
        else g->z_index = value;
    }
    return k->quiet <= 2 && k->more <= 1 && !k->both_ids;
}

static void kitty_reply(struct wsap_vtcore *t, const struct kitty_control *k,
                        enum wsap_vtcore_graphics_result result)
{
    if (!k->command.image_id || k->quiet == 2 ||
        (k->quiet == 1 && result == WSAP_VTCORE_GRAPHICS_OK))
        return;
    const char *message;
    switch (result) {
    case WSAP_VTCORE_GRAPHICS_OK: message = "OK"; break;
    case WSAP_VTCORE_GRAPHICS_NOT_FOUND: message = "ENOENT:Image not found"; break;
    case WSAP_VTCORE_GRAPHICS_NO_SPACE: message = "ENOSPC:Image limit exceeded"; break;
    case WSAP_VTCORE_GRAPHICS_UNSUPPORTED: message = "ENOTSUP:Unsupported graphics command"; break;
    default: message = "EINVAL:Invalid graphics command"; break;
    }
    char response[160];
    int n = snprintf(response, sizeof response, "\033_Gi=%" PRIu32, k->command.image_id);
    if (k->command.image_number)
        n += snprintf(response + n, sizeof response - (size_t)n, ",I=%" PRIu32,
                      k->command.image_number);
    if (k->command.placement_id)
        n += snprintf(response + n, sizeof response - (size_t)n, ",p=%" PRIu32,
                      k->command.placement_id);
    snprintf(response + n, sizeof response - (size_t)n, ";%s\033\\", message);
    send_response(t, response);
}

/* A feature query reports the keys of the query that the action supports.
 * A missing `e` in the reply means that the action is unsupported. */
static void kitty_feature_reply(struct wsap_vtcore *t, const struct kitty_control *k)
{
    static const char actions[] = "qtTpd";
    char response[512];
    int n = snprintf(response, sizeof response, "\033_Gi=%" PRIu32, k->command.image_id);
    if (strchr(actions, k->feature) && k->feature)
        n += snprintf(response + n, sizeof response - (size_t)n, ",e=%c", k->feature);
    for (const char *p = k->control; p < k->control + k->control_len && n < 400;) {
        const char *end = memchr(p, ',', (size_t)(k->control + k->control_len - p));
        if (!end) end = k->control + k->control_len;
        const char *eq = memchr(p, '=', (size_t)(end - p));
        size_t len = (size_t)(end - p);
        if (eq == p + 1 && !strchr("aiImqe", eq[-1]) && strchr("tfsvxywhXYCrzoSN", eq[-1])) {
            const char *v = eq + 1;
            size_t vlen = (size_t)(end - v);
            bool ok = !((eq[-1] == 't' && (vlen != 1 || *v != 'd')) ||
                        (eq[-1] == 'f' && !((vlen == 2 && !memcmp(v, "24", 2)) ||
                                            (vlen == 2 && !memcmp(v, "32", 2)) ||
                                            (vlen == 3 && !memcmp(v, "100", 3)))) ||
                        (eq[-1] == 'o' && (vlen != 1 || *v != 'z')));
            if (ok)
                n += snprintf(response + n, sizeof response - (size_t)n, ",%.*s", (int)len, p);
        }
        p = end + 1;
    }
    snprintf(response + n, sizeof response - (size_t)n, ";OK\033\\");
    send_response(t, response);
}

static uint32_t kitty_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static enum wsap_vtcore_graphics_result kitty_image(struct wsap_vtcore *t, struct kitty_control *k,
                                                   const char *data, size_t len)
{
    struct wsap_vtcore_graphics *g = &k->command;
    g->format = (enum wsap_vtcore_graphics_format)k->format;
    g->data = (const uint8_t *)data;
    g->len = len;
    uint32_t w = (uint32_t)g->size.x, h = (uint32_t)g->size.y;
    if (g->format == WSAP_VTCORE_GRAPHICS_PNG) {
        if (g->zlib) {
            if (!len || !g->uncompressed_size) return WSAP_VTCORE_GRAPHICS_INVALID;
            if (g->uncompressed_size > WSAP_VTCORE_GRAPHICS_BYTES_MAX)
                return WSAP_VTCORE_GRAPHICS_NO_SPACE;
            g->size = (struct wsap_vtcore_vector2i){0};
            return wsap_vtcore_graphics_emit(t, g);
        }
        static const uint8_t png_header[] = "\x89" "PNG\r\n\x1a\n\0\0\0\x0d" "IHDR";
        if (len < 33 || memcmp(g->data, png_header, sizeof png_header - 1))
            return WSAP_VTCORE_GRAPHICS_INVALID;
        w = kitty_be32(g->data + 16);
        h = kitty_be32(g->data + 20);
    }
    if (!w || !h || w > WSAP_VTCORE_GRAPHICS_DIM_MAX || h > WSAP_VTCORE_GRAPHICS_DIM_MAX)
        return WSAP_VTCORE_GRAPHICS_INVALID;
    size_t pixels = (size_t)w * h;
    unsigned channels = g->format == WSAP_VTCORE_GRAPHICS_RGB ? 3 : 4;
    if (pixels > WSAP_VTCORE_GRAPHICS_BYTES_MAX / channels)
        return WSAP_VTCORE_GRAPHICS_NO_SPACE;
    if (g->zlib) {
        if (!len || (g->uncompressed_size && g->uncompressed_size != pixels * channels))
            return WSAP_VTCORE_GRAPHICS_INVALID;
        g->uncompressed_size = pixels * channels;
    } else if (g->format != WSAP_VTCORE_GRAPHICS_PNG && len != pixels * channels) {
        return WSAP_VTCORE_GRAPHICS_INVALID;
    }
    g->size = (struct wsap_vtcore_vector2i){(int)w, (int)h};
    return wsap_vtcore_graphics_emit(t, g);
}

static bool kitty_append(struct wsap_vtcore_kitty_upload *u, const char *data, size_t len)
{
    if (len > WSAP_VTCORE_GRAPHICS_BYTES_MAX - u->len)
        return false;
    size_t need = u->len + len;
    if (need > u->cap) {
        size_t cap = wsap_vtcore_grow_cap(u->cap ? u->cap : 4096, need);
        if (cap > WSAP_VTCORE_GRAPHICS_BYTES_MAX) cap = WSAP_VTCORE_GRAPHICS_BYTES_MAX;
        char *buf = realloc(u->data, cap);
        if (!buf) return false;
        u->data = buf;
        u->cap = cap;
    }
    if (len) memcpy(u->data + u->len, data, len);
    u->len = need;
    return true;
}

void wsap_vtcore_kitty_dispatch(struct wsap_vtcore *t, const char *buf, size_t len)
{
    if (!t->graphics_receive)
        return;
    const char *end = buf + len;
    const char *sep = memchr(buf, ';', len);
    const char *payload = sep ? sep + 1 : end;
    size_t encoded = (size_t)(end - payload);
    struct kitty_control k = {
        .action = 't', .medium = 'd', .format = 32, .continuation = true,
        .command.delete_mode = 'a', .control = buf, .control_len = len,
    };
    enum wsap_vtcore_graphics_result result = WSAP_VTCORE_GRAPHICS_INVALID;
    if (!kitty_parse(&k, buf, sep ? sep : end))
        goto done;
    struct wsap_vtcore_kitty_upload *u = t->kitty_upload;
    bool more = k.more;
    if (u) {
        if (k.continuation) {
            uint32_t quiet = k.quiet_set ? k.quiet : u->control.quiet;
            k = u->control;
            k.quiet = quiet;
        } else if (k.action == 'd') {
            wsap_vtcore_kitty_abort(t);
            u = NULL;
        } else {
            goto done;
        }
    }
    if (k.feature && k.action == 'q' && k.command.image_id) {
        /* A feature query ignores unknown keys and any payload. */
        kitty_feature_reply(t, &k);
        return;
    }
    if (k.unsupported || k.medium != 'd') {
        result = WSAP_VTCORE_GRAPHICS_UNSUPPORTED;
        goto done;
    }
    switch (k.action) {
    case 'q':
        if (!k.command.image_id && !k.command.image_number) goto done;
        k.command.action = WSAP_VTCORE_GRAPHICS_QUERY;
        break;
    case 't': k.command.action = WSAP_VTCORE_GRAPHICS_TRANSMIT; break;
    case 'T': k.command.action = WSAP_VTCORE_GRAPHICS_TRANSMIT_AND_PLACE; break;
    case 'p':
        k.command.action = WSAP_VTCORE_GRAPHICS_PLACE;
        if ((!k.command.image_id && !k.command.image_number) || encoded || more) goto done;
        result = wsap_vtcore_graphics_emit(t, &k.command);
        goto done;
    case 'f':
        k.command.action = WSAP_VTCORE_GRAPHICS_FRAME;
        if (!k.command.image_id && !k.command.image_number) goto done;
        break;
    case 'a':
        k.command.action = WSAP_VTCORE_GRAPHICS_ANIMATE;
        if ((!k.command.image_id && !k.command.image_number) || encoded || more)
            goto done;
        result = wsap_vtcore_graphics_emit(t, &k.command);
        goto done;
    case 'd':
        k.command.action = WSAP_VTCORE_GRAPHICS_DELETE;
        if (!k.command.image_id && !k.command.image_number &&
            (k.command.delete_mode == 'i' || k.command.delete_mode == 'I'))
            goto done;
        if (encoded || more) goto done;
        if (!strchr("aAiIcCpPqQrRxXyYzZfF", k.command.delete_mode)) {
            result = WSAP_VTCORE_GRAPHICS_UNSUPPORTED;
            goto done;
        }
        result = wsap_vtcore_graphics_emit(t, &k.command);
        goto done;
    default:
        result = WSAP_VTCORE_GRAPHICS_UNSUPPORTED;
        goto done;
    }
    if (k.format != 24 && k.format != 32 && k.format != 100) {
        result = WSAP_VTCORE_GRAPHICS_UNSUPPORTED;
        goto done;
    }
    char decoded[(KITTY_CHUNK_MAX / 4 + 1) * 3];
    size_t decoded_len;
    if (encoded > KITTY_CHUNK_MAX || (more && encoded && (encoded % 4 || end[-1] == '=')) ||
        !wsap_vtcore_b64_decode(payload, encoded, decoded, &decoded_len))
        goto done;
    if (more && !u) {
        u = calloc(1, sizeof(*u));
        if (!u) {
            result = WSAP_VTCORE_GRAPHICS_NO_SPACE;
            goto done;
        }
        t->kitty_upload = u;
    }
    if (u && !kitty_append(u, decoded, decoded_len)) {
        result = WSAP_VTCORE_GRAPHICS_NO_SPACE;
        goto done;
    }
    if (more) {
        u->control = k;
        return;
    }
    result = kitty_image(t, &k, u ? u->data : decoded, u ? u->len : decoded_len);

done:
    kitty_reply(t, &k, result);
    wsap_vtcore_kitty_abort(t);
}
