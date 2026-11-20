#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "input/command.h"
#include "input/keybind.h"
#include "util/unicode/utf8.h"

struct name_entry {
    const char *name;
    unsigned value;
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

/* Finds the command that `s` names for scope `scope`. The function reads
 * `reg` first, then the built-in app commands. A widget registry holds no app
 * command, so a widget scope can still bind one. */
static bool parse_action_in(const struct input_command_registry *reg,
                            uint8_t scope, const char *s,
                            input_command_id *out)
{
    if (!s || !out)
        return false;
    if (!strcasecmp(s, "none")) {
        *out = INPUT_COMMAND_NONE;
        return true;
    }
    const struct input_command *cmd =
        input_command_find_name_scoped(reg, scope, s);
    if (!cmd && reg != input_command_builtin())
        cmd = input_command_find_name_scoped(input_command_builtin(),
                                             INPUT_COMMAND_SCOPE_APP, s);
    if (!cmd)
        return false;
    *out = cmd->id;
    return true;
}

bool input_parse_action(const char *s, input_command_id *out)
{
    return parse_action_in(input_command_builtin(), INPUT_COMMAND_SCOPE_APP, s,
                           out);
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
    uint32_t cp;
    int len = unicode_utf8_decode_one(tok, strlen(tok), &cp);
    if (len > 0 && !tok[len]) {
        *codepoint = cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp;
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

bool input_keymap_format_action_chord_scoped(const struct input_keymap *km,
                                input_command_id action, uint8_t scope,
                                char *out, size_t sz)
{
    if (!km || !out || sz == 0 || action == INPUT_COMMAND_NONE ||
        scope >= INPUT_COMMAND_SCOPE_COUNT)
        return false;
    out[0] = '\0';
    for (size_t i = 0; i < km->n; i++) {
        const struct input_keybind *b = &km->binds[i];
        if (b->action == (uint16_t)action && b->scope == scope) {
            if (input_format_chord(b->mods, b->codepoint, b->key, out, sz))
                return true;
        }
    }
    return false;
}

bool input_keymap_format_action_chord(const struct input_keymap *km,
                                input_command_id action, char *out,
                                size_t sz)
{
    return input_keymap_format_action_chord_scoped(
        km, action, INPUT_COMMAND_SCOPE_APP, out, sz);
}

void input_keymap_free(struct input_keymap *km)
{
    if (!km)
        return;
    free(km->hash_chord);
    free(km->hash_action);
    free(km->binds);
    km->hash_chord = NULL;
    km->hash_action = NULL;
    km->binds = NULL;
    km->hash_cap = 0;
    km->n = 0;
    km->cap = 0;
    km->hash_valid = false;
}

void input_keymap_init(struct input_keymap *km)
{
    if (km)
        *km = (struct input_keymap){0};
}

void input_keymap_clear(struct input_keymap *km)
{
    if (km) {
        km->n = 0;
        km->hash_valid = false;
    }
}

void input_keymap_clear_scope(struct input_keymap *km, uint8_t scope)
{
    if (!km)
        return;
    size_t kept = 0;
    for (size_t i = 0; i < km->n; i++)
        if (km->binds[i].scope != scope)
            km->binds[kept++] = km->binds[i];
    km->n = kept;
    km->hash_valid = false;
}

static uint32_t lower(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* The token of a key is above every Unicode codepoint, and the token of a
 * character is the codepoint itself. No token is zero. */
static inline uint32_t chord_token(uint32_t codepoint, uint16_t key)
{
    return key != INPUT_KEY_NONE ? 0x110000u + key : lower(codepoint);
}

/* A chord keeps all 32 bits of the modifiers and all 21 bits of the token. */
static inline uint64_t make_chord(uint8_t scope, unsigned mods, uint32_t token)
{
    return ((uint64_t)mods << 32) | ((uint64_t)scope << 21) | token;
}

static inline uint32_t hash_chord(uint64_t chord)
{
    return (uint32_t)((chord * 0x9e3779b97f4a7c15ull) >> 32);
}

static bool rebuild_hash(struct input_keymap *km)
{
    size_t cap = 256;
    while (cap < km->n * 2)
        cap *= 2;
    if (cap != km->hash_cap) {
        uint64_t *chord = realloc(km->hash_chord, cap * sizeof *chord);
        if (!chord)
            return false;
        km->hash_chord = chord;
        uint16_t *action = realloc(km->hash_action, cap * sizeof *action);
        if (!action)
            return false;
        km->hash_action = action;
        km->hash_cap = cap;
    }
    memset(km->hash_chord, 0, km->hash_cap * sizeof *km->hash_chord);
    size_t mask = km->hash_cap - 1;
    for (size_t i = 0; i < km->n; i++) {
        const struct input_keybind *b = &km->binds[i];
        uint64_t chord = make_chord(b->scope, b->mods,
                                    chord_token(b->codepoint, b->key));
        size_t idx = hash_chord(chord) & mask;
        for (size_t probe = 0; probe <= mask; probe++) {
            if (km->hash_chord[idx] == 0) {
                km->hash_chord[idx] = chord;
                km->hash_action[idx] = b->action;
                break;
            }
            if (km->hash_chord[idx] == chord)
                break;
            idx = (idx + 1) & mask;
        }
    }
    km->hash_valid = true;
    return true;
}

static inline bool hash_lookup(const struct input_keymap *km, uint64_t chord,
                               input_command_id *out)
{
    if (!chord || !km->hash_cap)
        return false;
    size_t mask = km->hash_cap - 1;
    size_t idx = hash_chord(chord) & mask;
    for (size_t probe = 0; probe <= mask; probe++) {
        uint64_t slot_chord = km->hash_chord[idx];
        if (slot_chord == 0)
            return false;
        if (slot_chord == chord) {
            *out = (input_command_id)km->hash_action[idx];
            return true;
        }
        idx = (idx + 1) & mask;
    }
    return false;
}

static bool keymap_set(struct input_keymap *km, uint8_t scope, unsigned mods,
                       uint32_t codepoint, uint16_t key, uint16_t action)
{
    if (!km || scope >= INPUT_COMMAND_SCOPE_COUNT ||
        (!codepoint && key == INPUT_KEY_NONE))
        return false;
    for (size_t i = 0; i < km->n; i++) {
        struct input_keybind *b = &km->binds[i];
        if (b->scope == scope && b->mods == mods &&
            b->codepoint == codepoint && b->key == key) {
            b->action = action;
            km->hash_valid = false;
            return true;
        }
    }
    if (km->n == km->cap) {
        size_t cap = km->cap ? km->cap * 2 : INPUT_KEYMAP_INITIAL_CAP;
        struct input_keybind *binds = realloc(km->binds, cap * sizeof *binds);
        if (!binds)
            return false;
        km->binds = binds;
        km->cap = cap;
    }
    km->binds[km->n++] = (struct input_keybind){
        .mods = mods, .codepoint = codepoint, .key = key,
        .action = action, .scope = scope,
    };
    km->hash_valid = false;
    return true;
}

bool input_keymap_copy_scope(struct input_keymap *dst,
                             const struct input_keymap *src, uint8_t scope)
{
    if (!dst || !src || scope >= INPUT_COMMAND_SCOPE_COUNT)
        return false;
    if (dst == src)
        return true;
    input_keymap_clear_scope(dst, scope);
    for (size_t i = 0; i < src->n; i++) {
        const struct input_keybind *b = &src->binds[i];
        if (b->scope == scope &&
            !keymap_set(dst, scope, b->mods, b->codepoint, b->key, b->action))
            return false;
    }
    return true;
}

bool input_keymap_apply_spec(struct input_keymap *km, const char *spec)
{
    return input_keymap_apply_spec_scoped(km, INPUT_COMMAND_SCOPE_APP, spec);
}

bool input_keymap_apply_spec_scoped(struct input_keymap *km, uint8_t scope,
                                    const char *spec)
{
    return input_keymap_apply_spec_registry(km, input_command_builtin(), scope,
                                            spec);
}

bool input_keymap_apply_spec_registry(struct input_keymap *km,
                                      const struct input_command_registry *reg,
                                      uint8_t scope, const char *spec)
{
    if (!km || !spec || !reg || scope >= INPUT_COMMAND_SCOPE_COUNT)
        return false;
    if (!strcasecmp(spec, "clear")) {
        input_keymap_clear_scope(km, scope);
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

    input_command_id action;
    unsigned mods;
    uint32_t codepoint;
    uint16_t key;
    if (!parse_action_in(reg, scope, act, &action) ||
        !input_parse_chord(chord, &mods, &codepoint, &key))
        return false;
    if (action == INPUT_COMMAND_NONE) {
        for (size_t i = 0; i < km->n; i++) {
            struct input_keybind *b = &km->binds[i];
            if (b->scope == scope && b->mods == mods &&
                b->codepoint == codepoint && b->key == key) {
                km->n--;
                memmove(&km->binds[i], &km->binds[i + 1],
                        (km->n - i) * sizeof(km->binds[0]));
                km->hash_valid = false;
                return true;
            }
        }
        return true;
    }
    return keymap_set(km, scope, mods, codepoint, key, (uint16_t)action);
}

/* Looks up the chord of `ev` in one scope of `km`. */
static bool lookup_scope(const struct input_keymap *km, uint8_t scope,
                         unsigned mods, const struct input_key_event *ev,
                         input_command_id *out)
{
    if (ev->key != INPUT_KEY_NONE &&
        hash_lookup(km, make_chord(scope, mods, chord_token(0, (uint16_t)ev->key)), out))
        return true;
    if (ev->keypad != INPUT_KEY_NONE &&
        hash_lookup(km, make_chord(scope, mods, chord_token(0, (uint16_t)ev->keypad)), out))
        return true;
    if (ev->codepoint &&
        hash_lookup(km, make_chord(scope, mods, chord_token(ev->codepoint, INPUT_KEY_NONE)), out))
        return true;
    if (ev->shifted && lower(ev->shifted) != lower(ev->codepoint) &&
        hash_lookup(km, make_chord(scope, mods, chord_token(ev->shifted, INPUT_KEY_NONE)), out))
        return true;
    if (ev->base && lower(ev->base) != lower(ev->codepoint) &&
        hash_lookup(km, make_chord(scope, mods, chord_token(ev->base, INPUT_KEY_NONE)), out))
        return true;
    return false;
}

input_command_id input_keymap_lookup(const struct input_keymap *km,
                                        const struct input_key_event *ev)
{
    return input_keymap_lookup_scoped(km, ev, INPUT_COMMAND_SCOPE_APP);
}

input_command_id input_keymap_lookup_scoped(const struct input_keymap *km,
                                               const struct input_key_event *ev,
                                               uint8_t scope)
{
    if (!km || !ev || scope >= INPUT_COMMAND_SCOPE_COUNT)
        return INPUT_COMMAND_NONE;
    unsigned mods = ev->mods & ~(unsigned)(INPUT_MOD_CAPS | INPUT_MOD_NUM);
    if (!km->hash_valid && !rebuild_hash((struct input_keymap *)km))
        return INPUT_COMMAND_NONE;

    input_command_id sc;
    if (scope != INPUT_COMMAND_SCOPE_APP && lookup_scope(km, scope, mods, ev, &sc))
        return sc;
    if (lookup_scope(km, INPUT_COMMAND_SCOPE_APP, mods, ev, &sc))
        return sc;
    return INPUT_COMMAND_NONE;
}

static void add_bind(struct input_keymap *km, unsigned mods, uint32_t cp,
                     uint16_t key, input_command_id action)
{
    keymap_set(km, INPUT_COMMAND_SCOPE_APP, mods, cp, key, (uint16_t)action);
}

void input_keymap_defaults(struct input_keymap *km,
                           const struct input_shortcut_policy *p,
                           enum input_keybind_style style)
{
    if (!km || !p)
        return;
    input_keymap_init(km);

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
        /* A policy with no alternate accel gives no binding. A zero modifier
         * set would bind the bare letter. */
        if (!accels[i])
            continue;
        add_bind(km, accels[i], 'c', INPUT_KEY_NONE, INPUT_COMMAND_COPY);
        add_bind(km, accels[i], 'v', INPUT_KEY_NONE, INPUT_COMMAND_PASTE);
    }
#ifdef __APPLE__
    add_bind(km, INPUT_MOD_SUPER, 'a', INPUT_KEY_NONE, INPUT_COMMAND_SELECT_ALL);
    add_bind(km, INPUT_MOD_SUPER, 0, INPUT_KEY_BACKSPACE, INPUT_COMMAND_KILL_LINE);
#endif

#ifdef __APPLE__
    add_bind(km, p->zoom, '+', INPUT_KEY_NONE, INPUT_COMMAND_FONT_LARGER);
    add_bind(km, p->zoom, '=', INPUT_KEY_NONE, INPUT_COMMAND_FONT_LARGER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_ADD, INPUT_COMMAND_FONT_LARGER);
    add_bind(km, p->zoom, '-', INPUT_KEY_NONE, INPUT_COMMAND_FONT_SMALLER);
    add_bind(km, p->zoom, '_', INPUT_KEY_NONE, INPUT_COMMAND_FONT_SMALLER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_SUBTRACT, INPUT_COMMAND_FONT_SMALLER);
#else
    add_bind(km, INPUT_MOD_CTRL, '=', INPUT_KEY_NONE,
             INPUT_COMMAND_FONT_LARGER);
    add_bind(km, INPUT_MOD_CTRL, '-', INPUT_KEY_NONE,
             INPUT_COMMAND_FONT_SMALLER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_ADD, INPUT_COMMAND_FONT_LARGER);
    add_bind(km, p->zoom, 0, INPUT_KEY_KP_SUBTRACT,
             INPUT_COMMAND_FONT_SMALLER);
#endif

#ifdef __APPLE__
    add_bind(km, INPUT_MOD_SUPER | INPUT_MOD_SHIFT, 'p', INPUT_KEY_NONE,
             INPUT_COMMAND_COMMAND_PALETTE);
#else
    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 'p', INPUT_KEY_NONE,
             INPUT_COMMAND_COMMAND_PALETTE);
#endif

    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 0, INPUT_KEY_F12,
             INPUT_COMMAND_DEBUG_OVERLAY);
#ifdef __APPLE__
    add_bind(km, INPUT_MOD_SUPER, ',', INPUT_KEY_NONE, INPUT_COMMAND_SETTINGS);
#else
    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, ',', INPUT_KEY_NONE,
             INPUT_COMMAND_SETTINGS);
#endif

    add_bind(km, INPUT_MOD_SHIFT, 0, INPUT_KEY_INSERT, INPUT_COMMAND_PASTE);
    add_bind(km, INPUT_MOD_SHIFT, 0, INPUT_KEY_PAGE_UP, INPUT_COMMAND_SCROLL_UP);
    add_bind(km, INPUT_MOD_SHIFT, 0, INPUT_KEY_PAGE_DOWN,
             INPUT_COMMAND_SCROLL_DOWN);

    unsigned tab = p->tab_accel ? p->tab_accel : p->accel;
    if (!tab)
        return;

    add_bind(km, tab, 't', INPUT_KEY_NONE, INPUT_COMMAND_NEW_TAB);
    add_bind(km, tab, 'w', INPUT_KEY_NONE, INPUT_COMMAND_CLOSE_TAB);
    for (int i = 0; i < 8; i++)
        add_bind(km, tab, (uint32_t)('1' + i), INPUT_KEY_NONE,
                 (input_command_id)(INPUT_COMMAND_TAB_1 + i));
    add_bind(km, tab, '9', INPUT_KEY_NONE, INPUT_COMMAND_LAST_TAB);

    add_bind(km, INPUT_MOD_CTRL, 0, INPUT_KEY_TAB, INPUT_COMMAND_NEXT_TAB);
    add_bind(km, INPUT_MOD_CTRL | INPUT_MOD_SHIFT, 0, INPUT_KEY_TAB,
             INPUT_COMMAND_PREV_TAB);
    add_bind(km, tab, 0, INPUT_KEY_PAGE_DOWN, INPUT_COMMAND_NEXT_TAB);
    add_bind(km, tab, 0, INPUT_KEY_PAGE_UP, INPUT_COMMAND_PREV_TAB);
    add_bind(km, tab, 0, INPUT_KEY_F6, INPUT_COMMAND_FOCUS_TABS);
}

void input_keymap_reset(struct input_keymap *km, enum input_keybind_style style)
{
    if (!km)
        return;
    input_keymap_clear_scope(km, INPUT_COMMAND_SCOPE_APP);
    struct input_keymap defaults;
    input_keymap_defaults(&defaults, &input_platform_shortcuts, style);
    for (size_t i = 0; i < defaults.n; i++) {
        const struct input_keybind *b = &defaults.binds[i];
        keymap_set(km, b->scope, b->mods, b->codepoint, b->key, b->action);
    }
    input_keymap_free(&defaults);
}
