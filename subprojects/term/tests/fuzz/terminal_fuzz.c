#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Keep each case small. The fuzzer must make progress on hostile input. */
enum { INPUT_MAX = 4096, OPERATIONS_MAX = 32, CLUSTER_CAP = 32 };

struct effects {
    uint64_t replies;
    size_t reply_bytes;
    uint64_t events;
    unsigned event_count;
};

static uint64_t hash_bytes(uint64_t hash, const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++)
        hash = (hash ^ (unsigned char)bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static void reply(void *userdata, const char *bytes, size_t len)
{
    struct effects *effects = userdata;
    effects->replies = hash_bytes(effects->replies, bytes, len);
    effects->reply_bytes += len;
}

static void event(struct effects *effects, char kind, const char *bytes, size_t len)
{
    effects->events = hash_bytes(effects->events, &kind, 1);
    effects->events = hash_bytes(effects->events, bytes, len);
    effects->events = (effects->events ^ len) * UINT64_C(1099511628211);
    effects->event_count++;
}

static void title(void *userdata, const char *text)
{
    event(userdata, 't', text, strlen(text));
}

static void clipboard(void *userdata, const char *selection, const char *bytes, size_t len)
{
    event(userdata, 's', selection, strlen(selection));
    event(userdata, 'c', bytes, len);
}

static void notify(void *userdata, const char *heading, const char *body)
{
    event(userdata, 'n', heading, strlen(heading));
    event(userdata, 'b', body, strlen(body));
}

static void bell(void *userdata)
{
    event(userdata, 'a', "", 0);
}

static struct wsap_vtcore *new_terminal(struct effects *effects)
{
    struct wsap_vtcore_options options = {
        .size = { .x = 12, .y = 6 },
        .scrollback = 8,
        .callbacks = {
            .ud = effects,
            .reply = reply,
            .title = title,
            .clipboard = clipboard,
            .notify = notify,
            .bell = bell,
        },
    };
    return wsap_vtcore_new(&options);
}

static void same_snapshot(const struct wsap_vtcore_snapshot *a,
                          const struct wsap_vtcore_snapshot *b)
{
    const struct wsap_vtcore_view *va = wsap_vtcore_snapshot_view(a);
    const struct wsap_vtcore_view *vb = wsap_vtcore_snapshot_view(b);
    assert(va->size.x == vb->size.x && va->size.y == vb->size.y);
    assert(va->cursor.pos.x == vb->cursor.pos.x && va->cursor.pos.y == vb->cursor.pos.y);
    assert(va->cursor.style == vb->cursor.style);
    assert(va->cursor.visible == vb->cursor.visible && va->cursor.blink == vb->cursor.blink);
    assert(va->scroll_offset == vb->scroll_offset && va->reverse_video == vb->reverse_video);
    assert(!memcmp(va->palette, vb->palette,
                   WSAP_VTCORE_PALETTE_SIZE * sizeof(*va->palette)));
    for (int y = 0; y < va->size.y; y++) {
        const struct wsap_vtcore_cell *ra = wsap_vtcore_snapshot_row(a, y);
        const struct wsap_vtcore_cell *rb = wsap_vtcore_snapshot_row(b, y);
        for (int x = 0; x < va->size.x; x++) {
            assert(ra[x].fg == rb[x].fg && ra[x].bg == rb[x].bg);
            assert(ra[x].attrs == rb[x].attrs && ra[x].width == rb[x].width);
            uint32_t ca[CLUSTER_CAP], cb[CLUSTER_CAP];
            int na = wsap_vtcore_snapshot_cluster(a, ra[x].text, ca, CLUSTER_CAP);
            int nb = wsap_vtcore_snapshot_cluster(b, rb[x].text, cb, CLUSTER_CAP);
            assert(na == nb && na >= 0 && na <= CLUSTER_CAP);
            assert(!memcmp(ca, cb, (size_t)na * sizeof(*ca)));
        }
    }
}

static void fuzz_stream(const uint8_t *data, size_t size)
{
    struct effects ea = {0}, eb = {0};
    struct wsap_vtcore *a = new_terminal(&ea);
    struct wsap_vtcore *b = new_terminal(&eb);
    struct wsap_vtcore_snapshot *sa = wsap_vtcore_snapshot_new();
    struct wsap_vtcore_snapshot *sb = wsap_vtcore_snapshot_new();
    if (!a || !b || !sa || !sb)
        goto done;

    assert(wsap_vtcore_feed(a, (const char *)data, size, 0));
    for (size_t i = 0; i < size; i++)
        assert(wsap_vtcore_feed(b, (const char *)data + i, 1, 0));
    assert(ea.replies == eb.replies && ea.reply_bytes == eb.reply_bytes);
    assert(ea.events == eb.events && ea.event_count == eb.event_count);
    if (!wsap_vtcore_snapshot_update(sa, a) || !wsap_vtcore_snapshot_update(sb, b))
        goto done;
    same_snapshot(sa, sb);
    char *ha = wsap_vtcore_history_text(a, 32);
    char *hb = wsap_vtcore_history_text(b, 32);
    if (ha && hb)
        assert(!strcmp(ha, hb));
    wsap_vtcore_text_free(ha);
    wsap_vtcore_text_free(hb);

done:
    wsap_vtcore_snapshot_free(sa);
    wsap_vtcore_snapshot_free(sb);
    wsap_vtcore_free(a);
    wsap_vtcore_free(b);
}

static unsigned take(const uint8_t *data, size_t size, size_t *at)
{
    return *at < size ? data[(*at)++] : 0;
}

static void fuzz_operations(const uint8_t *data, size_t size)
{
    struct effects effects = {0};
    struct wsap_vtcore *term = new_terminal(&effects);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    if (!term || !snapshot)
        goto done;

    size_t at = 1;
    for (unsigned op = 0; at < size && op < OPERATIONS_MAX; op++) {
        switch (take(data, size, &at) % 8) {
        case 0: {
            size_t n = take(data, size, &at) % 65;
            if (n > size - at)
                n = size - at;
            (void)wsap_vtcore_feed(term, (const char *)data + at, n, 0);
            at += n;
            break;
        }
        case 1: {
            struct wsap_vtcore_vector2i dimensions = {
                .x = 1 + (int)(take(data, size, &at) % 12),
                .y = 1 + (int)(take(data, size, &at) % 6),
            };
            (void)wsap_vtcore_resize(term, dimensions);
            break;
        }
        case 2:
            wsap_vtcore_scroll(term, (int)(take(data, size, &at) % 33) - 16);
            break;
        case 3: {
            struct wsap_vtcore_vector2i cell = {
                .x = (int)(take(data, size, &at) % 16) - 2,
                .y = (int)(take(data, size, &at) % 10) - 2,
            };
            (void)wsap_vtcore_select_start(term, cell, false,
                (enum wsap_vtcore_select_unit)(take(data, size, &at) % 4));
            cell.x++;
            (void)wsap_vtcore_select_extend(term, cell, true);
            wsap_vtcore_text_free(wsap_vtcore_selection_text(term));
            break;
        }
        case 4:
            if (wsap_vtcore_snapshot_update(snapshot, term)) {
                uint32_t cps[1024];
                uint32_t token = WSAP_VTCORE_TEXT_CLUSTER | take(data, size, &at);
                (void)wsap_vtcore_snapshot_cluster(snapshot, token, cps, 1024);
            }
            break;
        case 5:
            (void)wsap_vtcore_set_scrollback_limit(term, (int)(take(data, size, &at) % 9));
            break;
        case 6: {
            struct wsap_vtcore_modes modes = wsap_vtcore_modes(term);
            struct wsap_vtcore_key_event key = {
                .action = (enum wsap_vtcore_key_action)(take(data, size, &at) % 5),
                .key = (uint16_t)take(data, size, &at),
                .mods = take(data, size, &at),
                .text = take(data, size, &at),
            };
            char output[128];
            (void)wsap_vtcore_input_encode_key(NULL, &key, &modes, output,
                                             take(data, size, &at) % sizeof(output));
            break;
        }
        default:
            wsap_vtcore_select_clear(term);
            wsap_vtcore_scroll_to(term, 0);
            break;
        }
    }

done:
    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > INPUT_MAX)
        size = INPUT_MAX;
    if (size && data[0] == 0xff)
        fuzz_operations(data, size);
    else
        fuzz_stream(data, size);
    return 0;
}
