#ifndef WSAP_TERM_FUZZ_GEN_H
#define WSAP_TERM_FUZZ_GEN_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t rng;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

static int rn(int n)
{
    return (int)(rnd() % (uint32_t)n);
}

static char buf[1 << 16];
static size_t blen;

static void putb(char c)
{
    if (blen + 1 < sizeof(buf))
        buf[blen++] = c;
}

static void put(const char *s)
{
    size_t n = strlen(s);
    if (blen + n < sizeof(buf)) {
        memcpy(buf + blen, s, n);
        blen += n;
    }
}

static void __attribute__((format(printf, 1, 2))) putf(const char *fmt, ...)
{
    char tmp[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    put(tmp);
}

#define PICK(list) (list)[rn((int)(sizeof(list) / sizeof((list)[0])))]

static void gen_sgr(void)
{
    static const int plain[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 21, 22, 23, 24, 27, 29, 39, 49, 53, 55, 59,
    };
    put("\033[");
    int n = 1 + rn(3);
    for (int i = 0; i < n; i++) {
        if (i) put(";");
        switch (rn(9)) {
        case 0: putf("38;5;%d", rn(300)); break;
        case 1: putf("48;5;%d", rn(300)); break;
        case 2: putf("38;2;%d;%d;%d", rn(256), rn(256), rn(256)); break;
        case 3: putf("48:2::%d:%d:%d", rn(256), rn(256), rn(256)); break;
        case 4: putf("4:%d", rn(7)); break;
        case 5: putf("%d", 30 + rn(8) + (rn(2) ? 60 : 0)); break;
        case 6: putf("%d", 40 + rn(8) + (rn(2) ? 60 : 0)); break;
        case 7: putf("58;5;%d", rn(256)); break;
        default: putf("%d", PICK(plain)); break;
        }
    }
    put("m");
}

static inline void gen(int ops, int cols, int rows)
{
    static const char *words[] = {
        "a", "hello", "x y", "0123456789", "日本", "é", "中文字",
        "e\xcc\x81", "👍", "   ",
    };
    static const char *utf8_3[] = {
        "中", "文", "字", "€", "ｱ", "\xe2\x83\x90", "\xed\xa0\x80",
        "\xe0\x80\xaf", "\xef\xbf\xbd", "한", "a", "\xe2\x80\x8d", "\xe3\x80",
    };
    static const char *clusters[] = {
        "😀", "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa", "👍🏽", "👨‍👩‍👧", "❤️", "☺︎",
        "a\xcc\x81\xcc\x82", "\xe2\x80\x8b", "\xc2\x85", "\xc2\x84", "\xc2\x8d",
        "\xc2\x88", "\xf0\x9f\x87\xa9", "\xcc\x81",
    };
    static const char *modes[] = {
        "\033[4h", "\033[4l", "\033[?7l", "\033[?7h",
        "\033[?1049h", "\033[?1049l", "\033[?6h", "\033[?6l",
        "\033[20h", "\033[20l", "\033[?25l", "\033[?25h",
        "\033[?5h", "\033[?5l",
    };
    static const char *escs[] = {
        "\033D", "\033M", "\033E", "\0337", "\0338", "\033[s", "\033[u", "\033c",
        "\033[?1048h", "\033[?1048l", "\033H", "\033=", "\033>", "\033%G",
    };
    static const char *charsets[] = {
        "\033(0", "\033(B", "\033)0", "\033)B", "\x0e", "\x0f",
    };
    static const char *queries[] = {
        "\033[5n", "\033[6n", "\033[c", "\033[0c", "\033[>c", "\033[1c",
        "\033[14t", "\033P$qm\033\\", "\033P$qr\033\\", "\033[?6$p",
        "\033[4$p", "\033[20$p",
    };
    static const char *odd[] = {
        "\033[2\n;3H", "\033[1\bC", "\033[3\x18m", "\033[1\x1a;2H",
        "\033[\1772C", "\x0b", "\x0c", "\x7f", "\x18", "\x1a",
        "\033]0;title\007", "\033]2;x\033\\", "\033_apc\033\\", "\033^pm\007",
        "\033X\033\\", "\033]133;A\007", "\033]133;A\033\\", "\033]133;D\007",
        "\033#8",
    };
    static const char csi_final[] = "ABCDEFGHIJKLMPSTXZ@`abdefghlmnrsu";

    for (int i = 0; i < ops; i++) {
        switch (rn(36)) {
        case 0: case 1: case 2: put(PICK(words)); break;
        case 3: {
            int n = rn(cols * 2 + 2);
            for (int k = 0; k < n; k++)
                putb((char)(0x21 + rn(94)));
            break;
        }
        case 4: put("\r\n"); break;
        case 5: put("\n"); break;
        case 6: put("\r"); break;
        case 7: put("\t"); break;
        case 8: put("\b"); break;
        case 9: putf("\033[%d;%dH", rn(rows + 2), rn(cols + 2)); break;
        case 10: putf("\033[%d%c", rn(4), "JK"[rn(2)]); break;
        case 11: putf("\033[%d%c", rn(cols), "XP@LMST"[rn(7)]); break;
        case 12: putf("\033[%d;%dm", 30 + rn(8), 40 + rn(8)); break;
        case 13: put(rn(2) ? "\033[m" : "\033[0m"); break;
        case 14: putf("\033[48;2;%d;%d;1m", rn(256), rn(256)); break;
        case 15: putf("\033[%d%c", rn(cols + 2), csi_final[rn((int)sizeof(csi_final) - 1)]); break;
        case 16: putf("\033[%d;%dr", rn(rows) + 1, rn(rows) + 1); break;
        case 17: put(PICK(modes)); break;
        case 18: put(PICK(escs)); break;
        case 19: putf("\033[%d%c", rn(5), "IZb"[rn(3)]); break;
        case 20: putf("\033[%d;%d;1;1$r", rn(rows) + 1, rn(cols) + 1); break;
        case 21: put("\033[1;7m"); break;
        case 22: case 23: {
            int n = 64 + rn(400);
            for (int k = 0; k < n; k++) {
                unsigned char b = (unsigned char)rnd();
                putb((char)(b == 0x1b ? 'e' : b));
            }
            break;
        }
        case 24: case 25: {
            int n = 4 + rn(40);
            for (int k = 0; k < n; k++)
                put(utf8_3[rn(20) < 14 ? rn(3) : rn(13)]);
            break;
        }
        case 26: case 27: gen_sgr(); break;
        case 28: put(PICK(clusters)); break;
        case 29: put(PICK(charsets)); break;
        case 30: put(PICK(queries)); break;
        case 31: put(PICK(odd)); break;
        case 32: putf("\033[%d q", rn(7)); break;
        case 33: putf("\033[%dg", rn(4)); break;
        case 34:
            putf("\033[%d;%d;%d;%d;%d$r", rn(rows) + 1, rn(cols) + 1,
                 rn(rows) + 1, rn(cols) + 1, PICK(((const int[]){ 0, 1, 4, 7, 22, 24, 27, 31, 42 })));
            break;
        case 35: putf("\033[%d*x", rn(3)); break;
        }
    }
}

#endif
