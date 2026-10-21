#include "wsap_vtcore.h"
#include "unicode/utf8.h"

#include <string.h>

struct seqbuf {
    char *p;
    size_t left;
    bool overflow;
};

static inline void sb_ch(struct seqbuf *sb, char c)
{
    if (!sb->left) { sb->overflow = true; return; }
    *sb->p++ = c;
    sb->left--;
}

static inline void sb_bytes(struct seqbuf *sb, const char *s, size_t len)
{
    if (sb->overflow || len > sb->left) {
        sb->overflow = true;
        sb->left = 0;
        return;
    }
    memcpy(sb->p, s, len);
    sb->p += len;
    sb->left -= len;
}

static inline void sb_u32(struct seqbuf *sb, uint32_t val)
{
    if (val < 10) {
        char c = (char)('0' + val);
        sb_bytes(sb, &c, 1);
        return;
    }
    if (val < 100) {
        char s[2] = { (char)('0' + (val / 10)), (char)('0' + (val % 10)) };
        sb_bytes(sb, s, 2);
        return;
    }
    char tmp[10];
    char *end = tmp + sizeof(tmp);
    char *p = end;
    do {
        *--p = (char)('0' + (val % 10));
        val /= 10;
    } while (val);
    sb_bytes(sb, p, (size_t)(end - p));
}

static inline void sb_utf8(struct seqbuf *sb, uint32_t cp)
{
    char tmp[8];
    size_t n = (size_t)wsap_vtcore_utf8_encode(cp, tmp);
    sb_bytes(sb, tmp, n);
}

static struct wsap_vtcore_input_result sb_result(const struct seqbuf *sb, const char *buf)
{
    if (sb->overflow)
        return (struct wsap_vtcore_input_result){ .status = WSAP_VTCORE_INPUT_BUFFER_FULL };

    size_t len = (size_t)(sb->p - buf);
    return (struct wsap_vtcore_input_result){
        .status = len ? WSAP_VTCORE_INPUT_BYTES : WSAP_VTCORE_INPUT_CONSUMED,
        .len = len,
    };
}

struct kitty_desc {
    uint32_t code;
    char     final;
};

static const struct kitty_desc kitty_desc[WSAP_VTCORE_KEY_COUNT] = {
    [WSAP_VTCORE_KEY_RETURN]    = { 13,  'u' },
    [WSAP_VTCORE_KEY_BACKSPACE] = { 127, 'u' },
    [WSAP_VTCORE_KEY_TAB]       = { 9,   'u' },
    [WSAP_VTCORE_KEY_ESCAPE]    = { 27,  'u' },

    [WSAP_VTCORE_KEY_UP]        = { 1, 'A' },
    [WSAP_VTCORE_KEY_DOWN]      = { 1, 'B' },
    [WSAP_VTCORE_KEY_RIGHT]     = { 1, 'C' },
    [WSAP_VTCORE_KEY_LEFT]      = { 1, 'D' },
    [WSAP_VTCORE_KEY_HOME]      = { 1, 'H' },
    [WSAP_VTCORE_KEY_END]       = { 1, 'F' },
    [WSAP_VTCORE_KEY_PAGE_UP]   = { 5, '~' },
    [WSAP_VTCORE_KEY_PAGE_DOWN] = { 6, '~' },
    [WSAP_VTCORE_KEY_INSERT]    = { 2, '~' },
    [WSAP_VTCORE_KEY_DELETE]    = { 3, '~' },

    [WSAP_VTCORE_KEY_F1]  = { 1,  'P' },
    [WSAP_VTCORE_KEY_F2]  = { 1,  'Q' },
    [WSAP_VTCORE_KEY_F3]  = { 13, '~' },
    [WSAP_VTCORE_KEY_F4]  = { 1,  'S' },
    [WSAP_VTCORE_KEY_F5]  = { 15, '~' },
    [WSAP_VTCORE_KEY_F6]  = { 17, '~' },
    [WSAP_VTCORE_KEY_F7]  = { 18, '~' },
    [WSAP_VTCORE_KEY_F8]  = { 19, '~' },
    [WSAP_VTCORE_KEY_F9]  = { 20, '~' },
    [WSAP_VTCORE_KEY_F10] = { 21, '~' },
    [WSAP_VTCORE_KEY_F11] = { 23, '~' },
    [WSAP_VTCORE_KEY_F12] = { 24, '~' },
    [WSAP_VTCORE_KEY_F13] = { 57376, 'u' },
    [WSAP_VTCORE_KEY_F14] = { 57377, 'u' },
    [WSAP_VTCORE_KEY_F15] = { 57378, 'u' },
    [WSAP_VTCORE_KEY_F16] = { 57379, 'u' },
    [WSAP_VTCORE_KEY_F17] = { 57380, 'u' },
    [WSAP_VTCORE_KEY_F18] = { 57381, 'u' },
    [WSAP_VTCORE_KEY_F19] = { 57382, 'u' },
    [WSAP_VTCORE_KEY_F20] = { 57383, 'u' },
    [WSAP_VTCORE_KEY_F21] = { 57384, 'u' },
    [WSAP_VTCORE_KEY_F22] = { 57385, 'u' },
    [WSAP_VTCORE_KEY_F23] = { 57386, 'u' },
    [WSAP_VTCORE_KEY_F24] = { 57387, 'u' },
    [WSAP_VTCORE_KEY_F25] = { 57388, 'u' },
    [WSAP_VTCORE_KEY_F26] = { 57389, 'u' },
    [WSAP_VTCORE_KEY_F27] = { 57390, 'u' },
    [WSAP_VTCORE_KEY_F28] = { 57391, 'u' },
    [WSAP_VTCORE_KEY_F29] = { 57392, 'u' },
    [WSAP_VTCORE_KEY_F30] = { 57393, 'u' },
    [WSAP_VTCORE_KEY_F31] = { 57394, 'u' },
    [WSAP_VTCORE_KEY_F32] = { 57395, 'u' },
    [WSAP_VTCORE_KEY_F33] = { 57396, 'u' },
    [WSAP_VTCORE_KEY_F34] = { 57397, 'u' },
    [WSAP_VTCORE_KEY_F35] = { 57398, 'u' },

    [WSAP_VTCORE_KEY_KP_0] = { 57399, 'u' },
    [WSAP_VTCORE_KEY_KP_1] = { 57400, 'u' },
    [WSAP_VTCORE_KEY_KP_2] = { 57401, 'u' },
    [WSAP_VTCORE_KEY_KP_3] = { 57402, 'u' },
    [WSAP_VTCORE_KEY_KP_4] = { 57403, 'u' },
    [WSAP_VTCORE_KEY_KP_5] = { 57404, 'u' },
    [WSAP_VTCORE_KEY_KP_6] = { 57405, 'u' },
    [WSAP_VTCORE_KEY_KP_7] = { 57406, 'u' },
    [WSAP_VTCORE_KEY_KP_8] = { 57407, 'u' },
    [WSAP_VTCORE_KEY_KP_9] = { 57408, 'u' },
    [WSAP_VTCORE_KEY_KP_DECIMAL]   = { 57409, 'u' },
    [WSAP_VTCORE_KEY_KP_DIVIDE]    = { 57410, 'u' },
    [WSAP_VTCORE_KEY_KP_MULTIPLY]  = { 57411, 'u' },
    [WSAP_VTCORE_KEY_KP_SUBTRACT]  = { 57412, 'u' },
    [WSAP_VTCORE_KEY_KP_ADD]       = { 57413, 'u' },
    [WSAP_VTCORE_KEY_KP_ENTER]     = { 57414, 'u' },
    [WSAP_VTCORE_KEY_KP_EQUAL]     = { 57415, 'u' },
    [WSAP_VTCORE_KEY_KP_SEPARATOR] = { 57416, 'u' },
    [WSAP_VTCORE_KEY_KP_LEFT]      = { 57417, 'u' },
    [WSAP_VTCORE_KEY_KP_RIGHT]     = { 57418, 'u' },
    [WSAP_VTCORE_KEY_KP_UP]        = { 57419, 'u' },
    [WSAP_VTCORE_KEY_KP_DOWN]      = { 57420, 'u' },
    [WSAP_VTCORE_KEY_KP_PAGE_UP]   = { 57421, 'u' },
    [WSAP_VTCORE_KEY_KP_PAGE_DOWN] = { 57422, 'u' },
    [WSAP_VTCORE_KEY_KP_HOME]      = { 57423, 'u' },
    [WSAP_VTCORE_KEY_KP_END]       = { 57424, 'u' },
    [WSAP_VTCORE_KEY_KP_INSERT]    = { 57425, 'u' },
    [WSAP_VTCORE_KEY_KP_DELETE]    = { 57426, 'u' },
    [WSAP_VTCORE_KEY_KP_BEGIN]     = { 1,     'E' },

    [WSAP_VTCORE_KEY_CAPS_LOCK]    = { 57358, 'u' },
    [WSAP_VTCORE_KEY_SCROLL_LOCK]  = { 57359, 'u' },
    [WSAP_VTCORE_KEY_NUM_LOCK]     = { 57360, 'u' },
    [WSAP_VTCORE_KEY_PRINT_SCREEN] = { 57361, 'u' },
    [WSAP_VTCORE_KEY_PAUSE]        = { 57362, 'u' },
    [WSAP_VTCORE_KEY_MENU]         = { 57363, 'u' },

    [WSAP_VTCORE_KEY_MEDIA_PLAY]    = { 57428, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_PAUSE]   = { 57429, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_STOP]    = { 57432, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_FORWARD] = { 57433, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_REWIND]  = { 57434, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_NEXT]    = { 57435, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_PREV]    = { 57436, 'u' },
    [WSAP_VTCORE_KEY_MEDIA_RECORD]  = { 57437, 'u' },
    [WSAP_VTCORE_KEY_VOLUME_DOWN]   = { 57438, 'u' },
    [WSAP_VTCORE_KEY_VOLUME_UP]     = { 57439, 'u' },
    [WSAP_VTCORE_KEY_MUTE]          = { 57440, 'u' },

    [WSAP_VTCORE_KEY_LSHIFT] = { 57441, 'u' },
    [WSAP_VTCORE_KEY_LCTRL]  = { 57442, 'u' },
    [WSAP_VTCORE_KEY_LALT]   = { 57443, 'u' },
    [WSAP_VTCORE_KEY_LSUPER] = { 57444, 'u' },
    [WSAP_VTCORE_KEY_LHYPER] = { 57445, 'u' },
    [WSAP_VTCORE_KEY_LMETA]  = { 57446, 'u' },
    [WSAP_VTCORE_KEY_RSHIFT] = { 57447, 'u' },
    [WSAP_VTCORE_KEY_RCTRL]  = { 57448, 'u' },
    [WSAP_VTCORE_KEY_RALT]   = { 57449, 'u' },
    [WSAP_VTCORE_KEY_RSUPER] = { 57450, 'u' },
    [WSAP_VTCORE_KEY_RHYPER] = { 57451, 'u' },
    [WSAP_VTCORE_KEY_RMETA]  = { 57452, 'u' },
    [WSAP_VTCORE_KEY_ISO_LEVEL3_SHIFT] = { 57453, 'u' },
    [WSAP_VTCORE_KEY_ISO_LEVEL5_SHIFT] = { 57454, 'u' },
};

struct legacy_desc {
    uint8_t tilde;
    char    final;
    bool    app_cursor;
    char    keypad_ss3;
    enum wsap_vtcore_key equiv;
};

static const struct legacy_desc legacy_desc[WSAP_VTCORE_KEY_COUNT] = {
    [WSAP_VTCORE_KEY_UP]        = { .tilde = 0, .final = 'A', .app_cursor = true },
    [WSAP_VTCORE_KEY_DOWN]      = { .tilde = 0, .final = 'B', .app_cursor = true },
    [WSAP_VTCORE_KEY_RIGHT]     = { .tilde = 0, .final = 'C', .app_cursor = true },
    [WSAP_VTCORE_KEY_LEFT]      = { .tilde = 0, .final = 'D', .app_cursor = true },
    [WSAP_VTCORE_KEY_HOME]      = { .tilde = 0, .final = 'H', .app_cursor = true },
    [WSAP_VTCORE_KEY_END]       = { .tilde = 0, .final = 'F', .app_cursor = true },
    [WSAP_VTCORE_KEY_PAGE_UP]   = { .tilde = 5, .final = '~' },
    [WSAP_VTCORE_KEY_PAGE_DOWN] = { .tilde = 6, .final = '~' },
    [WSAP_VTCORE_KEY_INSERT]    = { .tilde = 2, .final = '~' },
    [WSAP_VTCORE_KEY_DELETE]    = { .tilde = 3, .final = '~' },

    [WSAP_VTCORE_KEY_F1]  = { .tilde = 0, .final = 'P' },
    [WSAP_VTCORE_KEY_F2]  = { .tilde = 0, .final = 'Q' },
    [WSAP_VTCORE_KEY_F3]  = { .tilde = 0, .final = 'R' },
    [WSAP_VTCORE_KEY_F4]  = { .tilde = 0, .final = 'S' },
    [WSAP_VTCORE_KEY_F5]  = { .tilde = 15, .final = '~' },
    [WSAP_VTCORE_KEY_F6]  = { .tilde = 17, .final = '~' },
    [WSAP_VTCORE_KEY_F7]  = { .tilde = 18, .final = '~' },
    [WSAP_VTCORE_KEY_F8]  = { .tilde = 19, .final = '~' },
    [WSAP_VTCORE_KEY_F9]  = { .tilde = 20, .final = '~' },
    [WSAP_VTCORE_KEY_F10] = { .tilde = 21, .final = '~' },
    [WSAP_VTCORE_KEY_F11] = { .tilde = 23, .final = '~' },
    [WSAP_VTCORE_KEY_F12] = { .tilde = 24, .final = '~' },

    [WSAP_VTCORE_KEY_KP_0] = { .keypad_ss3 = 'p' },
    [WSAP_VTCORE_KEY_KP_1] = { .keypad_ss3 = 'q' },
    [WSAP_VTCORE_KEY_KP_2] = { .keypad_ss3 = 'r' },
    [WSAP_VTCORE_KEY_KP_3] = { .keypad_ss3 = 's' },
    [WSAP_VTCORE_KEY_KP_4] = { .keypad_ss3 = 't' },
    [WSAP_VTCORE_KEY_KP_5] = { .keypad_ss3 = 'u' },
    [WSAP_VTCORE_KEY_KP_6] = { .keypad_ss3 = 'v' },
    [WSAP_VTCORE_KEY_KP_7] = { .keypad_ss3 = 'w' },
    [WSAP_VTCORE_KEY_KP_8] = { .keypad_ss3 = 'x' },
    [WSAP_VTCORE_KEY_KP_9] = { .keypad_ss3 = 'y' },
    [WSAP_VTCORE_KEY_KP_DECIMAL]   = { .keypad_ss3 = 'n' },
    [WSAP_VTCORE_KEY_KP_DIVIDE]    = { .keypad_ss3 = 'o' },
    [WSAP_VTCORE_KEY_KP_MULTIPLY]  = { .keypad_ss3 = 'j' },
    [WSAP_VTCORE_KEY_KP_SUBTRACT]  = { .keypad_ss3 = 'm' },
    [WSAP_VTCORE_KEY_KP_ADD]       = { .keypad_ss3 = 'k' },
    [WSAP_VTCORE_KEY_KP_ENTER]     = { .keypad_ss3 = 'M', .equiv = WSAP_VTCORE_KEY_RETURN },
    [WSAP_VTCORE_KEY_KP_LEFT]      = { .equiv = WSAP_VTCORE_KEY_LEFT },
    [WSAP_VTCORE_KEY_KP_RIGHT]     = { .equiv = WSAP_VTCORE_KEY_RIGHT },
    [WSAP_VTCORE_KEY_KP_UP]        = { .equiv = WSAP_VTCORE_KEY_UP },
    [WSAP_VTCORE_KEY_KP_DOWN]      = { .equiv = WSAP_VTCORE_KEY_DOWN },
    [WSAP_VTCORE_KEY_KP_PAGE_UP]   = { .equiv = WSAP_VTCORE_KEY_PAGE_UP },
    [WSAP_VTCORE_KEY_KP_PAGE_DOWN] = { .equiv = WSAP_VTCORE_KEY_PAGE_DOWN },
    [WSAP_VTCORE_KEY_KP_HOME]      = { .equiv = WSAP_VTCORE_KEY_HOME },
    [WSAP_VTCORE_KEY_KP_END]       = { .equiv = WSAP_VTCORE_KEY_END },
    [WSAP_VTCORE_KEY_KP_INSERT]    = { .equiv = WSAP_VTCORE_KEY_INSERT },
    [WSAP_VTCORE_KEY_KP_DELETE]    = { .equiv = WSAP_VTCORE_KEY_DELETE },
    [WSAP_VTCORE_KEY_KP_BEGIN]     = { .tilde = 0, .final = 'E' },
};

enum input_mod_key {
    MOD_KEY_SHIFT = 0,
    MOD_KEY_CTRL,
    MOD_KEY_ALT,
    MOD_KEY_SUPER,
    MOD_KEY_HYPER,
    MOD_KEY_META,
    MOD_KEY_ISO3,
    MOD_KEY_ISO5,
    MOD_KEY_COUNT,
};

struct mod_desc {
    enum input_mod_key group;
    enum wsap_vtcore_key      partner;
};

static const struct mod_desc mod_desc[WSAP_VTCORE_KEY_COUNT] = {
    [WSAP_VTCORE_KEY_LSHIFT] = { MOD_KEY_SHIFT, WSAP_VTCORE_KEY_RSHIFT },
    [WSAP_VTCORE_KEY_RSHIFT] = { MOD_KEY_SHIFT, WSAP_VTCORE_KEY_LSHIFT },
    [WSAP_VTCORE_KEY_LCTRL]  = { MOD_KEY_CTRL,  WSAP_VTCORE_KEY_RCTRL },
    [WSAP_VTCORE_KEY_RCTRL]  = { MOD_KEY_CTRL,  WSAP_VTCORE_KEY_LCTRL },
    [WSAP_VTCORE_KEY_LALT]   = { MOD_KEY_ALT,   WSAP_VTCORE_KEY_RALT },
    [WSAP_VTCORE_KEY_RALT]   = { MOD_KEY_ALT,   WSAP_VTCORE_KEY_LALT },
    [WSAP_VTCORE_KEY_LSUPER] = { MOD_KEY_SUPER, WSAP_VTCORE_KEY_RSUPER },
    [WSAP_VTCORE_KEY_RSUPER] = { MOD_KEY_SUPER, WSAP_VTCORE_KEY_LSUPER },
    [WSAP_VTCORE_KEY_LHYPER] = { MOD_KEY_HYPER, WSAP_VTCORE_KEY_RHYPER },
    [WSAP_VTCORE_KEY_RHYPER] = { MOD_KEY_HYPER, WSAP_VTCORE_KEY_LHYPER },
    [WSAP_VTCORE_KEY_LMETA]  = { MOD_KEY_META,  WSAP_VTCORE_KEY_RMETA },
    [WSAP_VTCORE_KEY_RMETA]  = { MOD_KEY_META,  WSAP_VTCORE_KEY_LMETA },
    [WSAP_VTCORE_KEY_ISO_LEVEL3_SHIFT] = { MOD_KEY_ISO3, WSAP_VTCORE_KEY_NONE },
    [WSAP_VTCORE_KEY_ISO_LEVEL5_SHIFT] = { MOD_KEY_ISO5, WSAP_VTCORE_KEY_NONE },
};

static const unsigned mod_group_input_mod[MOD_KEY_COUNT] = {
    WSAP_VTCORE_MOD_SHIFT, WSAP_VTCORE_MOD_CTRL, WSAP_VTCORE_MOD_ALT,
    WSAP_VTCORE_MOD_SUPER, WSAP_VTCORE_MOD_HYPER, WSAP_VTCORE_MOD_META,
    0, 0,
};

static inline bool key_is_modifier(enum wsap_vtcore_key k)
{
    return (unsigned)k < WSAP_VTCORE_KEY_COUNT &&
           (mod_desc[k].partner != WSAP_VTCORE_KEY_NONE ||
            mod_desc[k].group == MOD_KEY_ISO3 ||
            mod_desc[k].group == MOD_KEY_ISO5);
}

static unsigned effective_mods(const struct wsap_vtcore_keyboard_state *s,
                               const struct wsap_vtcore_key_event *ev)
{
    unsigned mods = ev->mods;
    if (!key_is_modifier(ev->key)) return mods;

    const struct mod_desc *d = &mod_desc[ev->key];
    if (d->group >= MOD_KEY_ISO3) return mods;

    unsigned bit = mod_group_input_mod[d->group];
    bool other_held = wsap_vtcore_keyboard_key_down(s, d->partner);
    if (ev->action != WSAP_VTCORE_KEY_RELEASE || other_held) mods |= bit;
    else                                           mods &= ~bit;
    return mods;
}

static void format_kitty(struct seqbuf *sb, uint32_t code, char final,
                         unsigned flags, bool functional, uint32_t shifted,
                         uint32_t base, unsigned mods, unsigned event,
                         bool report_text, const struct wsap_vtcore_key_event *ev)
{
    sb_bytes(sb, "\033[", 2);
    if (final != 'u' && final != '~' && code <= 1 && !shifted && !base &&
        !mods && !(flags & WSAP_VTCORE_KEYBOARD_EVENT_TYPES) && !report_text) {
        sb_ch(sb, final);
        return;
    }

    sb_u32(sb, code);
    if ((flags & WSAP_VTCORE_KEYBOARD_ALTERNATE_KEYS) && !functional) {
        if (shifted) {
            sb_ch(sb, ':');
            sb_u32(sb, shifted);
        }
        if (base) {
            sb_ch(sb, ':');
            if (!shifted) sb_ch(sb, ':');
            sb_u32(sb, base);
        }
    }
    if (mods || (flags & WSAP_VTCORE_KEYBOARD_EVENT_TYPES) || report_text) {
        sb_ch(sb, ';');
        sb_u32(sb, mods + 1);
    }
    if ((flags & WSAP_VTCORE_KEYBOARD_EVENT_TYPES) && event != 1) {
        sb_ch(sb, ':');
        sb_u32(sb, event);
    }
    if (report_text) {
        sb_ch(sb, ';');
        if (ev->text_len) {
            size_t offset = 0;
            while (offset < ev->text_len) {
                uint32_t cp;
                int size = wsap_vtcore_utf8_decode_one(ev->text_utf8 + offset,
                                                       ev->text_len - offset, &cp);
                if (!size) break;
                if (offset) sb_ch(sb, ':');
                sb_u32(sb, cp);
                offset += (size_t)size;
            }
        } else {
            sb_u32(sb, ev->text);
        }
    }
    sb_ch(sb, final);
}

static bool printable_key_text(const struct wsap_vtcore_key_event *ev)
{
    if (!ev->text_len)
        return ev->text >= 32 && !(ev->text >= 127 && ev->text < 160);
    size_t offset = 0;
    while (offset < ev->text_len) {
        uint32_t cp;
        int size = wsap_vtcore_utf8_decode_one(ev->text_utf8 + offset,
                                               ev->text_len - offset, &cp);
        if (!size || cp < 32 || (cp >= 127 && cp < 160)) return false;
        offset += (size_t)size;
    }
    return true;
}

static bool encode_kitty(const struct wsap_vtcore_keyboard_state *s,
                         const struct wsap_vtcore_key_event *ev,
                         const struct wsap_vtcore_modes *m,
                         struct seqbuf *sb)
{
    unsigned flags = m->keyboard;
    unsigned event = ev->action == WSAP_VTCORE_KEY_PRESS ? 1u :
                     ev->action == WSAP_VTCORE_KEY_REPEAT ? 2u : 3u;
    if (!flags) return ev->action == WSAP_VTCORE_KEY_RELEASE;

    const struct kitty_desc *d = &kitty_desc[ev->key];
    bool functional = d->code != 0;
    char final = functional ? d->final : 'u';
    bool all = (flags & WSAP_VTCORE_KEYBOARD_ALL_KEYS) != 0;

    bool lock = ev->key == WSAP_VTCORE_KEY_CAPS_LOCK || ev->key == WSAP_VTCORE_KEY_SCROLL_LOCK ||
                ev->key == WSAP_VTCORE_KEY_NUM_LOCK;
    if ((key_is_modifier(ev->key) || lock) && !all)
        return true;

    unsigned mods = effective_mods(s, ev);
    if (ev->action == WSAP_VTCORE_KEY_PRESS) {
        if (ev->key == WSAP_VTCORE_KEY_CAPS_LOCK) mods ^= WSAP_VTCORE_MOD_CAPS;
        if (ev->key == WSAP_VTCORE_KEY_NUM_LOCK) mods ^= WSAP_VTCORE_MOD_NUM;
    }

    bool plain_control = ev->key == WSAP_VTCORE_KEY_RETURN || ev->key == WSAP_VTCORE_KEY_TAB ||
                         ev->key == WSAP_VTCORE_KEY_BACKSPACE;
    if (ev->action == WSAP_VTCORE_KEY_RELEASE &&
        (!(flags & WSAP_VTCORE_KEYBOARD_EVENT_TYPES) || (!all && plain_control)))
        return true;

    const unsigned command_mods = WSAP_VTCORE_MOD_ALT | WSAP_VTCORE_MOD_CTRL |
                                  WSAP_VTCORE_MOD_SUPER | WSAP_VTCORE_MOD_HYPER |
                                  WSAP_VTCORE_MOD_META;
    bool has_text = printable_key_text(ev) && !(mods & command_mods);
    if (!all && has_text) return ev->action == WSAP_VTCORE_KEY_RELEASE;
    if (!all && plain_control && !(mods & 63))
        return ev->action == WSAP_VTCORE_KEY_RELEASE;
    if (!(flags & (WSAP_VTCORE_KEYBOARD_DISAMBIGUATE | WSAP_VTCORE_KEYBOARD_EVENT_TYPES | WSAP_VTCORE_KEYBOARD_ALL_KEYS)))
        return ev->action == WSAP_VTCORE_KEY_RELEASE;

    uint32_t code = d->code, shifted = 0, base = 0;
    if (!functional) {
        code = ev->codepoint;
        if (flags & WSAP_VTCORE_KEYBOARD_ALTERNATE_KEYS) {
            shifted = (mods & WSAP_VTCORE_MOD_SHIFT) ? ev->shifted : 0;
            base = ev->base;
            if (shifted == code) shifted = 0;
            if (base == code) base = 0;
        }
    }
    bool report_text = all && (flags & WSAP_VTCORE_KEYBOARD_ASSOCIATED_TEXT) && has_text &&
                       ev->action != WSAP_VTCORE_KEY_RELEASE;
    if (!code && !report_text) return true;
    format_kitty(sb, code, final, flags, functional, shifted, base, mods & 0xff,
                 event, report_text, ev);
    return true;
}

static uint32_t control_base_char(const struct wsap_vtcore_key_event *ev)
{
    uint32_t ch = ((ev->mods & WSAP_VTCORE_MOD_SHIFT) && ev->shifted)
        ? ev->shifted : ev->codepoint;
    return ch < 0x80 ? ch : 0;
}

static bool encode_legacy(const struct wsap_vtcore_key_event *ev,
                          const struct wsap_vtcore_modes *m,
                          struct seqbuf *sb)
{
    if (m->app_keypad && (unsigned)ev->keypad < WSAP_VTCORE_KEY_COUNT &&
        legacy_desc[ev->keypad].keypad_ss3) {
        sb_bytes(sb, "\033O", 2);
        sb_ch(sb, legacy_desc[ev->keypad].keypad_ss3);
        return true;
    }

    uint32_t ch = control_base_char(ev);
    if ((ev->mods & WSAP_VTCORE_MOD_CTRL) && ch) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '@' && ch <= '_')) {
            sb_ch(sb, (char)(ch & 0x1f));
            return true;
        }
        if (ch == ' ') { sb_ch(sb, 0); return true; }
        if (ch == '-') { sb_ch(sb, 0x1f); return true; }
    }

    unsigned csi = ev->mods & 63;
    unsigned p = csi + 1;
    bool shift = (ev->mods & WSAP_VTCORE_MOD_SHIFT) != 0;

    enum wsap_vtcore_key k = ev->key;
    if (legacy_desc[k].equiv != WSAP_VTCORE_KEY_NONE)
        k = legacy_desc[k].equiv;

    if ((unsigned)k < WSAP_VTCORE_KEY_COUNT) {
        const struct legacy_desc *d = &legacy_desc[k];
        if (d->final) {
            if (d->tilde) {
                sb_bytes(sb, "\033[", 2);
                sb_u32(sb, d->tilde);
                if (csi) {
                    sb_ch(sb, ';');
                    sb_u32(sb, p);
                }
                sb_ch(sb, '~');
                return true;
            }
            if (csi) {
                sb_bytes(sb, "\033[1;", 4);
                sb_u32(sb, p);
                sb_ch(sb, d->final);
                return true;
            }
            sb_ch(sb, '\033');
            sb_ch(sb, d->app_cursor && !m->app_cursor ? '[' : 'O');
            sb_ch(sb, d->final);
            return true;
        }
    }

    switch (k) {
    case WSAP_VTCORE_KEY_RETURN:    sb_ch(sb, '\r'); return true;
    case WSAP_VTCORE_KEY_BACKSPACE:
        sb_ch(sb, m->backarrow_bs ? '\x08' : '\x7f');
        return true;
    case WSAP_VTCORE_KEY_TAB:
        if (shift) { sb_bytes(sb, "\033[Z", 3); }
        else       { sb_ch(sb, '\t'); }
        return true;
    case WSAP_VTCORE_KEY_ESCAPE:    sb_ch(sb, '\033'); return true;
    default:                 break;
    }

    if ((ev->text || ev->text_len) && !ev->text_deferred) {
        if (ev->text_len) sb_bytes(sb, ev->text_utf8, ev->text_len);
        else sb_utf8(sb, ev->text);
        return true;
    }
    return false;
}

void wsap_vtcore_keyboard_update(struct wsap_vtcore_keyboard_state *s, const struct wsap_vtcore_key_event *ev)
{
    if (!s || !ev || ev->key <= WSAP_VTCORE_KEY_NONE || ev->key >= WSAP_VTCORE_KEY_COUNT)
        return;
    if (ev->action == WSAP_VTCORE_KEY_RELEASE)
        s->down[ev->key >> 6] &= ~(1ull << (ev->key & 63));
    else if (ev->action == WSAP_VTCORE_KEY_PRESS || ev->action == WSAP_VTCORE_KEY_REPEAT)
        s->down[ev->key >> 6] |= 1ull << (ev->key & 63);
}

bool wsap_vtcore_keyboard_key_down(const struct wsap_vtcore_keyboard_state *s, enum wsap_vtcore_key key)
{
    return s && key > WSAP_VTCORE_KEY_NONE && (unsigned)key < WSAP_VTCORE_KEY_COUNT &&
           ((s->down[key >> 6] >> (key & 63)) & 1);
}

static inline bool legacy_is_escape(const struct wsap_vtcore_key_event *ev,
                                    const struct wsap_vtcore_modes *m)
{
    if (m->app_keypad && (unsigned)ev->keypad < WSAP_VTCORE_KEY_COUNT &&
        legacy_desc[ev->keypad].keypad_ss3)
        return true;
    if ((ev->mods & WSAP_VTCORE_MOD_CTRL) && control_base_char(ev) == '[')
        return true;
    enum wsap_vtcore_key k = ev->key;
    if (legacy_desc[k].equiv != WSAP_VTCORE_KEY_NONE)
        k = legacy_desc[k].equiv;
    if ((unsigned)k < WSAP_VTCORE_KEY_COUNT && legacy_desc[k].final)
        return true;
    if (k == WSAP_VTCORE_KEY_ESCAPE)
        return true;
    if (k == WSAP_VTCORE_KEY_TAB && (ev->mods & WSAP_VTCORE_MOD_SHIFT))
        return true;
    if (ev->text == '\033')
        return true;
    return false;
}

struct wsap_vtcore_input_result wsap_vtcore_input_encode_key(const struct wsap_vtcore_keyboard_state *s,
                                         const struct wsap_vtcore_key_event *ev,
                                         const struct wsap_vtcore_modes *m,
                                         char *buf, size_t buf_sz)
{
    struct wsap_vtcore_input_result out = {0};
    if (!ev || !m || !buf ||
        (ev->action != WSAP_VTCORE_KEY_PRESS && ev->action != WSAP_VTCORE_KEY_REPEAT &&
         ev->action != WSAP_VTCORE_KEY_RELEASE) ||
        (unsigned)ev->key >= WSAP_VTCORE_KEY_COUNT ||
        (ev->text_len && !ev->text_utf8))
        return out;

    struct seqbuf sb = { .p = buf, .left = buf_sz };

    bool handled = encode_kitty(s, ev, m, &sb);
    if (!handled) {
        if (ev->action == WSAP_VTCORE_KEY_RELEASE)
            return out;

        if ((ev->mods & WSAP_VTCORE_MOD_ALT) && !legacy_is_escape(ev, m))
            sb_ch(&sb, '\033');

        handled = encode_legacy(ev, m, &sb);
    }

    if (!handled)
        return out;

    return sb_result(&sb, buf);
}

struct wsap_vtcore_input_result wsap_vtcore_input_encode_mouse(const struct wsap_vtcore_mouse_event *ev,
                                           const struct wsap_vtcore_modes *m,
                                           char *buf, size_t buf_sz)
{
    struct wsap_vtcore_input_result out = {0};
    if (!ev || !m || !buf) return out;

    if (!m->mouse || (ev->mods & WSAP_VTCORE_MOD_SHIFT)) return out;

    out.status = WSAP_VTCORE_INPUT_CONSUMED;
    if (ev->kind == WSAP_VTCORE_MOUSE_LEAVE) return out;
    struct wsap_vtcore_vector2i cell = ev->pos;
    if (cell.x < 0 || cell.y < 0 || cell.x >= m->grid.x || cell.y >= m->grid.y)
        return out;
    if (m->mouse == WSAP_VTCORE_MOUSE_X10 && (ev->kind == WSAP_VTCORE_MOUSE_RELEASE || ev->kind == WSAP_VTCORE_MOUSE_MOTION)) {
        return out;
    }
    if (ev->kind == WSAP_VTCORE_MOUSE_MOTION && (m->mouse == WSAP_VTCORE_MOUSE_NORMAL ||
                       (m->mouse == WSAP_VTCORE_MOUSE_BUTTON && ev->button == WSAP_VTCORE_MOUSE_BUTTON_NONE))) {
        return out;
    }
    int button;
    if (ev->button >= WSAP_VTCORE_MOUSE_BUTTON_PRIMARY && ev->button <= WSAP_VTCORE_MOUSE_BUTTON_NONE)
        button = ev->button;
    else if (ev->button >= WSAP_VTCORE_MOUSE_BUTTON_EXTRA_1 && ev->button <= WSAP_VTCORE_MOUSE_BUTTON_EXTRA_4)
        button = 128 + ev->button - WSAP_VTCORE_MOUSE_BUTTON_EXTRA_1;
    else if (ev->button >= WSAP_VTCORE_MOUSE_WHEEL_UP && ev->button <= WSAP_VTCORE_MOUSE_WHEEL_RIGHT)
        button = 64 + ev->button - WSAP_VTCORE_MOUSE_WHEEL_UP;
    else {
        return out;
    }

    int mods = 0;
    if (ev->mods & WSAP_VTCORE_MOD_ALT)  mods |= 8;
    if (ev->mods & WSAP_VTCORE_MOD_CTRL) mods |= 16;

    int code = button | mods | (ev->kind == WSAP_VTCORE_MOUSE_MOTION ? 32 : 0);
    int legacy_code = ev->kind == WSAP_VTCORE_MOUSE_RELEASE ? 3 | mods : code;
    struct wsap_vtcore_vector2i report = m->mouse_encoding == WSAP_VTCORE_MOUSE_ENC_SGR_PIXELS
        ? ev->pixel : cell;
    if (report.x < 0 || report.y < 0)
        return out;
    uint32_t report_x = (uint32_t)report.x + 1;
    uint32_t report_y = (uint32_t)report.y + 1;

    struct seqbuf sb = { .p = buf, .left = buf_sz };

    if (m->mouse_encoding == WSAP_VTCORE_MOUSE_ENC_SGR ||
        m->mouse_encoding == WSAP_VTCORE_MOUSE_ENC_SGR_PIXELS) {
        sb_bytes(&sb, "\033[<", 3);
        sb_u32(&sb, (uint32_t)code);
        sb_ch(&sb, ';');
        sb_u32(&sb, report_x);
        sb_ch(&sb, ';');
        sb_u32(&sb, report_y);
        sb_ch(&sb, ev->kind == WSAP_VTCORE_MOUSE_RELEASE ? 'm' : 'M');
    } else if (m->mouse_encoding == WSAP_VTCORE_MOUSE_ENC_URXVT) {
        sb_bytes(&sb, "\033[", 2);
        sb_u32(&sb, (uint32_t)(legacy_code + 32));
        sb_ch(&sb, ';');
        sb_u32(&sb, (uint32_t)(cell.x + 1));
        sb_ch(&sb, ';');
        sb_u32(&sb, (uint32_t)(cell.y + 1));
        sb_ch(&sb, 'M');
    } else if (m->mouse_encoding == WSAP_VTCORE_MOUSE_ENC_UTF8) {
        sb_bytes(&sb, "\033[M", 3);
        sb_utf8(&sb, (unsigned)legacy_code + 32);
        sb_utf8(&sb, (unsigned)cell.x + 33);
        sb_utf8(&sb, (unsigned)cell.y + 33);
    } else {
        if (legacy_code > 223 || cell.x > 222 || cell.y > 222)
            return out;
        char tmp[6] = {
            '\033', '[', 'M',
            (char)(legacy_code + 32),
            (char)(cell.x + 33),
            (char)(cell.y + 33),
        };
        sb_bytes(&sb, tmp, 6);
    }

    return sb_result(&sb, buf);
}

static bool is_direction_format(uint32_t cp)
{
    return cp == 0x061c ||
           (cp >= 0x200e && cp <= 0x200f) ||
           (cp >= 0x202a && cp <= 0x202e) ||
           (cp >= 0x2066 && cp <= 0x2069) ||
           cp == 0xfeff;
}

struct wsap_vtcore_input_result wsap_vtcore_input_encode_paste(const struct wsap_vtcore_modes *m,
                                                 const char *text, size_t len,
                                                 char *buf, size_t capacity)
{
    struct wsap_vtcore_input_result out = {0};
    if (!m || !buf || (!text && len)) return out;

    struct seqbuf sb = { .p = buf, .left = capacity };
    if (m->bracketed_paste) sb_bytes(&sb, "\033[200~", 6);
    char *body = sb.p;
    for (size_t i = 0; i < len;) {
        if (sb.overflow) break;
        size_t run = wsap_vtcore_scan_printable_span(text + i, len - i, 0x7f);
        if (run > 0) {
            sb_bytes(&sb, text + i, run);
            i += run;
            continue;
        }
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            if (c == '\r' || c == '\n') {
                sb_ch(&sb, '\r');
                if (c == '\r' && i + 1 < len && text[i + 1] == '\n') i++;
            } else if (c == '\t') {
                sb_ch(&sb, '\t');
            }
            i++;
            continue;
        }
        uint32_t cp;
        int n = wsap_vtcore_utf8_decode_one(text + i, len - i, &cp);
        if (!n) { i++; continue; }
        if (cp >= 0xa0 && !is_direction_format(cp)) {
            sb_bytes(&sb, text + i, (size_t)n);
        }
        i += (size_t)n;
    }

    out.status = WSAP_VTCORE_INPUT_CONSUMED;
    if (!sb.overflow && sb.p == body) return out;
    if (m->bracketed_paste) sb_bytes(&sb, "\033[201~", 6);
    return sb_result(&sb, buf);
}
