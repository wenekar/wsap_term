#include "charset.h"
#include "internal.h"

#include <stddef.h>

/* DEC shows an undefined position as a reversed question mark. */
#define CHARSET_UNDEFINED 0x2426

struct charset_designator {
    char inter, final;
    uint8_t set;
};

/* The 94-character designators. They follow xterm. */
static const struct charset_designator charset_94[] = {
    { 0, 'B', CHARSET_ASCII },
    { 0, '0', CHARSET_DEC_GRAPHICS },
    { 0, '<', CHARSET_DEC_SUPPLEMENTAL },
    { '%', '5', CHARSET_DEC_SUPPLEMENTAL },
    { 0, '>', CHARSET_DEC_TECHNICAL },
    { 0, 'A', CHARSET_UK },
    { 0, '4', CHARSET_DUTCH },
    { 0, 'C', CHARSET_FINNISH },
    { 0, '5', CHARSET_FINNISH },
    { 0, 'R', CHARSET_FRENCH },
    { 0, 'f', CHARSET_FRENCH },
    { 0, 'Q', CHARSET_FRENCH_CANADIAN },
    { 0, '9', CHARSET_FRENCH_CANADIAN },
    { 0, 'K', CHARSET_GERMAN },
    { 0, 'Y', CHARSET_ITALIAN },
    { 0, 'E', CHARSET_NORWEGIAN_DANISH },
    { 0, '6', CHARSET_NORWEGIAN_DANISH },
    { 0, '`', CHARSET_NORWEGIAN_DANISH },
    { '%', '6', CHARSET_PORTUGUESE },
    { 0, 'Z', CHARSET_SPANISH },
    { 0, 'H', CHARSET_SWEDISH },
    { 0, '7', CHARSET_SWEDISH },
    { 0, '=', CHARSET_SWISS },
};

/* DEC Special Graphics from 0x5F to 0x7E. */
static const uint16_t charset_dec_graphics[32] = {
    0x00a0,
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
    0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
    0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
    0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

/* DEC Technical from 0x21 to 0x7E. Unicode has no glyphs for the sigma
 * parts at 0x31 to 0x37. The table uses the nearest symbols.
 */
#define U CHARSET_UNDEFINED
static const uint16_t charset_dec_technical[94] = {
    /* 0x21 */ 0x23b7, 0x250c, 0x2500, 0x2320, 0x2321, 0x2502, 0x23a1,
    /* 0x28 */ 0x23a3, 0x23a4, 0x23a6, 0x23a7, 0x23a9, 0x23ab, 0x23ad, 0x23a8,
    /* 0x30 */ 0x23ac, 0x23b2, 0x23b3, 0x2572, 0x2571, 0x231d, 0x231f, 0x27e9,
    /* 0x38 */ U,      U,      U,      U,      0x2264, 0x2260, 0x2265, 0x222b,
    /* 0x40 */ 0x2234, 0x221d, 0x221e, 0x00f7, 0x0394, 0x2207, 0x03a6, 0x0393,
    /* 0x48 */ 0x223c, 0x2243, 0x0398, 0x00d7, 0x039b, 0x21d4, 0x21d2, 0x2261,
    /* 0x50 */ 0x03a0, 0x03a8, U,      0x03a3, U,      U,      0x221a, 0x03a9,
    /* 0x58 */ 0x039e, 0x03a5, 0x2282, 0x2283, 0x2229, 0x222a, 0x2227, 0x2228,
    /* 0x60 */ 0x00ac, 0x03b1, 0x03b2, 0x03c7, 0x03b4, 0x03b5, 0x03c6, 0x03b3,
    /* 0x68 */ 0x03b7, 0x03b9, 0x03b8, 0x03ba, 0x03bb, U,      0x03bd, 0x2202,
    /* 0x70 */ 0x03c0, 0x03c8, 0x03c1, 0x03c3, 0x03c4, U,      0x0192, 0x03c9,
    /* 0x78 */ 0x03be, 0x03c5, 0x03b6, 0x2190, 0x2191, 0x2192, 0x2193,
};
#undef U

/* The national sets replace these 12 positions of ASCII:
 * # @ [ \ ] ^ _ ` { | } ~. Zero keeps the ASCII character.
 */
static const uint16_t charset_nrcs[][12] = {
    [CHARSET_UK - CHARSET_UK] = { 0xa3 },
    [CHARSET_DUTCH - CHARSET_UK] = {
        0xa3, 0xbe, 0x133, 0xbd, 0x7c, 0, 0, 0, 0xa8, 0x192, 0xbc, 0xb4 },
    [CHARSET_FINNISH - CHARSET_UK] = {
        0, 0, 0xc4, 0xd6, 0xc5, 0xdc, 0, 0xe9, 0xe4, 0xf6, 0xe5, 0xfc },
    [CHARSET_FRENCH - CHARSET_UK] = {
        0xa3, 0xe0, 0xb0, 0xe7, 0xa7, 0, 0, 0, 0xe9, 0xf9, 0xe8, 0xa8 },
    [CHARSET_FRENCH_CANADIAN - CHARSET_UK] = {
        0, 0xe0, 0xe2, 0xe7, 0xea, 0xee, 0, 0xf4, 0xe9, 0xf9, 0xe8, 0xfb },
    [CHARSET_GERMAN - CHARSET_UK] = {
        0, 0xa7, 0xc4, 0xd6, 0xdc, 0, 0, 0, 0xe4, 0xf6, 0xfc, 0xdf },
    [CHARSET_ITALIAN - CHARSET_UK] = {
        0xa3, 0xa7, 0xb0, 0xe7, 0xe9, 0, 0, 0xf9, 0xe0, 0xf2, 0xe8, 0xec },
    [CHARSET_NORWEGIAN_DANISH - CHARSET_UK] = {
        0, 0xc4, 0xc6, 0xd8, 0xc5, 0xdc, 0, 0xe4, 0xe6, 0xf8, 0xe5, 0xfc },
    [CHARSET_PORTUGUESE - CHARSET_UK] = {
        0, 0, 0xc3, 0xc7, 0xd5, 0, 0, 0, 0xe3, 0xe7, 0xf5, 0 },
    [CHARSET_SPANISH - CHARSET_UK] = {
        0xa3, 0xa7, 0xa1, 0xd1, 0xbf, 0, 0, 0, 0xb0, 0xf1, 0xe7, 0 },
    [CHARSET_SWEDISH - CHARSET_UK] = {
        0, 0xc9, 0xc4, 0xd6, 0xc5, 0xdc, 0, 0xe9, 0xe4, 0xf6, 0xe5, 0xfc },
    [CHARSET_SWISS - CHARSET_UK] = {
        0xf9, 0xe0, 0xe9, 0xe7, 0xea, 0xee, 0xe8, 0xf4, 0xe4, 0xf6, 0xfc, 0xfb },
};

void wsap_vtcore_charset_designate(struct wsap_vtcore *t, int slot, char inter, char final,
                                   bool c96)
{
    int set = -1;
    if (c96) {
        if (!inter && final == 'A')
            set = CHARSET_LATIN1;
    } else {
        for (size_t i = 0; i < sizeof(charset_94) / sizeof(charset_94[0]); i++) {
            if (charset_94[i].inter == inter && charset_94[i].final == final) {
                set = charset_94[i].set;
                break;
            }
        }
    }
    if (set < 0 || slot < 0 || slot > 3)
        return;
    /* As in xterm, a national set selects ASCII while DECNRCM is reset.
     * UK is also a VT100 set, so it stays. It changes text only in NRCS mode. */
    if (set > CHARSET_UK && !t->nrcs)
        set = CHARSET_ASCII;
    t->charsets.g[slot] = (uint8_t)set;
    wsap_vtcore_charset_sync(t);
}

void wsap_vtcore_charset_shift(struct wsap_vtcore *t, unsigned char ch)
{
    struct charset_state *cs = &t->charsets;
    switch (ch) {
    case 0x0f: cs->gl = 0; break; /* SI (LS0) */
    case 0x0e: cs->gl = 1; break; /* SO (LS1) */
    case 'n': cs->gl = 2; break;  /* LS2 */
    case 'o': cs->gl = 3; break;  /* LS3 */
    case '~': cs->gr = 1; break;  /* LS1R */
    case '}': cs->gr = 2; break;  /* LS2R */
    case '|': cs->gr = 3; break;  /* LS3R */
    case 'N': cs->ss = 2; break;  /* SS2 */
    case 'O': cs->ss = 3; break;  /* SS3 */
    default: return;
    }
    wsap_vtcore_charset_sync(t);
}

static uint32_t charset_nrcs_map(int set, uint32_t cp)
{
    int i;
    if (cp == '#')
        i = 0;
    else if (cp == '@')
        i = 1;
    else if (cp >= '[' && cp <= '`')
        i = 2 + (int)(cp - '[');
    else if (cp >= '{')
        i = 8 + (int)(cp - '{');
    else
        return cp;
    uint16_t to = charset_nrcs[set - CHARSET_UK][i];
    return to ? to : cp;
}

/* DEC Supplemental Graphics: the code plus 0x80 in the DEC Multinational
 * set. It differs from Latin-1 only at these positions. */
static uint32_t charset_dec_supplemental(uint32_t cp)
{
    uint32_t c = cp | 0x80;
    switch (c) {
    case 0xa8: return 0x00a4;
    case 0xd7: return 0x0152;
    case 0xdd: return 0x0178;
    case 0xf7: return 0x0153;
    case 0xfd: return 0x00ff;
    case 0xa4: case 0xa6: case 0xac: case 0xad: case 0xae: case 0xaf:
    case 0xb4: case 0xb8: case 0xbe: case 0xd0: case 0xde: case 0xf0: case 0xfe:
        return CHARSET_UNDEFINED;
    default:
        return c;
    }
}

uint32_t wsap_vtcore_charset_map(struct wsap_vtcore *t, uint32_t cp)
{
    struct charset_state *cs = &t->charsets;
    int set = cs->g[cs->ss ? cs->ss : cs->gl];
    if (cs->ss) {
        cs->ss = 0;
        wsap_vtcore_charset_sync(t);
    }
    /* SP and DEL are not in a 94-character set. In GL, a 96-character set
     * also keeps them. */
    if (cp - 0x21u > 0x7eu - 0x21u)
        return cp;
    switch (set) {
    case CHARSET_DEC_GRAPHICS:
        return cp >= 0x5f ? charset_dec_graphics[cp - 0x5f] : cp;
    case CHARSET_DEC_SUPPLEMENTAL:
        return charset_dec_supplemental(cp);
    case CHARSET_DEC_TECHNICAL:
        return charset_dec_technical[cp - 0x21];
    case CHARSET_LATIN1:
        return cp | 0x80;
    case CHARSET_UK:
        /* As in xterm, UK replaces # only in NRCS mode. */
        return t->nrcs ? charset_nrcs_map(set, cp) : cp;
    case CHARSET_ASCII:
        return cp;
    default:
        return charset_nrcs_map(set, cp);
    }
}
