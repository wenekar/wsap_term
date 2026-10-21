#include "wsap_vtcore/wsap_vtcore.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wsap_vtcore_keyboard_state st;
static struct wsap_vtcore_modes m;
static char out[128];
static size_t out_len;

static void mode(unsigned kitty_flags)
{
    st = (struct wsap_vtcore_keyboard_state){0};
    m = (struct wsap_vtcore_modes){ .keyboard = kitty_flags, .grid = { .x = 80, .y = 24 } };
}

static void enc(struct wsap_vtcore_key_event ev)
{
    if (!ev.action) ev.action = WSAP_VTCORE_KEY_PRESS;
    wsap_vtcore_keyboard_update(&st, &ev);
    struct wsap_vtcore_input_result r =
        wsap_vtcore_input_encode_key(&st, &ev, &m, out, sizeof(out));
    out_len = r.len;
}
#define K(...) enc((struct wsap_vtcore_key_event){ __VA_ARGS__ })

static void typed(uint32_t cp, unsigned mods)
{
    K(.codepoint = cp, .base = cp, .text = cp, .mods = mods);
}

static bool consumed;
static void enc_mouse(struct wsap_vtcore_mouse_event ev)
{
    if (ev.pixel.x == 0 && ev.pixel.y == 0)
        ev.pixel = ev.pos;
    struct wsap_vtcore_input_result r =
        wsap_vtcore_input_encode_mouse(&ev, &m, out, sizeof(out));
    out_len = r.len;
    consumed = r.status != WSAP_VTCORE_INPUT_UNHANDLED;
}
#define MOUSE(x_, y_, ...) enc_mouse((struct wsap_vtcore_mouse_event){ .pos = { .x = (x_), .y = (y_) }, __VA_ARGS__ })

static void expect_at(const char *s, int line)
{
    if (out_len != strlen(s) || memcmp(out, s, out_len)) {
        fprintf(stderr, "line %d: expected %s, got %.*s (%zu bytes)\n", line, s,
                (int)out_len, out, out_len);
        abort();
    }
}
#define expect(s) expect_at(s, __LINE__)

int main(void)
{
    mode(0);
    K(.key = WSAP_VTCORE_KEY_UP); expect("\033[A");
    K(.key = WSAP_VTCORE_KEY_HOME); expect("\033[H");
    K(.key = WSAP_VTCORE_KEY_DELETE); expect("\033[3~");
    K(.key = WSAP_VTCORE_KEY_PAGE_UP); expect("\033[5~");
    K(.key = WSAP_VTCORE_KEY_F1); expect("\033OP");
    K(.key = WSAP_VTCORE_KEY_F5); expect("\033[15~");
    K(.key = WSAP_VTCORE_KEY_F12); expect("\033[24~");
    K(.key = WSAP_VTCORE_KEY_RETURN); expect("\r");
    K(.key = WSAP_VTCORE_KEY_ESCAPE); expect("\033");
    K(.key = WSAP_VTCORE_KEY_TAB); expect("\t");
    K(.key = WSAP_VTCORE_KEY_TAB, .mods = WSAP_VTCORE_MOD_SHIFT); expect("\033[Z");
    K(.key = WSAP_VTCORE_KEY_BACKSPACE); expect("\177");
    K(.key = WSAP_VTCORE_KEY_RIGHT, .mods = WSAP_VTCORE_MOD_CTRL); expect("\033[1;5C");
    K(.key = WSAP_VTCORE_KEY_F5, .mods = WSAP_VTCORE_MOD_SHIFT); expect("\033[15;2~");
    K(.key = WSAP_VTCORE_KEY_DELETE, .mods = WSAP_VTCORE_MOD_CTRL); expect("\033[3;5~");

    m.backarrow_bs = true;
    K(.key = WSAP_VTCORE_KEY_BACKSPACE); expect("\010");
    m.backarrow_bs = false;

    m.app_cursor = true;
    K(.key = WSAP_VTCORE_KEY_UP); expect("\033OA");
    K(.key = WSAP_VTCORE_KEY_END); expect("\033OF");

    K(.key = WSAP_VTCORE_KEY_UP, .mods = WSAP_VTCORE_MOD_CTRL); expect("\033[1;5A");
    m.app_cursor = false;

    typed('a', 0); expect("a");
    typed(0x00E9, 0); expect("\xc3\xa9");
    K(.codepoint = 'a', .shifted = 'A', .base = 'a', .text = 'A',
      .mods = WSAP_VTCORE_MOD_SHIFT); expect("A");
    K(.action = WSAP_VTCORE_KEY_RELEASE, .codepoint = 'a', .text = 'a'); expect("");

    typed('c', WSAP_VTCORE_MOD_CTRL); expect("\003");
    typed('a', WSAP_VTCORE_MOD_CTRL); expect("\001");
    typed('\\', WSAP_VTCORE_MOD_CTRL); expect("\034");
    typed(' ', WSAP_VTCORE_MOD_CTRL); assert(out_len == 1 && out[0] == 0);

    K(.codepoint = '2', .shifted = '@', .base = '2', .text = '@',
      .mods = WSAP_VTCORE_MOD_CTRL | WSAP_VTCORE_MOD_SHIFT);
    assert(out_len == 1 && out[0] == 0);

    typed('b', WSAP_VTCORE_MOD_ALT); expect("\033b");
    typed('b', WSAP_VTCORE_MOD_ALT | WSAP_VTCORE_MOD_CTRL); expect("\033\002");
    K(.key = WSAP_VTCORE_KEY_BACKSPACE, .mods = WSAP_VTCORE_MOD_ALT); expect("\033\177");
    K(.key = WSAP_VTCORE_KEY_RETURN, .mods = WSAP_VTCORE_MOD_ALT); expect("\033\r");
    K(.key = WSAP_VTCORE_KEY_TAB, .mods = WSAP_VTCORE_MOD_ALT); expect("\033\t");
    K(.key = WSAP_VTCORE_KEY_TAB, .mods = WSAP_VTCORE_MOD_ALT | WSAP_VTCORE_MOD_SHIFT); expect("\033[Z");
    K(.key = WSAP_VTCORE_KEY_ESCAPE, .mods = WSAP_VTCORE_MOD_ALT); expect("\033");

    K(.key = WSAP_VTCORE_KEY_LEFT, .mods = WSAP_VTCORE_MOD_ALT); expect("\033[1;3D");

    {
        char b_null[16];
        struct wsap_vtcore_input_result r = wsap_vtcore_input_encode_key(
            NULL,
            &(struct wsap_vtcore_key_event){ .key = WSAP_VTCORE_KEY_UP, .action = WSAP_VTCORE_KEY_PRESS },
            &(struct wsap_vtcore_modes){0},
            b_null, sizeof(b_null));
        assert(r.status == WSAP_VTCORE_INPUT_BYTES && r.len == 3 && !memcmp(b_null, "\033[A", 3));
    }

    K(.codepoint = 'b', .base = 'b', .text = 0x222B, .text_deferred = true);
    expect("");

    K(.key = WSAP_VTCORE_KEY_KP_DELETE, .keypad = WSAP_VTCORE_KEY_KP_DECIMAL); expect("\033[3~");
    K(.key = WSAP_VTCORE_KEY_KP_ENTER, .keypad = WSAP_VTCORE_KEY_KP_ENTER); expect("\r");
    K(.key = WSAP_VTCORE_KEY_KP_LEFT, .keypad = WSAP_VTCORE_KEY_KP_4); expect("\033[D");

    m.app_keypad = true;
    K(.key = WSAP_VTCORE_KEY_KP_END, .keypad = WSAP_VTCORE_KEY_KP_1); expect("\033Oq");
    K(.key = WSAP_VTCORE_KEY_KP_ENTER, .keypad = WSAP_VTCORE_KEY_KP_ENTER); expect("\033OM");
    K(.key = WSAP_VTCORE_KEY_KP_ADD, .keypad = WSAP_VTCORE_KEY_KP_ADD); expect("\033Ok");
    m.app_keypad = false;

    mode(1);
    K(.key = WSAP_VTCORE_KEY_ESCAPE); expect("\033[27u");
    K(.key = WSAP_VTCORE_KEY_DELETE); expect("\033[3~");
    K(.key = WSAP_VTCORE_KEY_F3); expect("\033[13~");
    K(.key = WSAP_VTCORE_KEY_KP_ENTER, .keypad = WSAP_VTCORE_KEY_KP_ENTER); expect("\033[57414u");
    typed('i', WSAP_VTCORE_MOD_CTRL); expect("\033[105;5u");
    K(.key = WSAP_VTCORE_KEY_RETURN, .mods = WSAP_VTCORE_MOD_CTRL); expect("\033[13;5u");

    typed('a', 0); expect("a");
    K(.key = WSAP_VTCORE_KEY_RETURN); expect("\r");

    K(.key = WSAP_VTCORE_KEY_UP, .action = WSAP_VTCORE_KEY_RELEASE); expect("");
    K(.key = WSAP_VTCORE_KEY_LSHIFT); expect("");

    mode(1 | 4);
    K(.codepoint = 'a', .shifted = 'A', .base = 'a', .text = 'A',
      .mods = WSAP_VTCORE_MOD_CTRL | WSAP_VTCORE_MOD_SHIFT); expect("\033[97:65;6u");

    mode(1 | 2 | 8);
    K(.key = WSAP_VTCORE_KEY_ESCAPE); expect("\033[27;1u");
    K(.key = WSAP_VTCORE_KEY_ESCAPE, .action = WSAP_VTCORE_KEY_REPEAT); expect("\033[27;1:2u");
    K(.key = WSAP_VTCORE_KEY_ESCAPE, .action = WSAP_VTCORE_KEY_RELEASE); expect("\033[27;1:3u");
    K(.key = WSAP_VTCORE_KEY_UP, .action = WSAP_VTCORE_KEY_REPEAT); expect("\033[1;1:2A");

    mode(31);
    K(.codepoint = 1092, .base = 'a', .text = 1092);
    expect("\033[1092::97;1;1092u");
    K(.codepoint = 'a', .shifted = 'A', .base = 'a', .text = 'A',
      .mods = WSAP_VTCORE_MOD_SHIFT); expect("\033[97:65;2;65u");

    K(.action = WSAP_VTCORE_KEY_RELEASE, .codepoint = 'a', .base = 'a', .text = 'a');
    expect("\033[97;1:3u");

    mode(1 | 2 | 8);
    K(.key = WSAP_VTCORE_KEY_LSHIFT, .mods = WSAP_VTCORE_MOD_SHIFT); expect("\033[57441;2u");
    K(.key = WSAP_VTCORE_KEY_RSHIFT, .mods = WSAP_VTCORE_MOD_SHIFT); expect("\033[57447;2u");
    K(.key = WSAP_VTCORE_KEY_LSHIFT, .action = WSAP_VTCORE_KEY_RELEASE, .mods = WSAP_VTCORE_MOD_SHIFT);
    expect("\033[57441;2:3u");
    K(.key = WSAP_VTCORE_KEY_RSHIFT, .action = WSAP_VTCORE_KEY_RELEASE, .mods = WSAP_VTCORE_MOD_SHIFT);
    expect("\033[57447;1:3u");

    K(.key = WSAP_VTCORE_KEY_ISO_LEVEL3_SHIFT); expect("\033[57453;1u");
    K(.key = WSAP_VTCORE_KEY_ISO_LEVEL5_SHIFT); expect("\033[57454;1u");
    K(.key = WSAP_VTCORE_KEY_CAPS_LOCK); expect("\033[57358;65u");

    mode(1 | 2 | 4);
    K(.key = WSAP_VTCORE_KEY_CAPS_LOCK); expect("");
    K(.key = WSAP_VTCORE_KEY_CAPS_LOCK, .action = WSAP_VTCORE_KEY_RELEASE); expect("");

    mode(1 | 2 | 4 | 8);
    K(.key = WSAP_VTCORE_KEY_CAPS_LOCK); expect("\033[57358;65u");
    K(.key = WSAP_VTCORE_KEY_CAPS_LOCK, .action = WSAP_VTCORE_KEY_RELEASE,
      .mods = WSAP_VTCORE_MOD_CAPS); expect("\033[57358;65:3u");

    mode(1);
    K(.key = WSAP_VTCORE_KEY_LEFT, .mods = WSAP_VTCORE_MOD_NUM); expect("\033[1;129D");
    K(.key = WSAP_VTCORE_KEY_LEFT); expect("\033[D");
    K(.key = WSAP_VTCORE_KEY_F1, .mods = WSAP_VTCORE_MOD_CAPS); expect("\033[1;65P");
    K(.codepoint = 'a', .base = 'a', .mods = WSAP_VTCORE_MOD_CTRL | WSAP_VTCORE_MOD_CAPS);
    expect("\033[97;69u");
    K(.codepoint = 'a', .base = 'a', .mods = WSAP_VTCORE_MOD_CTRL);
    expect("\033[97;5u");
    K(.codepoint = 'a', .base = 'a', .text = 'A', .mods = WSAP_VTCORE_MOD_CAPS);
    expect("A");
    K(.key = WSAP_VTCORE_KEY_RETURN, .mods = WSAP_VTCORE_MOD_CAPS); expect("\r");

    mode(1 | 8);
    K(.codepoint = 'a', .base = 'a', .text = 'A', .mods = WSAP_VTCORE_MOD_CAPS);
    expect("\033[97;65u");
    K(.key = WSAP_VTCORE_KEY_RETURN, .mods = WSAP_VTCORE_MOD_CAPS); expect("\033[13;65u");

    mode(0);
    K(.key = WSAP_VTCORE_KEY_LEFT, .mods = WSAP_VTCORE_MOD_NUM); expect("\033[D");
    K(.key = WSAP_VTCORE_KEY_F1, .mods = WSAP_VTCORE_MOD_CAPS); expect("\033OP");
    K(.codepoint = 'a', .base = 'a', .mods = WSAP_VTCORE_MOD_CTRL | WSAP_VTCORE_MOD_CAPS);
    expect("\001");

    mode(0);
    K(.codepoint = '8', .text_utf8 = "[", .text_len = 1, .text_deferred = true);
    expect("");
    mode(WSAP_VTCORE_KEYBOARD_DISAMBIGUATE);
    K(.codepoint = '8', .text_utf8 = "[", .text_len = 1, .text_deferred = true);
    expect("");
    mode(WSAP_VTCORE_KEYBOARD_ALL_KEYS | WSAP_VTCORE_KEYBOARD_ASSOCIATED_TEXT);
    K(.codepoint = '8', .text_utf8 = "[", .text_len = 1, .text_deferred = true);
    expect("\033[56;1;91u");
    K(.codepoint = 'a', .text_utf8 = "ä😀", .text_len = strlen("ä😀"), .text_deferred = true);
    expect("\033[97;1;97:776:128512u");
    K(.text_utf8 = "漢字", .text_len = strlen("漢字"));
    expect("\033[0;1;28450:23383u");
    mode(0);
    K(.text_utf8 = "漢字", .text_len = strlen("漢字"));
    expect("漢字");

    mode(0);
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_PRESS);
    assert(out_len == 0 && !consumed);

    m.mouse = WSAP_VTCORE_MOUSE_NORMAL;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_PRESS); expect("\033[M\040\043\042");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_RELEASE); expect("\033[M\043\043\042");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_MOTION); expect("");

    MOUSE(2, 1, .mods = WSAP_VTCORE_MOD_SHIFT);
    assert(out_len == 0 && !consumed);

    MOUSE(200, 99, .kind = WSAP_VTCORE_MOUSE_PRESS);
    assert(out_len == 0 && consumed);

    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_SGR;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_PRESS); expect("\033[<0;3;2M");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_RELEASE); expect("\033[<0;3;2m");
    MOUSE(2, 1, .mods = WSAP_VTCORE_MOD_CTRL); expect("\033[<16;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_WHEEL_UP); expect("\033[<64;3;2M");
    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_SGR_PIXELS;
    MOUSE(2, 1, .pixel = { .x = 25, .y = 17 }); expect("\033[<0;26;18M");
    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_URXVT;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_PRESS); expect("\033[32;3;2M");
    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_X10;

    m.mouse = WSAP_VTCORE_MOUSE_BUTTON;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_MOTION, .button = WSAP_VTCORE_MOUSE_BUTTON_NONE); expect("");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_MOTION, .button = WSAP_VTCORE_MOUSE_BUTTON_PRIMARY);
    expect("\033[M\100\043\042");
    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_SGR;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_MOTION, .button = WSAP_VTCORE_MOUSE_BUTTON_NONE); expect("");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_MOTION, .button = WSAP_VTCORE_MOUSE_BUTTON_PRIMARY, .mods = WSAP_VTCORE_MOD_CTRL);
    expect("\033[<48;3;2M");
    m.mouse = WSAP_VTCORE_MOUSE_ANY;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_MOTION, .button = WSAP_VTCORE_MOUSE_BUTTON_NONE); expect("\033[<35;3;2M");
    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_X10;
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_WHEEL_UP); expect("\033[M\140\043\042");

    m.mouse = WSAP_VTCORE_MOUSE_X10;
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_PRESS); expect("\033[M\040\043\042");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_RELEASE); expect("");

    {
        struct wsap_vtcore_keyboard_state s2 = {0};
        struct wsap_vtcore_modes m2 = { .keyboard = 1 | 2 | 8, .grid = { .x = 80, .y = 24 } };
        char b2[64];

        struct wsap_vtcore_key_event sh = {
            .key = WSAP_VTCORE_KEY_LSHIFT, .action = WSAP_VTCORE_KEY_PRESS,
            .mods = WSAP_VTCORE_MOD_SHIFT,
        };
        wsap_vtcore_keyboard_update(&s2, &sh);
        struct wsap_vtcore_key_event rsh = {
            .key = WSAP_VTCORE_KEY_RSHIFT, .action = WSAP_VTCORE_KEY_PRESS,
            .mods = WSAP_VTCORE_MOD_SHIFT,
        };
        wsap_vtcore_keyboard_update(&s2, &rsh);

        struct wsap_vtcore_key_event rel = {
            .key = WSAP_VTCORE_KEY_LSHIFT, .action = WSAP_VTCORE_KEY_RELEASE,
            .mods = WSAP_VTCORE_MOD_SHIFT,
        };
        wsap_vtcore_keyboard_update(&s2, &rel);
        struct wsap_vtcore_input_result r =
            wsap_vtcore_input_encode_key(&s2, &rel, &m2, b2, sizeof(b2));
        assert(r.status == WSAP_VTCORE_INPUT_BYTES);
        assert(!memcmp(b2, "\033[57441;2:3u", r.len));

        struct wsap_vtcore_key_event rrel = {
            .key = WSAP_VTCORE_KEY_RSHIFT, .action = WSAP_VTCORE_KEY_RELEASE,
            .mods = WSAP_VTCORE_MOD_SHIFT,
        };
        wsap_vtcore_keyboard_update(&s2, &rrel);
        r = wsap_vtcore_input_encode_key(&s2, &rrel, &m2, b2, sizeof(b2));
        assert(!memcmp(b2, "\033[57447;1:3u", r.len));
    }

    {
        char tiny[4];
        struct wsap_vtcore_input_result r = wsap_vtcore_input_encode_key(
            &st,
            &(struct wsap_vtcore_key_event){
                .key = WSAP_VTCORE_KEY_DELETE, .action = WSAP_VTCORE_KEY_PRESS,
                .mods = WSAP_VTCORE_MOD_CTRL | WSAP_VTCORE_MOD_ALT | WSAP_VTCORE_MOD_SHIFT,
            },
            &(struct wsap_vtcore_modes){ .grid = { .x = 80, .y = 24 } },
            tiny, sizeof(tiny));
        assert(r.status == WSAP_VTCORE_INPUT_BUFFER_FULL && r.len == 0);
    }

    {
        struct wsap_vtcore_keyboard_state s5 = {0};
        struct wsap_vtcore_key_event bad = {
            .key = UINT16_MAX, .action = WSAP_VTCORE_KEY_PRESS,
        };
        wsap_vtcore_keyboard_update(&s5, &bad);
        assert(!wsap_vtcore_keyboard_key_down(&s5, WSAP_VTCORE_KEY_LSHIFT));
        struct wsap_vtcore_input_result r =
            wsap_vtcore_input_encode_key(&s5, &bad, &m, out, sizeof(out));
        assert(r.status == WSAP_VTCORE_INPUT_UNHANDLED && r.len == 0);

        struct wsap_vtcore_key_event bad_kp = {
            .key = WSAP_VTCORE_KEY_KP_1, .keypad = UINT16_MAX,
            .action = WSAP_VTCORE_KEY_PRESS,
        };
        m.app_keypad = true;
        r = wsap_vtcore_input_encode_key(&s5, &bad_kp, &m, out, sizeof(out));
        assert(r.len == 0);
        m.app_keypad = false;
    }

    {
        struct wsap_vtcore_key_event deferred = {
            .action = WSAP_VTCORE_KEY_PRESS, .codepoint = 'a', .text = 'a',
            .text_deferred = true,
        };
        struct wsap_vtcore_input_result r = wsap_vtcore_input_encode_key(
            &st, &deferred, &(struct wsap_vtcore_modes){0}, out, sizeof(out));
        assert(r.status == WSAP_VTCORE_INPUT_UNHANDLED && r.len == 0);
        struct wsap_vtcore_modes kitty = { .keyboard = WSAP_VTCORE_KEYBOARD_ALL_KEYS };
        r = wsap_vtcore_input_encode_key(&st, &deferred, &kitty, out, sizeof(out));
        assert(r.status == WSAP_VTCORE_INPUT_BYTES && r.len == strlen("\033[97u"));
        assert(!memcmp(out, "\033[97u", r.len));
        deferred.action = WSAP_VTCORE_KEY_RELEASE;
        r = wsap_vtcore_input_encode_key(&st, &deferred, &kitty, out, sizeof(out));
        assert(r.status == WSAP_VTCORE_INPUT_CONSUMED && r.len == 0);
    }
    m.mouse = WSAP_VTCORE_MOUSE_ANY;
    m.mouse_encoding = WSAP_VTCORE_MOUSE_ENC_SGR;
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_1); expect("\033[<128;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_2); expect("\033[<129;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_3); expect("\033[<130;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_4); expect("\033[<131;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_WHEEL_LEFT); expect("\033[<66;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_WHEEL_RIGHT); expect("\033[<67;3;2M");

    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_PRIMARY, .mods = WSAP_VTCORE_MOD_ALT); expect("\033[<8;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_1, .mods = WSAP_VTCORE_MOD_ALT); expect("\033[<136;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_MIDDLE, .mods = WSAP_VTCORE_MOD_ALT); expect("\033[<9;3;2M");
    MOUSE(2, 1, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_2, .mods = WSAP_VTCORE_MOD_ALT); expect("\033[<137;3;2M");
    MOUSE(2, 1, .kind = WSAP_VTCORE_MOUSE_RELEASE, .button = WSAP_VTCORE_MOUSE_BUTTON_EXTRA_1); expect("\033[<128;3;2m");

    {
        static const char text[] = "ls\r\n\033[31m\u202erm\x7f\t\xe4\xb8\xad";
        static const char want[] = "\033[200~ls\r[31mrm\t\xe4\xb8\xad\033[201~";
        char buf[sizeof(text) - 1 + WSAP_VTCORE_PASTE_EXTRA];
        struct wsap_vtcore_modes bracketed = { .bracketed_paste = true };
        struct wsap_vtcore_input_result r =
            wsap_vtcore_input_encode_paste(&bracketed, text, sizeof(text) - 1, buf, sizeof(buf));
        assert(r.status == WSAP_VTCORE_INPUT_BYTES && r.len == sizeof(want) - 1);
        assert(!memcmp(buf, want, r.len));

        r = wsap_vtcore_input_encode_paste(&bracketed, text, sizeof(text) - 1, buf, r.len - 1);
        assert(r.status == WSAP_VTCORE_INPUT_BUFFER_FULL && r.len == 0);

        r = wsap_vtcore_input_encode_paste(&bracketed, "\033\a\u2066", 5, buf, sizeof(buf));
        assert(r.status == WSAP_VTCORE_INPUT_CONSUMED && r.len == 0);

        struct wsap_vtcore_modes plain = {0};
        r = wsap_vtcore_input_encode_paste(&plain, "a\nb", 3, buf, 3);
        assert(r.status == WSAP_VTCORE_INPUT_BYTES && r.len == 3 && !memcmp(buf, "a\rb", 3));
    }
    {
        mode(WSAP_VTCORE_KEYBOARD_ALL_KEYS | WSAP_VTCORE_KEYBOARD_EVENT_TYPES);
        K(.key = WSAP_VTCORE_KEY_NUM_LOCK); expect("\033[57360;129u");
        K(.key = WSAP_VTCORE_KEY_NUM_LOCK, .action = WSAP_VTCORE_KEY_REPEAT,
          .mods = WSAP_VTCORE_MOD_NUM); expect("\033[57360;129:2u");
        K(.key = WSAP_VTCORE_KEY_NUM_LOCK, .action = WSAP_VTCORE_KEY_RELEASE,
          .mods = WSAP_VTCORE_MOD_NUM); expect("\033[57360;129:3u");
        K(.key = WSAP_VTCORE_KEY_NUM_LOCK, .mods = WSAP_VTCORE_MOD_NUM); expect("\033[57360;1u");
        K(.key = WSAP_VTCORE_KEY_SCROLL_LOCK); expect("\033[57359;1u");
    }

    {
        struct wsap_vtcore_key_event key = { .action = WSAP_VTCORE_KEY_PRESS, .text = 0x4e2d };
        struct wsap_vtcore_mouse_event mouse = {
            .kind = WSAP_VTCORE_MOUSE_PRESS, .button = WSAP_VTCORE_MOUSE_BUTTON_PRIMARY,
            .pos = { .x = 190, .y = 95 },
        };
        struct wsap_vtcore_modes modes = {
            .mouse = WSAP_VTCORE_MOUSE_ANY, .mouse_encoding = WSAP_VTCORE_MOUSE_ENC_UTF8,
            .grid = { .x = 256, .y = 128 },
        };
        static const char *const want[] = { "\xe4\xb8\xad", "\033[M \xc3\x9f\xc2\x80" };
        char buf[16];
        for (size_t which = 0; which < 2; which++) {
            size_t len = strlen(want[which]);
            for (size_t cap = 0; cap <= len + 1; cap++) {
                memset(buf, '!', sizeof(buf));
                struct wsap_vtcore_input_result r = which == 0
                    ? wsap_vtcore_input_encode_key(NULL, &key, &modes, buf, cap)
                    : wsap_vtcore_input_encode_mouse(&mouse, &modes, buf, cap);
                assert(buf[cap] == '!');
                if (cap < len) {
                    assert(r.status == WSAP_VTCORE_INPUT_BUFFER_FULL && r.len == 0);
                } else {
                    assert(r.status == WSAP_VTCORE_INPUT_BYTES && r.len == len);
                    assert(!memcmp(buf, want[which], len));
                }
            }
        }
    }
    puts("PASS: terminal key, mouse, and paste encoding");
    return 0;
}
