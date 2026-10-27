#include "osc.h"
#include "../internal.h"
#include <stdlib.h>
#include <string.h>

static const int8_t b64_lut[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static inline int b64_val(char c)
{
    return b64_lut[(unsigned char)c];
}

static bool b64_decode(const char *s, size_t n, char *out, size_t *out_len)
{
    size_t pad = 0;
    if (n > 0 && s[n - 1] == '=') {
        pad = (n > 1 && s[n - 2] == '=') ? ((n > 2 && s[n - 3] == '=') ? 3 : 2) : 1;
    }
    n -= pad;
    if (pad > 2 || (pad && n % 4 + pad != 4))
        return false;
    size_t rem = n % 4;
    if (rem == 1)
        return false;
    size_t i = 0, o = 0;
    for (; i + 4 <= n; i += 4) {
        int a = b64_val(s[i]), b = b64_val(s[i + 1]);
        int c = b64_val(s[i + 2]), d = b64_val(s[i + 3]);
        if ((a | b | c | d) < 0)
            return false;
        out[o++] = (char)(a << 2 | b >> 4);
        out[o++] = (char)(b << 4 | c >> 2);
        out[o++] = (char)(c << 6 | d);
    }
    if (rem >= 2) {
        int a = b64_val(s[i]), b = b64_val(s[i + 1]);
        if ((a | b) < 0)
            return false;
        out[o++] = (char)(a << 2 | b >> 4);
        if (rem == 3) {
            int c = b64_val(s[i + 2]);
            if (c < 0)
                return false;
            out[o++] = (char)(b << 4 | c >> 2);
        }
    }
    *out_len = o;
    return true;
}

#define WSAP_VTCORE_OSC52_MAX_PAYLOAD 102400
#define WSAP_VTCORE_OSC52_MAX_ENCODED (((WSAP_VTCORE_OSC52_MAX_PAYLOAD + 2) / 3) * 4)

void wsap_vtcore_osc52(struct wsap_vtcore *t, const char *params)
{
    const char *sep = strchr(params, ';');
    if (!sep || sep == params)
        return;
    const char *pd = sep + 1;
    if (!strcmp(pd, "?") || !t->cb.clipboard)
        return;
    size_t sel_len = (size_t)(sep - params);
    char sel[64];
    if (sel_len >= sizeof(sel))
        return;
    memcpy(sel, params, sel_len);
    sel[sel_len] = '\0';
    if (!*pd) {
        t->cb.clipboard(t->cb.ud, sel, "", 0);
        return;
    }
    size_t enc_len = strlen(pd);
    // Limit clipboard payload to 100 KiB (102400 bytes).
    // This prevents memory exhaustion and UI thread locking.
    if (enc_len > WSAP_VTCORE_OSC52_MAX_ENCODED)
        return;
    char *dec = (char *)pd;
    size_t dec_len;
    if (b64_decode(pd, enc_len, dec, &dec_len) && dec_len <= WSAP_VTCORE_OSC52_MAX_PAYLOAD)
        t->cb.clipboard(t->cb.ud, sel, dec, dec_len);
}
