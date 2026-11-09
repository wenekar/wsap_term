#ifndef WSAP_TERM_FUZZ_GEN_H
#define WSAP_TERM_FUZZ_GEN_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Each thread has its own generator state. */
static _Thread_local uint64_t rng;

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

static _Thread_local char buf[1 << 16];
static _Thread_local size_t blen;

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

/* Emit sequences that the model does not implement: OSC 8 links and
 * XTGETTCAP at buffer-edge lengths. The model test leaves this off.
 */
static bool gen_unmodeled;

/* Parameters at the edges of the parsers' digit limits. */
static const char *big_number(void)
{
    static const char *values[] = {
        "255", "256", "65535", "999999", "1000000", "1234567",
        "2147483647", "2147483648", "99999999999999999999",
    };
    return PICK(values);
}

static void gen_sgr(void)
{
    static const int plain[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 21, 22, 23, 24, 25, 27, 28, 29, 39, 49, 53, 55, 59,
    };
    put("\033[");
    int n = 1 + rn(3);
    for (int i = 0; i < n; i++) {
        if (i) put(";");
        switch (rn(11)) {
        case 0: putf("38;5;%d", rn(300)); break;
        case 1: putf("48;5;%d", rn(300)); break;
        case 2: putf("38;2;%d;%d;%d", rn(256), rn(256), rn(256)); break;
        case 3: putf("48:2::%d:%d:%d", rn(256), rn(256), rn(256)); break;
        case 4: putf("4:%d", rn(7)); break;
        case 5: putf("%d", 30 + rn(8) + (rn(2) ? 60 : 0)); break;
        case 6: putf("%d", 40 + rn(8) + (rn(2) ? 60 : 0)); break;
        case 7: putf("58;5;%d", rn(256)); break;
        case 8: putf("%d8;2;%d;%s;%d", 3 + rn(3), rn(256), big_number(), rn(256)); break;
        case 9: putf("58:2::%d:%d:%d", rn(256), rn(256), rn(256)); break;
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
        "\033[4$p", "\033[20$p", "\033[16t", "\033[18t", "\033[19;0t", "\033[?6n",
        "\033[=c", "\033[=0c", "\033[22;0t", "\033[23;2t", "\033[22t", "\033[23t",
    };
    static const char *odd[] = {
        "\033[2\n;3H", "\033[1\bC", "\033[3\x18m", "\033[1\x1a;2H",
        "\033[\1772C", "\x0b", "\x0c", "\x7f", "\x18", "\x1a",
        "\033]0;title\007", "\033]2;x\033\\", "\033_apc\033\\", "\033^pm\007",
        "\033X\033\\", "\033]133;A\007", "\033]133;A\033\\", "\033]133;D\007",
        "\033]133;C\007", "\033]133;D;7\007",
        "\033]9;4;1;50\007", "\033]9;4;0\007", "\033]9;4;9;5\007",
        "\033]9;9;/tmp/x\007", "\033]1337;CurrentDir=/tmp/x\007",
        "\033]1337;SetUserVar=k=YQ==\007", "\033]1337;SetUserVar=bad=!\007",
        "\033#8",
    };
    static const char csi_final[] = "ABCDEFGHIJKLMPSTXZ@`abdefghlmnrsu";

    for (int i = 0; i < ops; i++) {
        switch (rn(49)) {
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
        case 36:
            putf("\033[%s;%s%c", big_number(), big_number(),
                 csi_final[rn((int)sizeof(csi_final) - 1)]);
            break;
        case 37: case 38: {
            if (!gen_unmodeled)
                break;
            static const int edges[] = { 254, 255, 256, 257, 4094, 4095, 4096, 4097 };
            int osc = rn(2), n = PICK(edges) - 3;
            put(osc ? "\033]8;;" : "\033P+q");
            for (int k = 0; k < n; k++)
                putb("0123456789abcdef"[rn(16)]);
            put(rn(2) ? "\007" : "\033\\");
            put(PICK(words));
            put(osc ? "\033]8;;\007" : "");
            break;
        }
        case 39: case 40: {
            /* Runs of plain lines take the line streams when 1 KiB follows. */
            const char *eol = rn(3) ? "\r\n" : "\n";
            int lines = 20 + rn(120);
            for (int k = 0; k < lines; k++) {
                int n = rn(cols * 2 + 1);
                for (int c = 0; c < n; c++)
                    putb((char)(0x21 + rn(94)));
                put(eol);
            }
            break;
        }
        case 41: {
            /* Forms that the fast CSI path takes. Cut one short, then cancel it,
             * end it, or let the next operation continue it. */
            static const char *forms[] = {
                "\033[0;38;2;12;34;56m", "\033[1;48;5;200m", "\033[38;5;9;48;2;1;2;3m",
                "\033[48;2;7;8;9m\033[38;5;100m", "\033[38;2;1;2;3;4m", "\033[1;1H",
                "\033[H", "\033[?25l", "\033[?2026h", "\033[?7l", "\033[?7h", "\033[22m",
                "\033[97m", "\033[104m", "\033[7m", "\033[2J", "\033[1K",
            };
            static const char *after[] = { "\x18", "\x1a", "\033D", "", "\033[m", "x", "m" };
            const char *form = PICK(forms);
            int cut = 2 + rn((int)strlen(form) - 1);
            for (int k = 0; k < cut; k++)
                putb(form[k]);
            put(PICK(after));
            break;
        }
        case 42:
            /* Left and right margins with the other operations. */
            if (!gen_unmodeled)
                break;
            if (rn(3)) {
                int left = 1 + rn(cols), right = left + rn(cols + 1 - left);
                putf("\033[?69h\033[%d;%ds", left, right);
            } else {
                put("\033[?69l");
            }
            break;
        case 43: {
            /* Report modes and queries that the model does not know. */
            if (!gen_unmodeled)
                break;
            static const char *reports[] = {
                "\033[?2048h", "\033[?2048l", "\033[?2031h", "\033[?2031l", "\033[?996n",
                "\033[?2048$p", "\033[?2031$p", "\033[?2031;2048h", "\033]2;t\007\033[22t",
                "\033]133;C\007x\033]133;A\007",
            };
            if (rn(3))
                put(PICK(reports));
            else
                putf("\033[%d;%s;%dt", 22 + rn(2), rn(2) ? big_number() : "1", rn(2));
            break;
        }
        case 44: {
            /* Rectangle and column edits. A rectangle parameter can be empty. */
            static const int fills[] = { 32, 65, 120, 126, 160, 233, 255, 31, 127, 256 };
            static const char *reverse[] = { "", ";0", ";1", ";4", ";5", ";7", ";8", ";1;7", ";7;7", ";0;4" };
            static const char *change[] = { "7;31", "31;7", "7;7", "0;7", "1;7;42", "7;0", "4;7;39",
                                            "5", "8", "5;8", "25", "28", "0;5" };
            char rect[48];
            int n = 0;
            for (int k = 0; k < 4; k++) {
                if (k)
                    rect[n++] = ';';
                if (rn(5))
                    n += snprintf(rect + n, sizeof(rect) - (size_t)n, "%d", rn((k & 1 ? cols : rows) + 2));
            }
            rect[n] = 0;
            switch (rn(9)) {
            case 0: putf("\033[%d;%s$x", PICK(fills), rect); break;
            case 1: putf("\033[%s$z", rect); break;
            case 2: putf("\033[%s;1;%d;%d;1$v", rect, rn(rows + 2), rn(cols + 2)); break;
            case 3: putf("\033[%s%s$t", rect, PICK(reverse)); break;
            case 4: putf("\033[%s;%s$r", rect, PICK(change)); break;
            case 5: putf("\033[%d'%c", rn(cols + 1), "}~"[rn(2)]); break;
            case 6: putf("\033[%d %c", rn(cols + 1), "@A"[rn(2)]); break;
            default: put(rn(2) ? "\0336" : "\0339"); break;
            }
            break;
        }
        case 45:
            /* DECSCA and the selective erases. */
            switch (rn(5)) {
            case 0: putf("\033[%d\"q", rn(4)); break;
            case 1:
                put("\033[1\"q");
                put(PICK(words));
                put(rn(2) ? "\033[0\"q" : "\033[2\"q");
                break;
            case 2: putf("\033[?%dJ", rn(4)); break;
            case 3: putf("\033[?%dK", rn(3)); break;
            default:
                putf("\033[%d;%d;%d;%d${", rn(rows + 1), rn(cols + 1), rn(rows + 2), rn(cols + 2));
                break;
            }
            break;
        case 46: {
            /* Reverse wraparound, XTSAVE and XTRESTORE, and mode reports. */
            static const int toggles[] = { 45, 1045, 7, 66, 1007 };
            static const int saved[] = {
                1, 5, 6, 7, 12, 25, 42, 45, 47, 66, 67, 1000, 1002, 1004, 1006,
                1007, 1045, 1048, 1049, 2004, 2026, 2027, 9999,
            };
            static const int unmodeled[] = { 69, 2031, 2048 };
            static const int reported[] = { 2, 42, 45, 66, 1007, 1045, 1046, 1048, 2027 };
            switch (rn(7)) {
            case 0: putf("\033[?%d%c", PICK(toggles), "hl"[rn(2)]); break;
            case 1:
                for (int k = 1 + rn(cols * 2); k > 0; k--)
                    putb('\b');
                break;
            case 2: putf("\033[%dD", rn(cols * rows * 2)); break;
            case 3:
                if (rn(2))
                    putf("\033[%sD", big_number());
                else
                    put("x\033[D");
                break;
            case 4: putf("\033[?%d;%d%c", PICK(saved), PICK(saved), "sr"[rn(2)]); break;
            case 5:
                if (gen_unmodeled)
                    putf("\033[?%d%c", PICK(unmodeled), "sr"[rn(2)]);
                break;
            default:
                if (rn(4))
                    putf("\033[?%d$p", PICK(reported));
                else
                    put("\033[12$p");
                break;
            }
            break;
        }
        case 47:
            /* Line sizes: DECDHL top and bottom, DECSWL, DECDWL. Text after
             * one fills the row to half the width. */
            putf("\033#%c", "3456"[rn(4)]);
            if (rn(2))
                put(PICK(words));
            break;
        case 48: {
            /* Character sets: designations of G0 to G3, shifts, NRCS mode,
             * and the ASCII positions that the sets replace. */
            static const char *sets[] = {
                "B", "0", "A", "<", ">", "%5", "%6", "4", "C", "5", "R", "f", "Q", "9", "K",
                "Y", "E", "6", "`", "Z", "H", "7", "=", "1", "%0", "\"4", "%%5", " 0", "\r0",
                "\1770",
            };
            static const char *shifts[] = {
                "\x0e", "\x0f", "\033n", "\033o", "\033N", "\033O", "\033~", "\033}", "\033|",
                "\xc2\x8e", "\xc2\x8f", "\033[?42h", "\033[?42l", "\033[?42$p", "\033[!p",
            };
            static const char *text[] = {
                "#@[\\]^_`{|}~", "!0>Aaqx~", " \177a", "\033N\xcc\x81", "\033O\xc3\xa9",
            };
            static const char *locking[] = { "\x0f", "\x0e", "\033n", "\033o" };
            switch (rn(5)) {
            case 0:
            case 1: {
                /* Designate a set into one G set, select it, and print. */
                int g = rn(4);
                if (rn(2))
                    put(rn(3) ? "\033[?42h" : "\033[?42l");
                if (g && !rn(3))
                    putf("\033%c%s", "-./"[g - 1], rn(3) ? "A" : PICK(sets));
                else
                    putf("\033%c%s", "()*+"[g], PICK(sets));
                if (g < 2 || rn(2))
                    put(locking[g]);
                else
                    put(g == 2 ? "\033N" : "\033O");
                put(PICK(text));
                break;
            }
            case 2: put(PICK(shifts)); break;
            case 3:
                /* DECCIR restores the shifts and the designations. */
                if (!gen_unmodeled)
                    break;
                putf("\033P1$t1;1;1;@;@;@;%d%d;%c;", rn(5), rn(5), 0x40 + rn(17));
                for (int k = 0; k < 4; k++)
                    put(PICK(sets));
                put("\033\\");
                break;
            default: put(PICK(text)); break;
            }
            break;
        }
        }
    }
}

#endif
