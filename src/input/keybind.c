#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "input/keybind.h"

struct name_entry {
    const char *name;
    unsigned value;
};

static const struct name_entry action_names[] = {
    {"none",         INPUT_SHORTCUT_NONE},
    {"font_larger",  INPUT_SHORTCUT_FONT_LARGER},
    {"font_smaller", INPUT_SHORTCUT_FONT_SMALLER},
    {"copy",         INPUT_SHORTCUT_COPY},
    {"paste",        INPUT_SHORTCUT_PASTE},
    {"select_all",   INPUT_SHORTCUT_SELECT_ALL},
    {"scroll_up",    INPUT_SHORTCUT_SCROLL_UP},
    {"scroll_down",  INPUT_SHORTCUT_SCROLL_DOWN},
    {"new_tab",      INPUT_SHORTCUT_NEW_TAB},
    {"close_tab",    INPUT_SHORTCUT_CLOSE_TAB},
    {"next_tab",     INPUT_SHORTCUT_NEXT_TAB},
    {"prev_tab",     INPUT_SHORTCUT_PREV_TAB},
    {"last_tab",     INPUT_SHORTCUT_LAST_TAB},
    {"tab_1",        INPUT_SHORTCUT_TAB_1},
    {"tab_2",        INPUT_SHORTCUT_TAB_2},
    {"tab_3",        INPUT_SHORTCUT_TAB_3},
    {"tab_4",        INPUT_SHORTCUT_TAB_4},
    {"tab_5",        INPUT_SHORTCUT_TAB_5},
    {"tab_6",        INPUT_SHORTCUT_TAB_6},
    {"tab_7",        INPUT_SHORTCUT_TAB_7},
    {"tab_8",        INPUT_SHORTCUT_TAB_8},
    {"kill_line",    INPUT_SHORTCUT_KILL_LINE},
    {"command_palette", INPUT_SHORTCUT_COMMAND_PALETTE},
    {"debug_overlay", INPUT_SHORTCUT_DEBUG_OVERLAY},
    {"settings", INPUT_SHORTCUT_SETTINGS},
    {"focus_tabs", INPUT_SHORTCUT_FOCUS_TABS},
};

static const struct name_entry mod_names[] = {
    {"ctrl",    INPUT_MOD_CTRL},
    {"control", INPUT_MOD_CTRL},
    {"shift",   INPUT_MOD_SHIFT},
    {"alt",     INPUT_MOD_ALT},
    {"option",  INPUT_MOD_ALT},
    {"super",   INPUT_MOD_SUPER},
    {"cmd",     INPUT_MOD_SUPER},
    {"command", INPUT_MOD_SUPER},
    {"logo",    INPUT_MOD_SUPER},
    {"hyper",   INPUT_MOD_HYPER},
    {"meta",    INPUT_MOD_META},
};

static const struct name_entry key_names[] = {
    {"enter",       INPUT_KEY_RETURN},
    {"return",      INPUT_KEY_RETURN},
    {"backspace",   INPUT_KEY_BACKSPACE},
    {"tab",         INPUT_KEY_TAB},
    {"escape",      INPUT_KEY_ESCAPE},
    {"esc",         INPUT_KEY_ESCAPE},
    {"up",          INPUT_KEY_UP},
    {"down",        INPUT_KEY_DOWN},
    {"right",       INPUT_KEY_RIGHT},
    {"left",        INPUT_KEY_LEFT},
    {"home",        INPUT_KEY_HOME},
    {"end",         INPUT_KEY_END},
    {"pageup",      INPUT_KEY_PAGE_UP},
    {"page_up",     INPUT_KEY_PAGE_UP},
    {"pagedown",    INPUT_KEY_PAGE_DOWN},
    {"page_down",   INPUT_KEY_PAGE_DOWN},
    {"insert",      INPUT_KEY_INSERT},
    {"delete",      INPUT_KEY_DELETE},
    {"kp_add",      INPUT_KEY_KP_ADD},
    {"kp_subtract", INPUT_KEY_KP_SUBTRACT},
    {"kp_enter",    INPUT_KEY_KP_ENTER},
    {"space",       ' '},
};

static bool lookup_name(const struct name_entry *tab, size_t n,
                        const char *s, unsigned *out)
{
    if (!s || !*s)
        return false;

    int first = tolower((unsigned char)s[0]);
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)tab[i].name[0]) == first &&
            !strcasecmp(tab[i].name, s)) {
            *out = tab[i].value;
            return true;
        }
    }
    return false;
}

bool input_parse_action(const char *s, enum input_shortcut *out)
{
    unsigned v;
    if (!s || !lookup_name(action_names, sizeof(action_names) / sizeof(*action_names),
                           s, &v))
        return false;
    *out = (enum input_shortcut)v;
    return true;
}

bool input_parse_keybind_style(const char *s, enum input_keybind_style *out)
{
    if (!s) return false;
    if (!strcasecmp(s, "legacy")) { *out = INPUT_KEYBIND_LEGACY; return true; }
    if (!strcasecmp(s, "modern")) { *out = INPUT_KEYBIND_MODERN; return true; }
    return false;
}

static bool parse_key_token(const char *tok, uint32_t *codepoint, uint16_t *key)
{
    unsigned v;
    *codepoint = 0;
    *key = INPUT_KEY_NONE;

    if (lookup_name(key_names, sizeof(key_names) / sizeof(*key_names), tok, &v)) {
        if (v == ' ') *codepoint = ' ';
        else *key = (uint16_t)v;
        return true;
    }
    if ((tok[0] == 'f' || tok[0] == 'F') && tok[1]) {
        char *end;
        long n = strtol(tok + 1, &end, 10);
        if (!*end && n >= 1 && n <= 35) {
            *key = (uint16_t)(INPUT_KEY_F1 + (n - 1));
            return true;
        }
    }
    if (tok[0] && !tok[1]) {
        unsigned char c = (unsigned char)tok[0];
        *codepoint = (c >= 'A' && c <= 'Z') ? (uint32_t)(c - 'A' + 'a') : c;
        return true;
    }
    return false;
}

bool input_parse_chord(const char *s, unsigned *mods, uint32_t *codepoint,
                       uint16_t *key)
{
    if (!s || !*s)
        return false;
    char buf[128];
    size_t n = strlen(s);
    if (n >= sizeof(buf))
        return false;
    memcpy(buf, s, n + 1);

    *mods = 0;
    char *tok = buf;
    for (;;) {
        char *plus = strchr(tok, '+');
        if (!plus)
            break;
        *plus = 0;
        unsigned m;
        if (!*tok || !lookup_name(mod_names, sizeof(mod_names) / sizeof(*mod_names),
                                  tok, &m))
            return false;
        *mods |= m;
        tok = plus + 1;
    }
    return parse_key_token(tok, codepoint, key);
}

static void append_modifier(char *buf, size_t *pos, const char *name)
{
    size_t len = strlen(name);
    memcpy(buf + *pos, name, len);
    *pos += len;
}

bool input_format_chord(unsigned mods, uint32_t codepoint, uint16_t key,
                        char *out, size_t sz)
{
    if (!out || sz == 0)
        return false;
    out[0] = '\0';
    char buf[64] = {0};
    size_t pos = 0;

    if (mods & INPUT_MOD_CTRL) append_modifier(buf, &pos, "Ctrl+");
#ifdef __APPLE__
    if (mods & INPUT_MOD_ALT) append_modifier(buf, &pos, "Option+");
#else
    if (mods & INPUT_MOD_ALT) append_modifier(buf, &pos, "Alt+");
#endif
    if (mods & INPUT_MOD_SHIFT) append_modifier(buf, &pos, "Shift+");
#ifdef __APPLE__
    if (mods & INPUT_MOD_SUPER) append_modifier(buf, &pos, "Cmd+");
#else
    if (mods & INPUT_MOD_SUPER) append_modifier(buf, &pos, "Super+");
#endif
    if (mods & INPUT_MOD_HYPER) append_modifier(buf, &pos, "Hyper+");
    if (mods & INPUT_MOD_META) append_modifier(buf, &pos, "Meta+");

    const char *key_str = NULL;
    char char_str[8] = {0};

    if (key >= INPUT_KEY_F1 && key <= INPUT_KEY_F35) {
        snprintf(char_str, sizeof(char_str), "F%d", (int)(key - INPUT_KEY_F1 + 1));
        key_str = char_str;
    } else {
        switch (key) {
        case INPUT_KEY_RETURN:      key_str = "Enter"; break;
        case INPUT_KEY_BACKSPACE:   key_str = "Backspace"; break;
        case INPUT_KEY_TAB:         key_str = "Tab"; break;
        case INPUT_KEY_ESCAPE:      key_str = "Esc"; break;
        case INPUT_KEY_UP:          key_str = "Up"; break;
        case INPUT_KEY_DOWN:        key_str = "Down"; break;
        case INPUT_KEY_LEFT:        key_str = "Left"; break;
        case INPUT_KEY_RIGHT:       key_str = "Right"; break;
        case INPUT_KEY_HOME:        key_str = "Home"; break;
        case INPUT_KEY_END:         key_str = "End"; break;
        case INPUT_KEY_PAGE_UP:     key_str = "PageUp"; break;
        case INPUT_KEY_PAGE_DOWN:   key_str = "PageDown"; break;
        case INPUT_KEY_INSERT:      key_str = "Insert"; break;
        case INPUT_KEY_DELETE:      key_str = "Delete"; break;
        case INPUT_KEY_KP_ADD:      key_str = "KP+"; break;
        case INPUT_KEY_KP_SUBTRACT: key_str = "KP-"; break;
        case INPUT_KEY_KP_ENTER:    key_str = "KP Enter"; break;
        default: break;
        }
    }

    if (!key_str) {
        if (codepoint == ' ') {
            key_str = "Space";
        } else if (codepoint >= 'a' && codepoint <= 'z') {
            char_str[0] = (char)(codepoint - 'a' + 'A');
            char_str[1] = '\0';
            key_str = char_str;
        } else if (codepoint >= 33 && codepoint <= 126) {
            char_str[0] = (char)codepoint;
            char_str[1] = '\0';
            key_str = char_str;
        }
    }

    if (!key_str)
        return false;

    size_t klen = strlen(key_str);
    if (pos + klen >= sizeof(buf))
        return false;
    memcpy(buf + pos, key_str, klen + 1);

    if (strlen(buf) >= sz)
        return false;
    memcpy(out, buf, strlen(buf) + 1);
    return true;
}

bool input_keymap_format_action_chord(const struct input_keymap *km,
                                enum input_shortcut action, char *out,
                                size_t sz)
{
    if (!km || !out || sz == 0 || action == INPUT_SHORTCUT_NONE)
        return false;
    out[0] = '\0';
    for (int i = 0; i < km->n; i++) {
        const struct input_keybind *b = &km->binds[i];
        if (b->action == (uint16_t)action) {
            if (input_format_chord(b->mods, b->codepoint, b->key, out, sz))
                return true;
        }
    }
    return false;
}

void input_keymap_clear(struct input_keymap *km)
{
    if (km) {
        km->n = 0;
        km->hash_valid = false;
    }
}

static uint32_t lower(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static inline uint32_t chord_token(uint32_t codepoint, uint16_t key)
{
    return key != INPUT_KEY_NONE ? ((uint32_t)key | 0x8000u) : lower(codepoint);
}

static inline uint32_t make_chord(unsigned mods, uint32_t token)
{
    return (mods << 16) | (token & 0xFFFFu);
}

static inline uint32_t hash_chord(uint32_t chord)
{
    return (chord * 2654435761u) & (INPUT_KEYMAP_HASH_CAP - 1);
}

static void rebuild_hash(struct input_keymap *km)
{
    memset(km->hash, 0, sizeof(km->hash));
    uint32_t mask = INPUT_KEYMAP_HASH_CAP - 1;
    for (int i = 0; i < km->n; i++) {
        const struct input_keybind *b = &km->binds[i];
        if (!b->mods)
            continue;
        uint32_t token = chord_token(b->codepoint, b->key);
        uint32_t chord = make_chord(b->mods, token);
        if (!chord)
            continue;
        uint32_t idx = hash_chord(chord);
        for (uint32_t probe = 0; probe <= mask; probe++) {
            struct input_keymap_slot *slot = &km->hash[idx];
            if (slot->chord == 0) {
                slot->chord = chord;
                slot->action = b->action;
                break;
            }
            if (slot->chord == chord) {
                break;
            }
            idx = (idx + 1) & mask;
        }
    }
    km->hash_valid = true;
}

static inline const struct input_keymap_slot *hash_lookup(const struct input_keymap *km,
                                                          uint32_t chord)
{
    if (!chord)
        return NULL;
    uint32_t mask = INPUT_KEYMAP_HASH_CAP - 1;
    uint32_t idx = hash_chord(chord);
    for (uint32_t probe = 0; probe <= mask; probe++) {
        const struct input_keymap_slot *slot = &km->hash[idx];
        if (slot->chord == 0)
            return NULL;
        if (slot->chord == chord)
            return slot;
        idx = (idx + 1) & mask;
    }
    return NULL;
}

static bool keymap_set(struct input_keymap *km, unsigned mods,
                       uint32_t codepoint, uint16_t key, uint16_t action)
{
    if (!km || !mods)
        return false;
    for (int i = 0; i < km->n; i++) {
        struct input_keybind *b = &km->binds[i];
        if (b->mods == mods && b->codepoint == codepoint && b->key == key) {
            b->action = action;
            km->hash_valid = false;
            return true;
        }
    }
    if (km->n >= INPUT_MAX_KEYBINDS)
        return false;
    km->binds[km->n++] = (struct input_keybind){mods, codepoint, key,
                                                action};
    km->hash_valid = false;
    return true;
}

bool input_keymap_apply_spec(struct input_keymap *km, const char *spec)
{
    if (!km || !spec)
        return false;
    if (!strcasecmp(spec, "clear")) {
        input_keymap_clear(km);
        return true;
    }
    const char *eq = strrchr(spec, '=');
    if (!eq || eq == spec)
        return false;

    char chord[128];
    size_t n = (size_t)(eq - spec);
    while (n && (spec[n - 1] == ' ' || spec[n - 1] == '\t'))
        n--;
    if (!n || n >= sizeof(chord))
        return false;
    memcpy(chord, spec, n);
    chord[n] = 0;

    const char *act = eq + 1;
    while (*act == ' ' || *act == '\t')
        act++;

    enum input_shortcut action;
    unsigned mods;
    uint32_t codepoint;
    uint16_t key;
    if (!input_parse_action(act, &action) ||
        !input_parse_chord(chord, &mods, &codepoint, &key))
        return false;
    if (action == INPUT_SHORTCUT_NONE) {
        for (int i = 0; i < km->n; i++) {
            struct input_keybind *b = &km->binds[i];
            if (b->mods == mods && b->codepoint == codepoint && b->key == key) {
                km->n--;
                memmove(&km->binds[i], &km->binds[i + 1],
                        (size_t)(km->n - i) * sizeof(km->binds[0]));
                km->hash_valid = false;
                return true;
            }
        }
        return true;
    }
    return keymap_set(km, mods, codepoint, key, (uint16_t)action);
}

enum input_shortcut input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev)
{
    if (!km || !ev)
        return INPUT_SHORTCUT_NONE;
    unsigned mods = ev->mods & ~(unsigned)(INPUT_MOD_CAPS | INPUT_MOD_NUM);
    if (!mods)
        return INPUT_SHORTCUT_NONE;

    if (!km->hash_valid)
        rebuild_hash((struct input_keymap *)km);

    if (ev->key != INPUT_KEY_NONE) {
        const struct input_keymap_slot *s =
            hash_lookup(km, make_chord(mods, chord_token(0, (uint16_t)ev->key)));
        if (s)
            return (enum input_shortcut)s->action;
    }
    if (ev->keypad != INPUT_KEY_NONE) {
        const struct input_keymap_slot *s =
            hash_lookup(km, make_chord(mods, chord_token(0, (uint16_t)ev->keypad)));
        if (s)
            return (enum input_shortcut)s->action;
    }
    if (ev->codepoint) {
        const struct input_keymap_slot *s =
            hash_lookup(km, make_chord(mods, chord_token(ev->codepoint, INPUT_KEY_NONE)));
        if (s)
            return (enum input_shortcut)s->action;
    }

    if (ev->shifted && lower(ev->shifted) != lower(ev->codepoint)) {
        const struct input_keymap_slot *s =
            hash_lookup(km, make_chord(mods, chord_token(ev->shifted, INPUT_KEY_NONE)));
        if (s)
            return (enum input_shortcut)s->action;
    }

    if (ev->base && lower(ev->base) != lower(ev->codepoint)) {
        const struct input_keymap_slot *s =
            hash_lookup(km, make_chord(mods, chord_token(ev->base, INPUT_KEY_NONE)));
        if (s)
            return (enum input_shortcut)s->action;
    }

    return INPUT_SHORTCUT_NONE;
}

static void add_bind(struct input_keymap *km, unsigned mods, uint32_t cp,
                     uint16_t key, enum input_shortcut action)
{
    if (mods)
        keymap_set(km, mods, cp, key, (uint16_t)action);
}

void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style)
{
    if (!km || !p)
        return;
    input_keymap_clear(km);

#ifndef __APPLE__
    unsigned accel = (style == INPUT_KEYBIND_MODERN)
        ? INPUT_MOD_CTRL
        : p->accel;
    unsigned accel_alt = (style == INPUT_KEYBIND_MODERN)
        ? (INPUT_MOD_CTRL | INPUT_MOD_SHIFT)
        : p->accel_alt;
    const unsigned accels[2] = {accel, accel_alt};
#else
    (void)style;
    const unsigned accels[2] = {p->accel, p->accel_alt};
#endif
    for (int i = 0; i < 2; i++) {
        add_bind(km, accels[i], 'c', INPUT_KEY_NONE, INPUT_SHORTCUT_COPY);
        add_bind(km, accels[i], 'v', INPUT_KEY_NONE, INPUT_SHORTCUT_PASTE);
    }
#ifdef __APPLE__
    add_bind(km, INPUT_MOD_SUPER, 'a', INPUT_KEY_NONE, INPUT_SHORTCUT_SELECT_ALL);
    add_bind(km, INPUT_MOD_SUPER, 0, INPUT_KEY_BACKSPACE, INPUT_SHORTCUT_KILL_LINE);
#endif

#ifdef __APPLE__
    add_bind(km, p->zoom, '+', INPUT_KEY_NONE, INPUT_SHORTCUT_FONT_LARGER);
    add_bind(km, p->zoom, '=', INPUT_KEY_NONE, INPUT_SHORTCUT_FONT_LARGER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_ADD, INPUT_SHORTCUT_FONT_LARGER);
    add_bind(km, p->zoom, '-', INPUT_KEY_NONE, INPUT_SHORTCUT_FONT_SMALLER);
    add_bind(km, p->zoom, '_', INPUT_KEY_NONE, INPUT_SHORTCUT_FONT_SMALLER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_SUBTRACT, INPUT_SHORTCUT_FONT_SMALLER);
#else
    add_bind(km, INPUT_MOD_CTRL, '=', INPUT_KEY_NONE,
             INPUT_SHORTCUT_FONT_LARGER);
    add_bind(km, INPUT_MOD_CTRL, '-', INPUT_KEY_NONE,
             INPUT_SHORTCUT_FONT_SMALLER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_ADD, INPUT_SHORTCUT_FONT_LARGER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_SUBTRACT,
             INPUT_SHORTCUT_FONT_SMALLER);
#endif

#ifdef __APPLE__
    add_bind(km, INPUT_MOD_SUPER | INPUT_MOD_SHIFT, 'p', INPUT_KEY_NONE,
             INPUT_SHORTCUT_COMMAND_PALETTE);
#else
    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 'p', INPUT_KEY_NONE,
             INPUT_SHORTCUT_COMMAND_PALETTE);
#endif

    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 0, INPUT_KEY_F12,
             INPUT_SHORTCUT_DEBUG_OVERLAY);
#ifdef __APPLE__
    add_bind(km, INPUT_MOD_SUPER, ',', INPUT_KEY_NONE, INPUT_SHORTCUT_SETTINGS);
#else
    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, ',', INPUT_KEY_NONE,
             INPUT_SHORTCUT_SETTINGS);
#endif

    add_bind(km, INPUT_MOD_SHIFT, 0, INPUT_KEY_INSERT, INPUT_SHORTCUT_PASTE);
    add_bind(km, INPUT_MOD_SHIFT, 0, INPUT_KEY_PAGE_UP, INPUT_SHORTCUT_SCROLL_UP);
    add_bind(km, INPUT_MOD_SHIFT, 0, INPUT_KEY_PAGE_DOWN,
             INPUT_SHORTCUT_SCROLL_DOWN);

    unsigned tab = p->tab_accel ? p->tab_accel : p->accel;
    if (!tab)
        return;

    add_bind(km, tab, 't', INPUT_KEY_NONE, INPUT_SHORTCUT_NEW_TAB);
    add_bind(km, tab, 'w', INPUT_KEY_NONE, INPUT_SHORTCUT_CLOSE_TAB);
    for (int i = 0; i < 8; i++)
        add_bind(km, tab, (uint32_t)('1' + i), INPUT_KEY_NONE,
                 (enum input_shortcut)(INPUT_SHORTCUT_TAB_1 + i));
    add_bind(km, tab, '9', INPUT_KEY_NONE, INPUT_SHORTCUT_LAST_TAB);

    add_bind(km, INPUT_MOD_CTRL, 0, INPUT_KEY_TAB, INPUT_SHORTCUT_NEXT_TAB);
    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 0, INPUT_KEY_TAB,
             INPUT_SHORTCUT_PREV_TAB);
    add_bind(km, tab, 0, INPUT_KEY_PAGE_DOWN, INPUT_SHORTCUT_NEXT_TAB);
    add_bind(km, tab, 0, INPUT_KEY_PAGE_UP, INPUT_SHORTCUT_PREV_TAB);
    add_bind(km, tab, 0, INPUT_KEY_F6, INPUT_SHORTCUT_FOCUS_TABS);
}

static struct input_keymap active_keymap;
static bool active_ready;

void input_keymap_reset_active(enum input_keybind_style style)
{
    input_keymap_defaults(&active_keymap, &input_platform_shortcuts, style);
    active_ready = true;
}

struct input_keymap *input_keymap_active(void)
{
    if (!active_ready)
        input_keymap_reset_active(INPUT_KEYBIND_MODERN);
    return &active_keymap;
}
