#ifndef WSAP_VTCORE_CHARSET_H
#define WSAP_VTCORE_CHARSET_H

#include <stdbool.h>
#include <stdint.h>

/* ISO 2022 character sets (SCS). The terminal decodes UTF-8 input. A set
 * changes only the printable ASCII codes that GL or a single shift selects.
 * Zero is ASCII.
 */
enum charset {
    CHARSET_ASCII,
    CHARSET_DEC_GRAPHICS,
    CHARSET_DEC_SUPPLEMENTAL,
    CHARSET_DEC_TECHNICAL,
    /* ISO Latin-1 Supplemental, a 96-character set. */
    CHARSET_LATIN1,
    /* The national replacement sets (NRCS). They start with UK. */
    CHARSET_UK,
    CHARSET_DUTCH,
    CHARSET_FINNISH,
    CHARSET_FRENCH,
    CHARSET_FRENCH_CANADIAN,
    CHARSET_GERMAN,
    CHARSET_ITALIAN,
    CHARSET_NORWEGIAN_DANISH,
    CHARSET_PORTUGUESE,
    CHARSET_SPANISH,
    CHARSET_SWEDISH,
    CHARSET_SWISS,
};

/* The designations G0 to G3 and the shift state. In the all-zero value,
 * each G set holds ASCII, GL holds G0, GR holds G0, and no single shift
 * waits. A reset uses charset_state_initial, which puts G2 in GR.
 */
struct charset_state {
    uint8_t g[4];
    /* The G set in GL and the G set in GR, 0 to 3. */
    uint8_t gl, gr;
    /* 2 or 3 while a single shift (SS2 or SS3) waits for a character. */
    uint8_t ss;
};

static inline struct charset_state charset_state_initial(void)
{
    return (struct charset_state){ .gr = 2 };
}

/* A saved cursor keeps G0 to G3, GL, and GR in 24 bits: 5 bits for each
 * set, then 2 bits for GL and 2 bits for GR. It does not keep a single
 * shift. As in xterm, a restore keeps the single shift that waits.
 */
_Static_assert(CHARSET_SWISS < 32, "a set id must fit in 5 bits");
#define CHARSET_PACKED_INITIAL (2u << 22)

static inline uint32_t charset_pack(const struct charset_state *cs)
{
    return (uint32_t)cs->g[0] | (uint32_t)cs->g[1] << 5 | (uint32_t)cs->g[2] << 10 |
           (uint32_t)cs->g[3] << 15 | (uint32_t)cs->gl << 20 | (uint32_t)cs->gr << 22;
}

static inline void charset_unpack(struct charset_state *cs, uint32_t packed)
{
    for (int i = 0; i < 4; i++)
        cs->g[i] = (uint8_t)(packed >> (5 * i) & 31u);
    cs->gl = (uint8_t)(packed >> 20 & 3u);
    cs->gr = (uint8_t)(packed >> 22 & 3u);
}

struct wsap_vtcore;

/* Designate a set into G slot. inter is 0 or the intermediate byte of the
 * designator. c96 is true for the 96-character designators (ESC - . /).
 * An unknown designator does nothing.
 */
void wsap_vtcore_charset_designate(struct wsap_vtcore *t, int slot, char inter, char final,
                                   bool c96);

/* Apply a shift: SI, SO, LS2, LS3, LS1R, LS2R, LS3R, SS2, or SS3. ch is the
 * C0 byte or the final byte of the ESC sequence.
 */
void wsap_vtcore_charset_shift(struct wsap_vtcore *t, unsigned char ch);

/* Map a code point that starts a new cell. Use the set of the single shift
 * or of GL, and end the single shift. Return the code point to print.
 */
uint32_t wsap_vtcore_charset_map(struct wsap_vtcore *t, uint32_t cp);

#endif
