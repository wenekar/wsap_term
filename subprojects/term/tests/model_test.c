#include "model.h"
#include "fuzz_gen.h"
#include "seeds.h"
#include "wsap_vtcore/wsap_vtcore.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char probe[] =
    "\033[6n\033P$qm\033\\\033P$qr\033\\\033P$q q\033\\"
    "\033[?6$p\033[?7$p\033[4$p\033[20$p";

struct event {
    char *bytes;
    size_t len;
    unsigned flags;
    struct wsap_vtcore_vector2i size;
};

struct script {
    struct wsap_vtcore_vector2i size;
    int history;
    struct event *ev;
    int n, cap;
};

static void script_add(struct script *s, struct event e)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->ev = realloc(s->ev, (size_t)s->cap * sizeof(*s->ev));
        if (!s->ev) abort();
    }
    s->ev[s->n++] = e;
}

static void script_free(struct script *s)
{
    for (int i = 0; i < s->n; i++) free(s->ev[i].bytes);
    free(s->ev);
    *s = (struct script){0};
}

static char *dup_bytes(const char *p, size_t n)
{
    char *d = malloc(n ? n : 1);
    if (!d) abort();
    memcpy(d, p, n);
    return d;
}

struct replies {
    char *s;
    size_t len, cap;
};

static void on_reply(void *ud, const char *bytes, size_t len)
{
    struct replies *r = ud;
    if (r->len + len > r->cap) {
        r->cap = (r->len + len) * 2;
        r->s = realloc(r->s, r->cap);
        if (!r->s) abort();
    }
    memcpy(r->s + r->len, bytes, len);
    r->len += len;
}

static void dump(FILE *f, const char *s, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 27) fputs("\\033", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c >= 32 && c < 127) fputc(c, f);
        else fprintf(f, "\\x%02x\"\"", c);
    }
    fputc('"', f);
}

static char glyph(uint32_t cp)
{
    return cp >= 32 && cp < 127 ? (char)cp : cp ? '?' : '.';
}

static void dump_all(struct model *m, struct wsap_vtcore *t, struct wsap_vtcore_snapshot *snap, FILE *log)
{
    int total = wsap_vtcore_scrollback(t).total, mtotal = model_history(m);
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t), msize = model_size(m);
    int rows = total + size.y > mtotal + msize.y ? total + size.y : mtotal + msize.y;
    fprintf(log, "  term (history %d)%*s model (history %d)\n", total, size.x > 8 ? size.x - 8 : 0,
            "", mtotal);
    for (int r = 0; r < rows; r++) {
        fprintf(log, "  %3d |", r);
        if (r < total + size.y) {
            int offset = r < total ? total - r : 0;
            wsap_vtcore_scroll_to(t, offset);
            wsap_vtcore_snapshot_update(snap, t);
            const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, r < total ? 0 : r - total);
            for (int x = 0; x < size.x; x++) {
                uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
                if (row[x].text) wsap_vtcore_snapshot_cluster(snap, row[x].text, cp, WSAP_VTCORE_GRAPHEME_MAX_LEN);
                fputc(glyph(cp[0]), log);
            }
        } else {
            fprintf(log, "%*s", size.x, "");
        }
        fprintf(log, "| |");
        if (r < mtotal + msize.y) {
            const struct model_cell *row = model_row(m, r);
            for (int x = 0; x < msize.x; x++) fputc(glyph(row[x].n ? row[x].cp[0] : 0), log);
        }
        fprintf(log, "|\n");
    }
    wsap_vtcore_scroll_to(t, 0);
    struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t), mc = model_cursor(m);
    fprintf(log, "  cursor term %d,%d model %d,%d\n", c.pos.x, c.pos.y, mc.pos.x, mc.pos.y);
}

static const char *compare(struct model *m, struct wsap_vtcore *t, struct wsap_vtcore_snapshot *snap,
                           struct replies *got, char *why, size_t cap, FILE *log)
{
    size_t want_len;
    const char *want = model_replies(m, &want_len);
    if (want_len != got->len || (want_len && memcmp(want, got->s, want_len))) {
        if (log) {
            fprintf(log, "replies: term ");
            dump(log, got->s ? got->s : "", got->len);
            fprintf(log, "\n         model ");
            dump(log, want, want_len);
            fputc('\n', log);
        }
        snprintf(why, cap, "replies differ");
        return why;
    }
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(t), msize = model_size(m);
    if (size.x != msize.x || size.y != msize.y) {
        snprintf(why, cap, "size %dx%d, model %dx%d", size.x, size.y, msize.x, msize.y);
        return why;
    }
    struct wsap_vtcore_cursor c = wsap_vtcore_cursor(t), mc = model_cursor(m);
    if (c.pos.x != mc.pos.x || c.pos.y != mc.pos.y || c.style != mc.style ||
        c.visible != mc.visible || c.blink != mc.blink) {
        snprintf(why, cap, "cursor %d,%d style %d visible %d blink %d; model %d,%d %d %d %d",
                 c.pos.x, c.pos.y, c.style, c.visible, c.blink,
                 mc.pos.x, mc.pos.y, mc.style, mc.visible, mc.blink);
        return why;
    }
    struct wsap_vtcore_modes md = wsap_vtcore_modes(t), mm = model_modes(m);
    if (md.keyboard != mm.keyboard || md.mouse != mm.mouse ||
        md.mouse_encoding != mm.mouse_encoding || md.app_cursor != mm.app_cursor ||
        md.app_keypad != mm.app_keypad || md.backarrow_bs != mm.backarrow_bs ||
        md.bracketed_paste != mm.bracketed_paste || md.focus_events != mm.focus_events ||
        md.alternate != mm.alternate || md.reverse_video != mm.reverse_video ||
        md.synchronized != mm.synchronized || md.alternate_scroll != mm.alternate_scroll) {
        snprintf(why, cap, "modes differ (alternate %d/%d reverse %d/%d keypad %d/%d)",
                 md.alternate, mm.alternate, md.reverse_video, mm.reverse_video,
                 md.app_keypad, mm.app_keypad);
        return why;
    }
    int total = model_history(m);
    wsap_vtcore_scroll_to(t, 0);
    wsap_vtcore_scroll(t, INT_MAX);
    for (int offset = total;; offset = offset > size.y ? offset - size.y : 0) {
        if (!offset)
            wsap_vtcore_scroll_to(t, 0);
        if (!wsap_vtcore_snapshot_update(snap, t)) {
            snprintf(why, cap, "snapshot update failed");
            return why;
        }
        for (int y = 0; y < size.y; y++) {
            const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snap, y);
            const wsap_vtcore_color *uls = wsap_vtcore_snapshot_row_underline_colors(snap, y);
            int retained = total - offset + y;
            enum wsap_vtcore_line_size line = wsap_vtcore_snapshot_row_size(snap, y);
            if (line != model_row_size(m, retained)) {
                snprintf(why, cap, "retained row %d: line size %d, model %d", retained, (int)line,
                         (int)model_row_size(m, retained));
                wsap_vtcore_scroll_to(t, 0);
                return why;
            }
            const struct model_cell *want_row = model_row(m, retained);
            for (int x = 0; x < size.x; x++) {
                const struct model_cell *e = &want_row[x];
                uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
                int n = row[x].text ? wsap_vtcore_snapshot_cluster(snap, row[x].text, cps,
                                                            WSAP_VTCORE_GRAPHEME_MAX_LEN) : 0;
                wsap_vtcore_color ul = uls ? uls[x] : WSAP_VTCORE_COLOR_NONE;
                if (n == e->n && row[x].width == e->width && row[x].attrs == e->attrs &&
                    row[x].fg == e->fg && row[x].bg == e->bg && ul == e->ul &&
                    !memcmp(cps, e->cp, (size_t)n * sizeof(*cps)))
                    continue;
                if (log) {
                    fprintf(log, "retained row %d (offset %d):\n  term:  ", retained, offset);
                    for (int k = 0; k < size.x; k++) {
                        uint32_t cp[WSAP_VTCORE_GRAPHEME_MAX_LEN] = {0};
                        if (row[k].text)
                            wsap_vtcore_snapshot_cluster(snap, row[k].text, cp, WSAP_VTCORE_GRAPHEME_MAX_LEN);
                        fputc(glyph(cp[0]), log);
                    }
                    fprintf(log, "\n  model: ");
                    want_row = model_row(m, retained);
                    for (int k = 0; k < size.x; k++)
                        fputc(glyph(want_row[k].n ? want_row[k].cp[0] : 0), log);
                    fputc('\n', log);
                    e = &want_row[x];
                }
                snprintf(why, cap,
                         "cell %d,%d: term U+%04X (%d cps) w%u attrs %#x fg %#x bg %#x ul %#x; "
                         "model U+%04X (%u cps) w%u attrs %#x fg %#x bg %#x ul %#x",
                         x, y, cps[0], n, row[x].width, row[x].attrs, row[x].fg, row[x].bg, ul,
                         e->cp[0], e->n, e->width, e->attrs, e->fg, e->bg, e->ul);
                wsap_vtcore_scroll_to(t, 0);
                return why;
            }
        }
        if (!offset) break;
        wsap_vtcore_scroll(t, -(offset > size.y ? size.y : offset));
    }
    int observed_total = wsap_vtcore_scrollback(t).total;
    if (observed_total != total) {
        snprintf(why, cap, "visited history %d rows, model %d", observed_total, total);
        return why;
    }
    wsap_vtcore_scroll_to(t, 0);
    return NULL;
}

enum chunking { CHUNK_RANDOM, CHUNK_WHOLE, CHUNK_BYTES };

static int run(const struct script *s, enum chunking chunks, uint64_t chunk_seed,
               char *why, size_t cap, bool verbose)
{
    struct replies got = {0};
    struct wsap_vtcore_options opt = {
        .size = s->size, .scrollback = s->history,
        .callbacks = { .ud = &got, .reply = on_reply },
    };
    struct wsap_vtcore *t = wsap_vtcore_new(&opt);
    struct model *m = model_new(s->size, s->history, NULL);
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    if (!t || !snap) abort();
    uint64_t saved_rng = rng;
    rng = chunk_seed | 1;
    int bad = -1;
    for (int i = 0; i < s->n && bad < 0; i++) {
        const struct event *e = &s->ev[i];
        if (e->bytes) {
            model_feed(m, e->bytes, e->len, e->flags);
            for (size_t k = 0; k < e->len;) {
                size_t n = chunks == CHUNK_WHOLE ? e->len - k
                         : chunks == CHUNK_BYTES ? 1
                         : 1 + (size_t)rn((int)(e->len - k));
                wsap_vtcore_feed(t, e->bytes + k, n, e->flags);
                k += n;
            }
        } else {
            bool a = model_resize(m, e->size);
            bool b = wsap_vtcore_resize(t, e->size);
            if (a != b) {
                snprintf(why, cap, "resize result differs");
                bad = i;
                break;
            }
        }
        if (compare(m, t, snap, &got, why, cap, verbose ? stderr : NULL)) {
            bad = i;
            if (verbose) dump_all(m, t, snap, stderr);
        }
        model_take_replies(m);
        got.len = 0;
    }
    rng = saved_rng;
    free(got.s);
    wsap_vtcore_snapshot_free(snap);
    model_free(m);
    wsap_vtcore_free(t);
    return bad;
}

static bool fails(const struct script *s, char *why, size_t cap)
{
    return run(s, CHUNK_WHOLE, 1, why, cap, false) >= 0 ||
           run(s, CHUNK_BYTES, 1, why, cap, false) >= 0;
}

static void shrink(struct script *s)
{
    char why[512];
    for (int i = s->n - 1; i >= 0; i--) {
        struct event e = s->ev[i];
        memmove(s->ev + i, s->ev + i + 1, (size_t)(s->n - i - 1) * sizeof(*s->ev));
        s->n--;
        if (fails(s, why, sizeof(why))) {
            free(e.bytes);
            continue;
        }
        memmove(s->ev + i + 1, s->ev + i, (size_t)(s->n - i) * sizeof(*s->ev));
        s->ev[i] = e;
        s->n++;
    }
    for (int i = 0; i < s->n; i++) {
        struct event *e = &s->ev[i];
        if (!e->bytes) continue;
        for (size_t step = e->len / 2 ? e->len / 2 : 1; step >= 1; step /= 2) {
            for (size_t at = 0; at + step <= e->len;) {
                char *keep = dup_bytes(e->bytes, e->len);
                size_t keep_len = e->len;
                memmove(e->bytes + at, e->bytes + at + step, e->len - at - step);
                e->len -= step;
                if (fails(s, why, sizeof(why))) {
                    free(keep);
                    continue;
                }
                free(e->bytes);
                e->bytes = keep;
                e->len = keep_len;
                at++;
            }
            if (step == 1) break;
        }
    }
}

static void report(struct script *s, int seed, const char *why)
{
    fprintf(stderr, "seed %d: %s\n", seed, why);
    shrink(s);
    char buf2[512];
    fprintf(stderr, "shrunk: %dx%d history %d\n", s->size.x, s->size.y, s->history);
    for (int i = 0; i < s->n; i++) {
        if (s->ev[i].bytes) {
            fprintf(stderr, "  feed%s ", s->ev[i].flags ? " (ONLCR)" : "");
            dump(stderr, s->ev[i].bytes, s->ev[i].len);
            fputc('\n', stderr);
        } else {
            fprintf(stderr, "  resize %dx%d\n", s->ev[i].size.x, s->ev[i].size.y);
        }
    }
    int at = run(s, CHUNK_WHOLE, 1, buf2, sizeof(buf2), true);
    if (at < 0) at = run(s, CHUNK_BYTES, 1, buf2, sizeof(buf2), true);
    fprintf(stderr, "shrunk mismatch after event %d: %s\n", at, buf2);
}

static int test_seed_157406_regression(void)
{
    struct script s = {
        .size = { .x = 40, .y = 6 },
        .history = 0,
    };
    static const char ev0a[] = "\033[48:2::204:28:133;38;2;205;77;226m\033[1;4r\033[30;40m\x87""\x82""K\xe1""\xb4""\x99""\xa6""%\x83""d4\x90""\x1f""\x15""\xd5""\xcc""\x9f""zV\xde""3\x8d""\xcc""\xe1""\xa0""\xb5""|\xca""\xcf""\x08""\xa0""+\xe3""H\xe0""[=\xeb""\x92""\x07""\x8b""\xb4""\xbb""f(n\xe0""\xcd""\xd4""\x85""\xd2""\x8b""\x92""\x07""W9\xc9""F\x18""\x98""\xdd""\xec""+\xe3""x\xb0""_%g\xc9""\xdc""\xa7""\xca""\x91""@N\x1a""V\xc2""\x8f""V\xb2""#\xef""\x1a""\x9a""}\xfd""X0\xaa""O\xdd""v\xf4""7\x17""\xdd""\xe2""/\x83""\x9e""?\x9b""\xa0""\x1f""\xfb""F \xce""A_\x17""w\xdd""\xd6""\xdb""r\x06""\xdc""\xb7""v\xae""w\xf3""\xe8""K\xd5""\x1a"":\xe4""?\x99""\nB\xa0""\x85""L2\xaa""%\x97""B\xe4""\x1f""\xc5""\xb0""\x9d""p\xa8""g\xad""V\xf4""\x05""\xa7""\x02""\xa0""\xcd""Bdl\xae""\xb3""\xda""\xc3""\xa4""%+M\xcd""0\xd6"";+\xf3""\x80""\x11""A9#]a\x9f""\xb8""g\x8f""\x8e""71U\xa3""\xcc""3\xed""\x96""!\xc3""p\xe6""\xdd""6\x9c""\x01""=Ua{u\xb1"",\xe2""\xdd""@j\xd8""\xf7""\x10""0\x9a""\xa5""*l&`\xe0""g\xfd""Xt\xf0""\x93""|4\xb2""\xd1""*\x90""e\xdb""\x1e""@\x9c""\xb7""\xc6""\xed""\xb4""I\xe3""\xdc""\xdb""\x94""ae\x0b""5S\xd3""\x10""X\xf6""C\xd3""F\xce""5\xfd""\xea""\xf9""6H\xfc""A7e\x83""|&x\xce""\xbd""@\x00""X\x18""\xf6""#\xed""\x82""\x9d"",\xd6""\xa1""X>\xa2""7\x89""\xd4""G\x87""\x00""~\xc0""\xe1""\x04""\xd4""-\xb3""\xe8""A\x9f""B\x1e""\xd4""\xd3""F\x0c""\xb0""1\xd5""\xdc""\xf5""\x92""\x11""\xdb""\x96""/\xe3""\x9a""3\xad""\x10""\x96""=e\xc3""\x1e""\xa2""\xcf"".\x1a""\xf8""\x0b""\x0b""\xb7""\xca""\xc5""n\xc0""o\xd7""\xae""5\xd7""^\"l\xa6""E\x05""\xaf""\xba""K\xa7""6@\xf2""\xf9""\x96""\x15""qa/)\xe4""\xb8""\xad""\xe3""\x80""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe5""\xad""\x97""\xe6""\x96""\x87""a\xe4""\xb8""\xad""\xef""\xbd""\xb1""\xed""\x95""\x9c""hello   ";
    static const char ev0b[] = "\033[7;1H\033[48;2;82;12;1m\r\n\033[1g0TwLy`&,E1gAd7&yD\"\r\nva9jt}0Eduf;`oF[rAdxF7S|onoh/nFqp/u[22eeEF'{mADdkL$([1+Xx03r,mJE&\\0BXvt\r\n8TRr@vgo?*?\r\n\"A/d/V7(F8\"s`en#p]S2wHb!L&b/[kfF#P`x7iDzJe{.dg\r\n3W$t\\sTUX'sfmMD7Gq\r\nRi*f_Vr6C;-:Dm[c2gItGv\\v-hegiB`X7}`\r\n}naZ-pn(J\"\"\r\n5f_G4.16\"{HCmoZ?%Wmw9TqQBWZ#flzOay[f];UW<k$J9cWx)~#Z3O$Le/1Mz_T\r\nkm)`\\F4rQn#bo\\q4>`#}+|Qk@qIO\r\n#y@D}3TPo6g&u1{*j^/:q{EYu3:iMhV>4?NkL>!uA:Y%m,^SX6\r\n%$_\"JY|d$iK|)V3b^}~'+0C8=UbS}M/~Gs_7Wio@L.){Z7xqKN=aP@x/K&2uK\r\nF]D~{zn\r\ncmH#T>!^pxJom-6*gqp+#(TY\\.LKZ7jq,eJ{I+A\"#!59@-b]-P++\".pMNw!CVuPD*(5y&y3Y1\"\r\n8f!gt<U#.^<[ruP={A\r\n{XTu\r\noXg8OIB9pg3%icTA\r\nn]GvK2b~pb\"\\4Fa$:s!={gNSL3>bx1Q!'K6sFVGs2yg);&_>z-EjU<9[y0_>a1\r\nj@u`8DIk8/PR|0}(*'MS]#RmM|;usmF_GC~3b2=}jKm6w6`PWYZj~{9`+3:uns9Sz^D)\r\nWa@hgqMwtr\r\nDA8J9e`OA_Kls)^v,io(9:]Nu!Kx&Si>N\r\n98uULO{5hnW%]eNdqT15(v#_UrMyQhNTNa&0Q[PkSWPq<(f[+9~@}2H=zc5HK-(E2N<*S-eF9ZkI#C\r\n?-xvK]No0zJ{5$1B~dU[\"Z7d.`Yn1katlWIF#nl#,:=x^JH;?1N,&\r\na`UlnMWr`n/Tb9\\[;w<gE,RxJ*,wtM}[H(^3FANR\\0hK$%&TM0dm~O#MX;a&%xBiP3Z]D~Je#\r\nxIblgeAAK{L>PP%&l|MW51mh\"(+B!qA`1!e2zl\\q30\r\n3/Zd1zm@NP%^:Zn6Kd^`};VH1}REkQe-\r\n#&Cf;@`!M@+7'>RZ]7>,F.1pF\"yJq$-R/3v<E94T~t,6as\"'Hg]}4Fx*!vE*[0Aje\r\n|D+DdUkg8<ZmBU_3EzcKdpQB4)5S,ao<eAq|}!7ZwXc3^Op!xF$:G.Ajj+]H%M\r\ngal+xL6h|SaReHnc1_9a^-CKDs?!Y947Dv5<8U@aBw'tv/#Ml0~@>^NK|||c7iK^M\r\nIJE_;[qH:D2bdNU.Q>OSSI1,EJ\\~KTYs}[+}@]_*3ZR8\r\nG!6NG/eVrYX-XuT@+zJb\"8N\"=@}Be`}t~|X(#\\oyXE\r\n>rn2[x'ppK>y.6q$Bss`h3@4_Z~x,)_I7:#\r\nigb}!y/fr}0-S>$!OSKX:s2cn|S%GOv%'n,!dSvDoE^\r\nl[6m7M/h68uBS\r\nli>Wqi*VOerUq+[;sMqc-mCbmT:iO2\\na>)^9+9BCTE]e||(`6a_s)-Y&_\\1s}4IZ^4KFR#$8\r\n!{xk\"rZ_[C|9>N5_zq}td,[yB)%-0mI\r\n-U7^ZLCw2e6=9<>>q\r\n/Q<_.XR+<+uCL|k0[4)\r\nh-c5KKd*OO'\r\n|bMI<T;2z^b`<I_$x7hz7ZU_3@rQm|i.'=!wgg@}'T^s}EnR^}8L,?VjGe<SGX[0ssL1M84OGnJ\r\n20$_e<[V'*jHW`}I\r\ny==FPkcV~B/el}U`+ZE\\[]#nx-bqqv3:>km=$45@\\.kr=tMy6v?eL:bU<h#>:dTKa}3uzxPJAOJeX<\r\ng#ISU8%[F,k}[uA?!jfk;Gw$kAbP8R:9\r\n+_!6i|}!Wx9@x,t4L[GGn8\\M,tXMf7|-31;Y2AyJ4G<j]S.!u'oW?cQaf\r\n^T{nDv4WT,dO&r\r\n4t1egVWg8QV{Ded***j(N!A]j[5:TQMWzmkPB:z?XW'a+b$0vQ\r\n$p'UCS3#:z{Cg7D$$]_b>Wa1LYb2t_b;rAJ.X;^/U3pAFhL8/p\r\n-I%hfA]cOB$}@Oer[wMXnkE-@|0kNf[6/^ejj?S'B(#ICg\r\nJCdUF\r";
    static const char ev0c[] = "\naS6LGrwiy(B%I5\"&r$.+!rz&'Jxkyjaz|l8nR\\a1U'8!oZF8xUftq<2cZmn}WQ`dnv\r\no7yT{H/[}u$MF:JgzYWqgJ;o>I/z#C_/#@RgSmn.zhZm1\\=XWa{hvJrV\"\r\n/P!yyC\r\n@*K89}7^Cjmr=.}C|G9R:6HAe_bP$#1/l@.TFLuC)C^txN;u5E\r\n[X;+5%2qfdAU'JWRF*o@yjjYI)0nrR5NkOu8''s7bl10\r\n';)M4g_YD~h~KyZjdJgr>yp;S7{=RwYfr\\8'HRY78(I{uo>)QhbWv1P>Q+rSD%X>j<\"\r\n`j69@mng:!q]o>;@'/K27Fy}-[1dT$2Cb6B^p=NiBU%c++(wt}\r\n\\k^8%x.J8zxRx&cT*C18M]y_JEf$WVF&&\r\nMoa=`+8<FI1}<8$s(zsj\r\n}{O;0^\"'c}EvE~D5HnIeh,4m,v8~'xoYTYl0v\\Tg\r\n)'qwp2\r\nFx$QinS)D~C$bwX|\r\n#)p7DBiLu-YF\r\n4,*%25}'$j@\\\r\nJ*^+\\Mh9aG^j}@!C7u(.&CZ4s>7;8IUoD\r\nKG\\@:Y^Y-[aoaalPmn&0M%HuP;MORGT,3$A7.A5,]{Te?OrjtO%8eS:3%?ud)\r\n847Gz|DCnMLRy1Niz_^HY-Gn\r\n#=\\mGCkeXn!]|Qz'86Zk5LuN,7<,$^Tp/`'H6mDw\r\n^zAG~wzApuaDCkYk+iHHU(LU3b`Qo!Vz%u]>5_nn?2TQyvLI|R~\r\nj\\C0/$hA2F1G{tW#XpT:[QvFu85AS*pBg\r\nabo#.M6ytAUw/D\r\nS='.TCW/s\r\nFHw*MVDwmuY0\"ZD6XE)n|W%0lzqSm\"Tx*!w;AMl>|K%wL86x7$Z';Cs1=9j]:&v5Ad].b[\r\nYxiEMcmBvjv8yNh$\\Db\"3x*SNl;QAc&n~qlhm^bL@MPh$MC>-3OiGgRVY1W+iF;ASsW\r\n:\\$*kf_`e<y.ETD6|%A.,\r\n1#yEV.je'^tn.AGolf@Y)+Fr;a.rj?_,3#adR\\%V`@IB1Z|)J,\r\n68TBXG6#sV;*^l8/3>5oTa<QL%KY&:L!\r\nn>bVJBX\\WmJ,c&9kf$hoBZDeh@ueqvH<|''<x$=5sC'87\\H.~Y\"2KusSlnx1J/Ln9NBuA*tz$Mk\r\nA[Im-eL|R%^M\"kt/yo-sSJq\"d|gn\r\nW-\"xRq:G4+yz(H<(#(R!OW(i7uVvDAP,9eMJgj@$RX\r\nm5j/NNslU^8mqB|*C`8M2l?}0$1b)6#Ei`K6'}DjuA/|U4`0Aqbj?$)$-mHs/OQx&hrUef!)7kB\r\n.12H#7SSlkkTObb>1PBXw7BM5UlLL2AhP\r\n'TN<*`n%+j{*'gfL[fLmd|oYV.7ZR2h28:Qq%5Wv}[ypsY&W;M/WSXJ2c@+XkW-ig^3xP'bbg39\r\nyzmR#\r\nW}dX.hFR*\r\nihpLy?7S+tsuAr_*oGj\"ZITTgwB<Y\r\n!g5=~DxRg%V)'|p7',5\r\n>w$\"L'mkzG#YyDdK\\>u91xQNkg66LJm]vE-b0W201zI0voc.c>7pv:*l^`\\t'<3j:7\"YvCPx:b{=\r\n\n\033[1;39;3;40;42$r\033[36m\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe3""\x80""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xed""\xa0""\x80""\xe5""\xad""\x97""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe2""\x80""\x8d""\xe5""\xad""\x97""\xe0""\x80""\xaf""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe0""\x80""\xaf""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe5""\xad""\x97""\033[20f\033[48;2;253;198;1m\033[3b\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe2""\x83""\x90""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe2""\x83""\x90""\xef""\xbf""\xbd""\xe0""\x80""\xaf""\xe5""\xad""\x97""\xed""\xa0""\x80""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe2""\x80""\x8d""\xe5""\xad""\x97""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe2""\x83""\x90""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe0""\x80""\xaf""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe5""\xad""\x97""\xef""\xbf""\xbd""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe2""\x83""\x90""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe5""\xad""\x97""\033[1000000;99999999999999999999a\033#8\x0b""\033[48;2;103;236;1m\r\033]0;title\x07""\033[1;13;2;16;1$r\033[1*x\033[6n\033P$qm\033\\\033P$qr\033\\\033P$q q\033\\\033[?6$p\033[?7$p\033[4$p\033[20$p";
    size_t ev0_len = sizeof(ev0a) - 1 + sizeof(ev0b) - 1 + sizeof(ev0c) - 1;
    char *ev0 = malloc(ev0_len + 1);
    memcpy(ev0, ev0a, sizeof(ev0a) - 1);
    memcpy(ev0 + sizeof(ev0a) - 1, ev0b, sizeof(ev0b) - 1);
    memcpy(ev0 + sizeof(ev0a) - 1 + sizeof(ev0b) - 1, ev0c, sizeof(ev0c));
    script_add(&s, (struct event){ .bytes = ev0, .len = ev0_len, .flags = 1 });
    static const char ev1[] = "\t\033[120;5;20;1;1$x\033[48;2;228;193;1m\033[35;45m\r\n\r\n\033[1*x\xf0""\x9f""\x91""\x8d""\033[255;999999X\033[1 qhello\033[20$p\033[99999999999999999999;1234567g\033[m\033[48;2;56;106;1m\xe0""\x80""\xaf""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe5""\xad""\x97""a\xe4""\xb8""\xad""\xe5""\xad""\x97""\xef""\xbf""\xbd""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""a\xe5""\xad""\x97""a\xe4""\xb8""\xad""\xe2""\x83""\x90""\xed""\x95""\x9c""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xed""\xa0""\x80""\033[2;3r\033[4;30;6;28;31$r\r\n\033[2;7;1;1$r\x0e""\033[2;18;1;1$r\033EBway-6#=xzP\"5DK\"1u@x4oT~+^-Ro-%0NYGJ6lPS]HaHVd6la/w[lYfqeSJ#jQfSCl\"3Ye^z@::K.r)q_e\xcc""\x81""\033[6;35;5;24;1$r\xef""\xbd""\xb1""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xef""\xbf""\xbd""\xe6""\x96""\x87""\xe6""\x96""\x87""\033[6;7;1;19;24$r\r\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe2""\x83""\x90""\xef""\xbf""\xbd""\xed""\x95""\x9c""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xed""\xa0""\x80""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe0""\x80""\xaf""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe4""\xb8""\xad""a\xe2""\x83""\x90""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe6""\x96""\x87""\xef""\xbd""\xb1""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe2""\x82""\xac""\xe6""\x96""\x87""\xe2""\x83""\x90""\xe0""\x80""\xaf""\xc2""\x8d""e\xcc""\x81""\033[3\x18""m\x9a""\x17""\xc5""\xb4""#\x01""\x95""\xc6""1\xf1""\x96""c\x83""\x04""\x90""e]\xa3""0\xc8""\x9d""\x98""?O\xa5""\xd4""\xef""\x08""\xd2""\xfb""\xee""!\x99""\xc4""\x07""\xdd""Z~\xc2""\r\xa3""fn\xee""\xa3""<\xec""\xdd"" TV\xc0""G\xd1""\x14""\xa0""E\xbf""R\x9c""\x8f"",xR\x94""\xa5""@@\xb0""S\xd5""\x9a""Cy\x01""i/\x8b""&$8h\x06""\xf4""\xd7""Z\xcc""1\xbd""j\x04""\x8e""+w\xff""~\xb2""\x1f""\xdf""\x0e""N\x9e"";ok\xf9""4\xe8""\xdd""@\xce""\x99""\x02""\x8e""\xf7""l\xaa""\xc7""\xf6""_\x05""\xb9""\xe0""y}\xbd""\x1e""rR\xbe""!+\xa5""v2`8\xf0""\xfd""d\x88""\x93""Ll\xdc""\x7f""=1#%O\x8b""f8v\xe8""\xdb""\xe0""\xf5""\xfc""\x87""\xf4""Q\xf3""\xea""\xd1""T\xb0""\xc9""\x1a""\xfe"";\x95""^\xa2""kA\x85""2N\xfa""\x9b""\x96""K\xa1""\xa0""\xab"" \xf0""\xf5""\xb4""\xdb""\x82"";\x1f""\xb3"" \x08""\"b\x12""J LdJ\xa2"")\x83""\xb0""\xef""\xe2""\xbf""\xf6""\xe1""\xae""ge#\xed""\x9e""\x8f""N\xcc""M\xef""\xd2""\xa7""jN\xc0""S\xc5""Pt\xe6""o\x85""\xde""%\x81""\xb2""\xb1""\xde""\x1d""OU\xbf""\x92""\xdf""\x8e""K\xd1""\xd2""\xc5""\xb2""\x0b""G#\xc1""\xa4""\xc7""|0\xb2""\xe1""X\xac""\xcf""FN\xd4""K\x01""\xf9""\xde""3\xcd""\x96""\xdd""\x84""\xa9""z,\xcc""\x81""\x12""\x9e""a'\x01""K\xe9""\xd6""\x1d""\xab""^v\x00""X\x1c""\x04""F\xf2""\x83""\xe2""y!I\xff""\x88""W\xbd""\xa4""u\x8b""\x9e""\xd3""ph\x16""\x06""\xb0""\x8d""\xe6""eK\xe3""\x14""\xdc""\xe9""\xbc""\x11""\x0b""\x8b""0fr\x96""\x0b""\xbb""\x1e""\xa0""oe\x13""\x97""*\xc0""\x97""\x1e""\x9c""scwW\xb7""\xb0""9\xf7""\xfa""WK\x85""Ht\r\033[38;2;78;100;209m\033[6n\033P$qm\033\\\033P$qr\033\\\033P$q q\033\\\033[?6$p\033[?7$p\033[4$p\033[20$p";
    script_add(&s, (struct event){ .bytes = dup_bytes(ev1, sizeof(ev1) - 1), .len = sizeof(ev1) - 1, .flags = 0 });
    script_add(&s, (struct event){ .size = { .x = 21, .y = 5 } });
    static const char ev3[] = "\033[?25\x1a""\x0f""\033[1P\033[16t\033[2J\033EBN5q,Uuog_>!\t\033[48;2;76;247;1m\033[2147483648;256J\033[3;4r\xe4""\xb8""\xad""a\xe6""\x96""\x87""\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe5""\xad""\x97""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe6""\x96""\x87""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""\xef""\xbd""\xb1""\xe4""\xb8""\xad""aa\xe4""\xb8""\xad""\xe4""\xb8""\xad""\xe6""\x96""\x87""\xe5""\xad""\x97""\xe6""\x96""\x87""\033[6n\033P$qm\033\\\033P$qr\033\\\033P$q q\033\\\033[?6$p\033[?7$p\033[4$p\033[20$p";
    script_add(&s, (struct event){ .bytes = dup_bytes(ev3, sizeof(ev3) - 1), .len = sizeof(ev3) - 1, .flags = 0 });
    static const char ev4[] = "\033[2;20;1;1$r\033[5 q\n\x08""\033[48:2::228:235:89;48:2::5:214:157m\033[2;4r\033[32;41m\033[19;0thello\033[6n\033P$qm\033\\\033P$qr\033\\\033P$q q\033\\\033[?6$p\033[?7$p\033[4$p\033[20$p";
    script_add(&s, (struct event){ .bytes = dup_bytes(ev4, sizeof(ev4) - 1), .len = sizeof(ev4) - 1, .flags = 1 });
    char why[512] = {0};
    int at = run(&s, CHUNK_RANDOM, 6762033954686556513ULL, why, sizeof(why), false);
    if (at >= 0) {
        fprintf(stderr, "seed 157406 regression: failed at event %d: %s\n", at, why);
        script_free(&s);
        return 1;
    }
    script_free(&s);
    return 0;
}

static int fixed_cases(void)
{
    static const struct {
        int cols, rows, history;
        const char *input;
    } cases[] = {
        { 12, 3, 5, "a中e\xcc\x81日本 \033[1;3;4;9;38;5;123;48;2;1;2;3mQ\033[0mR" },
        { 8, 4, 5, "\033[2;3r\033[?6hZ\nQ\nR\033[?6l\033[r" },
        { 12, 3, 50, "one two three four five\r\nsix seven eight\r\nnine" },
        { 10, 3, 50, "\033]133;A\007$ ls -la /usr/share\r\nout1\r\n\033]133;A\007$ " },
        { 6, 3, 5, "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa\xe2\x9d\xa4\xef\xb8\x8f" "ab\xe2\x98\xba\xef\xb8\x8e" },
        { 8, 3, 5, "\033(0lqqk\033(Bx\x0e" "abc\x0f" "abc" },
        { 8, 3, 5, "abcdefgh\033[3;4H\033[31;1m\0337\033[0m\033[H\0338X" },
        { 6, 2, 5, "abcdef\xe3\x80z\xed\xa0\x80\x80" },
    };
    static const struct wsap_vtcore_vector2i sizes[] = {
        { .x = 5, .y = 3 }, { .x = 16, .y = 4 }, { .x = 7, .y = 3 }, { .x = 20, .y = 5 },
        { .x = 1, .y = 2 }, { .x = 3, .y = 6 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct script s = {
            .size = { .x = cases[i].cols, .y = cases[i].rows },
            .history = cases[i].history,
        };
        script_add(&s, (struct event){
            .bytes = dup_bytes(cases[i].input, strlen(cases[i].input)),
            .len = strlen(cases[i].input) });
        script_add(&s, (struct event){ .bytes = dup_bytes(probe, sizeof(probe) - 1),
                                       .len = sizeof(probe) - 1 });
        for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
            script_add(&s, (struct event){ .size = sizes[k] });
        char why[512];
        int at = run(&s, CHUNK_BYTES, 1, why, sizeof(why), true);
        if (at >= 0) {
            fprintf(stderr, "fixed case %zu event %d: %s\n", i, at, why);
            report(&s, -(int)i, why);
            script_free(&s);
            return 1;
        }
        script_free(&s);
    }
    if (test_seed_157406_regression())
        return 1;
    return 0;
}

static const int history_limits[] = { 0, 5, 50, 4096 };

/* Build the script of one seed. Leave rng at the seed of its write chunks. */
static void build_script(int seed, struct script *s)
{
    rng = 0x9e3779b97f4a7c15ull * (uint64_t)seed;
    *s = (struct script){
        .size = { .x = 4 + rn(37), .y = 2 + rn(11) },
        .history = history_limits[rn(4)],
    };
    struct wsap_vtcore_vector2i size = s->size;
    for (int step = 0; step < 4; step++) {
        blen = 0;
        gen(10 + rn(30), size.x, size.y);
        put(probe);
        unsigned flags = rn(5) == 0 ? WSAP_VTCORE_FEED_ONLCR : 0;
        script_add(s, (struct event){ .bytes = dup_bytes(buf, blen), .len = blen,
                                      .flags = flags });
        if (rn(4) == 0) {
            size = (struct wsap_vtcore_vector2i){ .x = 1 + rn(44), .y = 1 + rn(14) };
            script_add(s, (struct event){ .size = size });
        }
    }
    if (getenv("MODEL_TEST_PRINT")) {
        fprintf(stderr, "seed %d: %dx%d history %d\n", seed, s->size.x, s->size.y, s->history);
        for (int i = 0; i < s->n; i++) {
            if (!s->ev[i].bytes) {
                fprintf(stderr, "  resize %dx%d\n", s->ev[i].size.x, s->ev[i].size.y);
                continue;
            }
            fprintf(stderr, "  feed ");
            dump(stderr, s->ev[i].bytes, s->ev[i].len);
            fputc('\n', stderr);
        }
    }
}

static bool seed_passes(int seed)
{
    struct script s;
    build_script(seed, &s);
    char why[512];
    bool ok = run(&s, CHUNK_RANDOM, rng, why, sizeof(why), false) < 0;
    script_free(&s);
    return ok;
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 20000;
    int first = argc > 2 ? atoi(argv[2]) : 1;
    if (fixed_cases()) return 1;
    // The seed scripts print in order only on one thread.
    int failed = run_seeds(seed_passes, first, seeds, getenv("MODEL_TEST_PRINT") ? 1 : 0);
    if (failed < 0)
        return 0;
    // Report and shrink the first failed seed on this thread.
    struct script s;
    build_script(failed, &s);
    char why[512];
    uint64_t chunk_seed = rng;
    run(&s, CHUNK_RANDOM, chunk_seed, why, sizeof(why), true);
    report(&s, failed, why);
    script_free(&s);
    return 1;
}
