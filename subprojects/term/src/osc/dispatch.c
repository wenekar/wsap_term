#include "osc.h"
#include "../internal.h"
#include "../palette.h"
#include "../screen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define OSC_HEAP_INIT 4096
#define OSC_HEAP_KEEP (1u << 20)

size_t wsap_vtcore_osc_room(const struct wsap_vtcore *t)
{
    size_t cap = t->osc_heap ? t->osc_heap_cap : sizeof(t->osc_buf);
    return cap - 1 - (size_t)t->osc_len;
}

char *wsap_vtcore_osc_tail(struct wsap_vtcore *t)
{
    return (t->osc_heap ? t->osc_heap : t->osc_buf) + t->osc_len;
}

bool wsap_vtcore_osc_grow(struct wsap_vtcore *t)
{
    if (!t->osc_heap) {
        char *p = malloc(OSC_HEAP_INIT);
        if (!p) return false;
        memcpy(p, t->osc_buf, (size_t)t->osc_len);
        t->osc_heap = p;
        t->osc_heap_cap = OSC_HEAP_INIT;
        return true;
    }
    if (t->osc_heap_cap >= WSAP_VTCORE_OSC_MAX) return false;
    size_t cap = t->osc_heap_cap * 2;
    if (cap > WSAP_VTCORE_OSC_MAX) cap = WSAP_VTCORE_OSC_MAX;
    char *p = realloc(t->osc_heap, cap);
    if (!p) return false;
    t->osc_heap = p;
    t->osc_heap_cap = cap;
    return true;
}

void wsap_vtcore_osc_cleanup(struct wsap_vtcore *t)
{
    free(t->osc_heap);
    t->osc_heap = NULL;
    t->osc_heap_cap = 0;
    free(t->dcs_buf);
    t->dcs_buf = NULL;
    t->dcs_cap = 0;
    wsap_vtcore_osc8_cleanup(t);
    free(t->cwd);
    t->cwd = NULL;
    free(t->pointer_shape);
    t->pointer_shape = NULL;
}

/* Reply to a color query. An OSC 4 reply names the index it answers for;
 * index is negative for OSC 10, 11, and 12. */
static void send_color_query_reply(struct wsap_vtcore *t, int code, int index, uint32_t argb)
{
    char resp[64], prefix[16] = "";
    uint8_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
    if (index >= 0)
        snprintf(prefix, sizeof(prefix), "%d;", index);
    snprintf(resp, sizeof(resp),
             "\x1b]%d;%srgb:%02x%02x/%02x%02x/%02x%02x\x1b\\",
             code, prefix, r, r, g, g, b, b);
    send_response(t, resp);
}

static void osc_color_set_or_query(struct wsap_vtcore *t, int code, const char *params, int slot)
{
    if (!strcmp(params, "?")) {
        send_color_query_reply(t, code, -1, t->palette.colors[slot]);
        return;
    }
    uint32_t argb;
    if (wsap_vtcore_color_parse(params, &argb))
        wsap_vtcore_palette_set(&t->palette, slot, argb);
}

static long osc_next_index(const char **pp)
{
    char *end;
    long idx = strtol(*pp, &end, 10);
    if (end == *pp || (*end != ';' && *end != '\0'))
        return -1;
    *pp = end;
    if (**pp == ';') (*pp)++;
    return idx;
}

static void osc4_handle(struct wsap_vtcore *t, const char *params)
{
    const char *p = params;
    while (*p) {
        long idx = osc_next_index(&p);
        if (idx < 0)
            return;

        const char *spec = p;
        const char *spec_end = spec;
        while (*spec_end && *spec_end != ';')
            spec_end++;
        size_t spec_len = (size_t)(spec_end - spec);
        if (spec_len == 1 && spec[0] == '?') {
            if (idx <= 255)
                send_color_query_reply(t, 4, (int)idx, t->palette.colors[idx]);
        } else {
            char buf[128];
            if (idx <= 255 && spec_len < sizeof(buf)) {
                memcpy(buf, spec, spec_len);
                buf[spec_len] = '\0';
                uint32_t argb;
                if (wsap_vtcore_color_parse(buf, &argb))
                    wsap_vtcore_palette_set(&t->palette, (int)idx, argb);
            }
        }
        p = spec_end;
        if (*p == ';') p++;
    }
}

static void osc104_handle(struct wsap_vtcore *t, const char *params)
{
    if (!*params) {
        wsap_vtcore_palette_reset_all(&t->palette);
        return;
    }
    const char *p = params;
    while (*p) {
        long idx = osc_next_index(&p);
        if (idx < 0)
            return;
        if (idx <= 255)
            wsap_vtcore_palette_reset(&t->palette, (int)idx);
    }
}

void wsap_vtcore_osc_finish(struct wsap_vtcore *t)
{
    if (!wsap_vtcore_str_was_truncated(t)) {
        char *buf = t->osc_heap ? t->osc_heap : t->osc_buf;
        buf[t->osc_len] = '\0';

        if (!t->alternate && t->osc_len >= 5 && !strncmp(buf, "133;", 4) &&
            (buf[5] == ';' || buf[5] == '\0')) {
            if (buf[4] == 'A') {
                t->prompt_y = t->cursor_y;
                t->has_prompt = true;
            }
            if (buf[4] == 'C' || buf[4] == 'D') {
                t->prompt_y = -1;
                t->has_prompt = false;
            }
        }

        char *rest;
        long code = strtol(buf, &rest, 10);
        bool no_code = rest == buf;
        bool legacy_title = no_code && !strchr(buf, ';');
        if (no_code) {
            code = legacy_title || *buf == ';' ? 0 : -1;
            rest = buf;
        }
        if (code >= 0 && (legacy_title || *rest == ';' || *rest == '\0' || code <= 2)) {
            const char *params = *rest == ';' ? rest + 1 : rest;
            switch (code) {
            case 0: case 1: case 2:
                if (t->cb.title)
                    t->cb.title(t->cb.ud, params);
                break;
            case 4:
                osc4_handle(t, params);
                break;
            case 8:
                wsap_vtcore_osc8(t, params);
                break;
            case 10:
                osc_color_set_or_query(t, 10, params, WSAP_VTCORE_PALETTE_FG);
                break;
            case 11:
                osc_color_set_or_query(t, 11, params, WSAP_VTCORE_PALETTE_BG);
                break;
            case 12:
                osc_color_set_or_query(t, 12, params, WSAP_VTCORE_PALETTE_CURSOR);
                break;
            case 52:
                wsap_vtcore_osc52(t, params);
                break;
            case 104:
                osc104_handle(t, params);
                break;
            case 110:
                wsap_vtcore_palette_reset(&t->palette, WSAP_VTCORE_PALETTE_FG);
                break;
            case 111:
                wsap_vtcore_palette_reset(&t->palette, WSAP_VTCORE_PALETTE_BG);
                break;
            case 112:
                wsap_vtcore_palette_reset(&t->palette, WSAP_VTCORE_PALETTE_CURSOR);
                break;
            default:
                wsap_vtcore_osc_misc_dispatch(t, code, params);
                break;
            }
        }
    }
    t->osc_len = 0;
    if (t->osc_heap && t->osc_heap_cap > OSC_HEAP_KEEP) {
        free(t->osc_heap);
        t->osc_heap = NULL;
        t->osc_heap_cap = 0;
    }
}
