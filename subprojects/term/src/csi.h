#ifndef CSI_H
#define CSI_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#define MAX_PARAMS 16

struct csi {
    int params[MAX_PARAMS];
    int nparams;
    uint16_t subparams;
    char prefix, intermediate, final;
};

enum csi_phase { CSI_ENTRY, CSI_PARAMETERS, CSI_INTERMEDIATES, CSI_IGNORE };
enum csi_result { CSI_MORE, CSI_DISPATCH, CSI_DISCARD, CSI_CONTROL };

struct csi_parser {
    struct csi csi;
    enum csi_phase phase;
};

static inline void csi_parser_reset(struct csi_parser *p)
{
    p->csi.params[0] = 0;
    p->csi.nparams = 0;
    p->csi.subparams = 0;
    p->csi.prefix = p->csi.intermediate = p->csi.final = 0;
    p->phase = CSI_ENTRY;
}

static inline int csi_count(const struct csi *csi)
{
    return csi->params[0] > 0 ? csi->params[0] : 1;
}

static inline enum csi_result csi_read(struct csi_parser *restrict p, const char *restrict s,
                                     size_t len, size_t *restrict consumed)
{
    size_t i = 0;
    enum csi_result result = CSI_MORE;
    while (i < len) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch >= 0x80) {
            result = CSI_CONTROL;
            break;
        }
        i++;
        if (ch == 0x7f) continue;
        if (ch >= 0x40) {
            p->csi.final = (char)ch;
            result = p->phase == CSI_IGNORE ? CSI_DISCARD : CSI_DISPATCH;
            break;
        }
        if (p->phase == CSI_IGNORE) continue;

        if (ch >= '0' && ch <= ';') {
            if (p->phase == CSI_INTERMEDIATES) {
                p->phase = CSI_IGNORE;
                continue;
            }
            p->phase = CSI_PARAMETERS;
            int n = p->csi.nparams ? p->csi.nparams : 1;
            int value = p->csi.params[n - 1];
            for (;;) {
                if (ch <= '9') {
                    for (;;) {
                        int digit = ch - '0';
                        value = value < 214748364 ? value * 10 + digit : INT_MAX;
                        if (i == len) break;
                        ch = (unsigned char)s[i];
                        if (ch < '0' || ch > '9') break;
                        i++;
                    }
                } else {
                    p->csi.params[n - 1] = value;
                    if (n == MAX_PARAMS) {
                        p->phase = CSI_IGNORE;
                        break;
                    }
                    if (ch == ':') p->csi.subparams |= (uint16_t)(1u << n);
                    n++;
                    value = 0;
                }
                if (i == len) break;
                ch = (unsigned char)s[i];
                if (ch < '0' || ch > ';') break;
                i++;
            }
            p->csi.params[n - 1] = value;
            p->csi.nparams = n;
        } else if (ch >= '<') {
            if (p->phase != CSI_ENTRY || p->csi.prefix) p->phase = CSI_IGNORE;
            else p->csi.prefix = (char)ch;
        } else {
            if (p->phase == CSI_INTERMEDIATES) p->phase = CSI_IGNORE;
            else {
                p->phase = CSI_INTERMEDIATES;
                p->csi.intermediate = (char)ch;
            }
        }
    }
    *consumed = i;
    return result;
}

#endif
