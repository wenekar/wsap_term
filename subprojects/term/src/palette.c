#include "palette.h"
#include "internal.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const float srgb_linear[256] = {
    0x0p+0f,        0x1.3e4568p-12f, 0x1.3e4568p-11f, 0x1.dd681cp-11f,
    0x1.3e4568p-10f, 0x1.8dd6c2p-10f, 0x1.dd681cp-10f, 0x1.167cbap-9f,
    0x1.3e4568p-9f, 0x1.660e16p-9f,  0x1.8dd6c2p-9f,  0x1.b6a31ap-9f,
    0x1.e1e31ap-9f, 0x1.07c38cp-8f,  0x1.1fcc2cp-8f,  0x1.390ffap-8f,
    0x1.53936ep-8f, 0x1.6f5adep-8f,  0x1.8c6a92p-8f,  0x1.aac6c2p-8f,
    0x1.ca7382p-8f, 0x1.eb74ep-8f,   0x1.06e76ap-7f,  0x1.18c2a4p-7f,
    0x1.2b4e06p-7f, 0x1.3e8b7cp-7f,  0x1.527cd6p-7f,  0x1.6723eep-7f,
    0x1.7c8292p-7f, 0x1.929a86p-7f,  0x1.a96d8ep-7f,  0x1.c0fd62p-7f,
    0x1.d94bbep-7f, 0x1.f25a44p-7f,  0x1.06155p-6f,   0x1.135f3ep-6f,
    0x1.210bb6p-6f, 0x1.2f1b8ap-6f,  0x1.3d8f84p-6f,  0x1.4c6866p-6f,
    0x1.5ba6fap-6f, 0x1.6b4c02p-6f,  0x1.7b584p-6f,   0x1.8bcc72p-6f,
    0x1.9ca956p-6f, 0x1.adefaap-6f,  0x1.bfa02p-6f,   0x1.d1bb72p-6f,
    0x1.e44258p-6f, 0x1.f73582p-6f,  0x1.054ad2p-5f,  0x1.0f31b8p-5f,
    0x1.194fccp-5f, 0x1.23a55ep-5f,  0x1.2e32c6p-5f,  0x1.38f85cp-5f,
    0x1.43f678p-5f, 0x1.4f2d64p-5f,  0x1.5a9d76p-5f,  0x1.6647p-5f,
    0x1.722a56p-5f, 0x1.7e47c6p-5f,  0x1.8a9fa2p-5f,  0x1.973238p-5f,
    0x1.a3ffdep-5f, 0x1.b108d4p-5f,  0x1.be4d6ep-5f,  0x1.cbcdfcp-5f,
    0x1.d98ac8p-5f, 0x1.e7841ep-5f,  0x1.f5ba52p-5f,  0x1.0216cep-4f,
    0x1.096f2ap-4f, 0x1.10e66p-4f,   0x1.187c94p-4f,  0x1.2031ecp-4f,
    0x1.28068ap-4f, 0x1.2ffa94p-4f,  0x1.380e2cp-4f,  0x1.404176p-4f,
    0x1.489496p-4f, 0x1.5107aep-4f,  0x1.599aep-4f,   0x1.624e5p-4f,
    0x1.6b2224p-4f, 0x1.741676p-4f,  0x1.7d2b6ap-4f,  0x1.866124p-4f,
    0x1.8fb7c4p-4f, 0x1.992f6cp-4f,  0x1.a2c83ep-4f,  0x1.ac8258p-4f,
    0x1.b65ddep-4f, 0x1.c05aeep-4f,  0x1.ca79aap-4f,  0x1.d4ba32p-4f,
    0x1.df1ca4p-4f, 0x1.e9a122p-4f,  0x1.f447dp-4f,   0x1.ff10c2p-4f,
    0x1.04fe0ep-3f, 0x1.0a85p-3f,    0x1.101d46p-3f,  0x1.15c6fp-3f,
    0x1.1b820ap-3f, 0x1.214ea8p-3f,  0x1.272cd6p-3f,  0x1.2d1ca4p-3f,
    0x1.331e2p-3f,  0x1.39315ap-3f,  0x1.3f566p-3f,   0x1.458d46p-3f,
    0x1.4bd612p-3f, 0x1.5230d8p-3f,  0x1.589dap-3f,   0x1.5f1c82p-3f,
    0x1.65ad88p-3f, 0x1.6c50c2p-3f,  0x1.73063cp-3f,  0x1.79ce06p-3f,
    0x1.80a82cp-3f, 0x1.8794bep-3f,  0x1.8e93c8p-3f,  0x1.95a55cp-3f,
    0x1.9cc984p-3f, 0x1.a4004ep-3f,  0x1.ab49ccp-3f,  0x1.b2a606p-3f,
    0x1.ba1516p-3f, 0x1.c196f8p-3f,  0x1.c92bcp-3f,   0x1.d0d37ep-3f,
    0x1.d88e4p-3f,  0x1.e05c18p-3f,  0x1.e83d08p-3f,  0x1.f0312p-3f,
    0x1.f8387p-3f,  0x1.002984p-2f,  0x1.044078p-2f,  0x1.08611ap-2f,
    0x1.0c8b74p-2f, 0x1.10bf8ap-2f,  0x1.14fd64p-2f,  0x1.194506p-2f,
    0x1.1d967ap-2f, 0x1.21f1c2p-2f,  0x1.2656e8p-2f,  0x1.2ac5fp-2f,
    0x1.2f3ee2p-2f, 0x1.33c1c4p-2f,  0x1.384e9cp-2f,  0x1.3ce56ep-2f,
    0x1.418644p-2f, 0x1.463124p-2f,  0x1.4ae61p-2f,   0x1.4fa512p-2f,
    0x1.546e3p-2f,  0x1.59416ep-2f,  0x1.5e1ed2p-2f,  0x1.630666p-2f,
    0x1.67f83p-2f,  0x1.6cf43p-2f,   0x1.71fa6ep-2f,  0x1.770af2p-2f,
    0x1.7c25c2p-2f, 0x1.814ae2p-2f,  0x1.867a5ap-2f,  0x1.8bb42ep-2f,
    0x1.90f866p-2f, 0x1.964706p-2f,  0x1.9ba016p-2f,  0x1.a1039ap-2f,
    0x1.a67198p-2f, 0x1.abea16p-2f,  0x1.b16d1ap-2f,  0x1.b6faa8p-2f,
    0x1.bc92cap-2f, 0x1.c23582p-2f,  0x1.c7e2d6p-2f,  0x1.cd9acep-2f,
    0x1.d35d6cp-2f, 0x1.d92abap-2f,  0x1.df02bap-2f,  0x1.e4e574p-2f,
    0x1.ead2ecp-2f, 0x1.f0cb28p-2f,  0x1.f6ce2ep-2f,  0x1.fcdc04p-2f,
    0x1.017a5ap-1f, 0x1.048c1cp-1f,  0x1.07a34ep-1f,  0x1.0abff2p-1f,
    0x1.0de20cp-1f, 0x1.11099ep-1f,  0x1.1436acp-1f,  0x1.176936p-1f,
    0x1.1aa14p-1f,  0x1.1ddecep-1f,  0x1.2121e2p-1f,  0x1.246a7cp-1f,
    0x1.27b8a2p-1f, 0x1.2b0c56p-1f,  0x1.2e659ap-1f,  0x1.31c472p-1f,
    0x1.3528dep-1f, 0x1.3892e2p-1f,  0x1.3c0282p-1f,  0x1.3f77bep-1f,
    0x1.42f29cp-1f, 0x1.46731cp-1f,  0x1.49f94p-1f,   0x1.4d850cp-1f,
    0x1.511682p-1f, 0x1.54ada6p-1f,  0x1.584a7ap-1f,  0x1.5bedp-1f,
    0x1.5f953cp-1f, 0x1.63432ep-1f,  0x1.66f6d8p-1f,  0x1.6ab04p-1f,
    0x1.6e6f66p-1f, 0x1.72344cp-1f,  0x1.75fef8p-1f,  0x1.79cf6ap-1f,
    0x1.7da5a4p-1f, 0x1.8181a8p-1f,  0x1.85637cp-1f,  0x1.894b2p-1f,
    0x1.8d3896p-1f, 0x1.912be2p-1f,  0x1.952504p-1f,  0x1.992402p-1f,
    0x1.9d28dcp-1f, 0x1.a13396p-1f,  0x1.a5443p-1f,   0x1.a95aaep-1f,
    0x1.ad7714p-1f, 0x1.b1996p-1f,   0x1.b5c19ap-1f,  0x1.b9efcp-1f,
    0x1.be23d8p-1f, 0x1.c25ddcp-1f,  0x1.c69ddep-1f,  0x1.cae3d2p-1f,
    0x1.cf2fc6p-1f, 0x1.d381acp-1f,  0x1.d7d99ap-1f,  0x1.dc377ep-1f,
    0x1.e09b7p-1f,  0x1.e5055ep-1f,  0x1.e9755cp-1f,  0x1.edeb5cp-1f,
    0x1.f26772p-1f, 0x1.f6e98cp-1f,  0x1.fb71cp-1f,   0x1p+0f,
};

static inline float linf_to_srgbf(float c)
{
    return c > 0.0031308f ? 1.055f * powf(c, 1.0f / 2.4f) - 0.055f
                          : 12.92f * c;
}

static const struct wsap_vtcore_theme builtin_theme = {
    .fg = 0xFFFFFFFFu,
    .bg = 0xFF1E1E2Eu,
    .cursor = 0xFF89B4FAu,
    .ansi = {
        0xFF585B70, 0xFFCC6666, 0xFFB5BD68, 0xFFF0C674,
        0xFF81A2BE, 0xFFB294BB, 0xFF8ABEB7, 0xFFC5C8C6,
        0xFF666666, 0xFFD54E53, 0xFFB9CA4A, 0xFFE7C547,
        0xFF7AA6DA, 0xFFC397D8, 0xFF70C0B1, 0xFFEAEAEA,
    },
};

void wsap_vtcore_theme_default(struct wsap_vtcore_theme *theme)
{
    *theme = builtin_theme;
}

static const struct wsap_vtcore_theme high_contrast_theme = {
    .fg = 0xFFFFFFFFu,
    .bg = 0xFF000000u,
    .cursor = 0xFF00FFFFu,
    .ansi = {
        0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
        0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
        0xFF808080, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
        0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF,
    },
};

void wsap_vtcore_theme_high_contrast(struct wsap_vtcore_theme *theme)
{
    *theme = high_contrast_theme;
}

static uint32_t *palette_slot(struct wsap_vtcore_theme *p, const char *key,
                              uint32_t *fallback)
{
    if (!strcmp(key, "foreground")) {
        *fallback = builtin_theme.fg;
        return &p->fg;
    }
    if (!strcmp(key, "background")) {
        *fallback = builtin_theme.bg;
        return &p->bg;
    }
    if (strncmp(key, "color", 5))
        return NULL;
    const char *digits = key + 5;
    if (!*digits || (*digits == '0' && digits[1]))
        return NULL;
    unsigned i = 0;
    for (; *digits; digits++) {
        if ((unsigned)(*digits - '0') > 9)
            return NULL;
        i = i * 10 + (unsigned)(*digits - '0');
        if (i > 15)
            return NULL;
    }
    *fallback = builtin_theme.ansi[i];
    return &p->ansi[i];
}

bool wsap_vtcore_theme_set_key(struct wsap_vtcore_theme *p, const char *key, const char *val)
{
    if (!p || !key || !val)
        return false;
    uint32_t fallback;
    uint32_t *slot = palette_slot(p, key, &fallback);
    if (!slot)
        return false;
    if (!val[0]) {
        *slot = fallback;
        return true;
    }
    uint32_t v;
    if (!wsap_vtcore_color_parse(val, &v))
        return false;
    *slot = v;
    return true;
}

static bool parse_hex_channel(const char *s, size_t len, uint8_t *out)
{
    if (len == 2) {
        int d0 = wsap_vtcore_hex_nib(s[0]), d1 = wsap_vtcore_hex_nib(s[1]);
        if ((d0 | d1) < 0)
            return false;
        *out = (uint8_t)((d0 << 4) | d1);
        return true;
    }
    if (len < 1 || len > 4)
        return false;
    unsigned value = 0;
    for (size_t i = 0; i < len; i++) {
        int d = wsap_vtcore_hex_nib(s[i]);
        if (d < 0)
            return false;
        value = (value << 4) | (unsigned)d;
    }
    *out = (uint8_t)(value * 255 / ((1u << (4 * len)) - 1));
    return true;
}

static bool parse_fraction(const char *s, size_t len, uint8_t *out)
{
    if (len == 0)
        return false;
    bool seen_digit = false;
    bool seen_dot = false;
    unsigned integer = 0;
    unsigned frac = 0;
    unsigned frac_digits = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            seen_digit = true;
            if (!seen_dot) {
                integer = integer * 10 + (unsigned)(c - '0');
                if (integer > 1)
                    return false;
            } else {
                if (frac_digits < 9) {
                    frac = frac * 10 + (unsigned)(c - '0');
                    frac_digits++;
                }
            }
        } else if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else {
            return false;
        }
    }
    if (!seen_digit)
        return false;
    static const double pow10[10] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
    };
    double value = (double)integer + (double)frac / pow10[frac_digits];
    if (value > 1.0)
        value = 1.0;
    *out = (uint8_t)(value * 255.0 + 0.5);
    return true;
}

static bool parse_hex_rgb(const char *s, size_t cw, uint32_t *out)
{
    uint8_t r, g, b;
    if (!parse_hex_channel(s, cw, &r) ||
        !parse_hex_channel(s + cw, cw, &g) ||
        !parse_hex_channel(s + 2 * cw, cw, &b))
        return false;
    *out = wsap_vtcore_rgb(r, g, b);
    return true;
}

bool wsap_vtcore_color_parse(const char *input, uint32_t *out)
{
    if (!input || !out)
        return false;
    const char *s = input;
    while (*s == ' ' || *s == '\t')
        s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (len == 0)
        return false;

    if (s[0] == '#') {
        size_t blen = len - 1;
        if (blen != 3 && blen != 6 && blen != 9 && blen != 12)
            return false;
        return parse_hex_rgb(s + 1, blen / 3, out);
    }

    if (len >= 4 && (s[0] == 'r' || s[0] == 'R') &&
        (s[1] == 'g' || s[1] == 'G') && (s[2] == 'b' || s[2] == 'B')) {
        size_t i = 3;
        bool intensity = false;
        if ((s[i] == 'i' || s[i] == 'I')) {
            intensity = true;
            i++;
        }
        if (i >= len || s[i] != ':')
            return false;
        i++;

        const char *channels[3];
        size_t chlens[3];
        int ci = 0;
        size_t start = i;
        for (;; i++) {
            if (i == len || s[i] == '/') {
                if (ci >= 3)
                    return false;
                channels[ci] = s + start;
                chlens[ci] = i - start;
                ci++;
                if (i == len)
                    break;
                start = i + 1;
            }
        }
        if (ci != 3)
            return false;
        uint8_t c[3];
        for (int j = 0; j < 3; j++) {
            bool ok = intensity ? parse_fraction(channels[j], chlens[j], &c[j]) :
                                  parse_hex_channel(channels[j], chlens[j], &c[j]);
            if (!ok) return false;
        }
        *out = wsap_vtcore_rgb(c[0], c[1], c[2]);
        return true;
    }

    if (len == 6 || len == 3)
        return parse_hex_rgb(s, len / 3, out);

    return false;
}

struct lab { float l, a, b; };

static inline float lab_f(float t)
{
    return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f;
}

static inline float lab_f_inv(float t)
{
    float t3 = t * t * t;
    return t3 > 0.008856f ? t3 : (t - 16.0f / 116.0f) / 7.787f;
}

static inline uint8_t quantize_channel(float c)
{
    if (c < 0.0f)
        return 0;
    if (c > 1.0f)
        return 255;
    return (uint8_t)(c * 255.0f + 0.5f);
}

static struct lab lab_from_rgb(uint32_t argb)
{
    float r = srgb_linear[(argb >> 16) & 0xFF];
    float g = srgb_linear[(argb >> 8) & 0xFF];
    float b = srgb_linear[argb & 0xFF];

    float x = lab_f((r * 0.4124564f + g * 0.3575761f + b * 0.1804375f) / 0.95047f);
    float y = lab_f(r * 0.2126729f + g * 0.7151522f + b * 0.0721750f);
    float z = lab_f((r * 0.0193339f + g * 0.1191920f + b * 0.9503041f) / 1.08883f);

    struct lab lab;
    lab.l = 116.0f * y - 16.0f;
    lab.a = 500.0f * (x - y);
    lab.b = 200.0f * (y - z);
    return lab;
}

static uint32_t lab_to_rgb(struct lab lab)
{
    float y = (lab.l + 16.0f) / 116.0f;
    float x = lab.a / 500.0f + y;
    float z = y - lab.b / 200.0f;

    float xf = lab_f_inv(x) * 0.95047f;
    float yf = lab_f_inv(y);
    float zf = lab_f_inv(z) * 1.08883f;

    float r = linf_to_srgbf(xf * 3.2404542f - yf * 1.5371385f - zf * 0.4985314f);
    float g = linf_to_srgbf(-xf * 0.9692660f + yf * 1.8760108f + zf * 0.0415560f);
    float b = linf_to_srgbf(xf * 0.0556434f - yf * 0.2040259f + zf * 1.0572252f);

    return wsap_vtcore_rgb(quantize_channel(r), quantize_channel(g), quantize_channel(b));
}

static struct lab lab_lerp(float t, struct lab a, struct lab b)
{
    struct lab r;
    r.l = a.l + t * (b.l - a.l);
    r.a = a.a + t * (b.a - a.a);
    r.b = a.b + t * (b.b - a.b);
    return r;
}

static void derive_palette(uint32_t *p, const struct wsap_vtcore_theme *theme)
{
    struct lab base8[8];
    base8[0] = lab_from_rgb(theme->bg);
    for (int i = 1; i < 7; i++)
        base8[i] = lab_from_rgb(theme->ansi[i]);
    base8[7] = lab_from_rgb(theme->fg);

    for (int i = 0; i < 16; i++)
        p[i] = theme->ansi[i];
    p[WSAP_VTCORE_PALETTE_FG] = theme->fg;
    p[WSAP_VTCORE_PALETTE_BG] = theme->bg;
    p[WSAP_VTCORE_PALETTE_CURSOR] = theme->cursor;

    bool is_light_theme = base8[7].l < base8[0].l;
    if (is_light_theme) {
        struct lab tmp = base8[0];
        base8[0] = base8[7];
        base8[7] = tmp;
    }

    static const float steps[6] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
    struct lab c0[6], c1[6], c2[6], c3[6];
    for (int i = 0; i < 6; i++) {
        c0[i] = lab_lerp(steps[i], base8[0], base8[1]);
        c1[i] = lab_lerp(steps[i], base8[2], base8[3]);
        c2[i] = lab_lerp(steps[i], base8[4], base8[5]);
        c3[i] = lab_lerp(steps[i], base8[6], base8[7]);
    }

    struct lab c4[36], c5[36];
    int k = 0;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            c4[k] = lab_lerp(steps[j], c0[i], c1[i]);
            c5[k] = lab_lerp(steps[j], c2[i], c3[i]);
            k++;
        }
    }

    int idx = 16;
    for (int i = 0; i < 36; i++) {
        for (int j = 0; j < 6; j++)
            p[idx++] = lab_to_rgb(lab_lerp(steps[j], c4[i], c5[i]));
    }

    for (int i = 0; i < 24; i++) {
        float t = (float)(i + 1) / 25.0f;
        p[idx++] = lab_to_rgb(lab_lerp(t, base8[0], base8[7]));
    }
}

void wsap_vtcore_palette_init(struct palette *p, const struct wsap_vtcore_theme *theme)
{
    static const struct wsap_vtcore_theme zero;
    p->theme = theme && memcmp(theme, &zero, sizeof(zero)) ? *theme : builtin_theme;
    if (!p->theme.cursor)
        p->theme.cursor = builtin_theme.cursor;
    derive_palette(p->base, &p->theme);
    memcpy(p->colors, p->base, sizeof(p->colors));
    p->version++;
}

void wsap_vtcore_palette_set(struct palette *p, int slot, uint32_t argb)
{
    if ((unsigned)slot >= WSAP_VTCORE_PALETTE_SIZE || p->colors[slot] == argb)
        return;
    p->colors[slot] = argb;
    p->version++;
}

void wsap_vtcore_palette_reset(struct palette *p, int slot)
{
    if ((unsigned)slot < WSAP_VTCORE_PALETTE_SIZE)
        wsap_vtcore_palette_set(p, slot, p->base[slot]);
}

void wsap_vtcore_palette_reset_all(struct palette *p)
{
    if (memcmp(p->colors, p->base, sizeof(p->colors))) {
        memcpy(p->colors, p->base, sizeof(p->colors));
        p->version++;
    }
}
