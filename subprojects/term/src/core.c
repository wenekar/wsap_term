#include "compat.h"
#include "core.h"
#include "kernels.h"
#include "pen.h"
#include "screen.h"
#include "text/run.h"
#include "unicode/grapheme.h"
#include "unicode/unicode.h"
#include "unicode/utf8.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint16_t cluster_presentation(const uint32_t *cps, int len)
{
    if (len <= 1)
        return 0;
    bool pictographic = wsap_vtcore_unicode_extended_pictographic(cps[0]);
    if (len == 2) {
        uint32_t c1 = cps[1];
        if (c1 == 0xFE0F) return CELL_EMOJI;
        if (c1 == 0xFE0E) return CELL_TEXT;
        if (c1 == 0x20E3 || wsap_vtcore_grapheme_regional_indicator(c1) ||
            (pictographic && (c1 == 0x200D || wsap_vtcore_grapheme_emoji_modifier(c1) ||
                             (c1 >= 0xE0020 && c1 <= 0xE007F))))
            return CELL_EMOJI;
        return 0;
    }
    bool emoji = false;
    for (int i = 1; i < len; i++) {
        if (cps[i] == 0xFE0F)
            return CELL_EMOJI;
        if (cps[i] == 0xFE0E)
            return CELL_TEXT;
        if (cps[i] == 0x20E3 || wsap_vtcore_grapheme_regional_indicator(cps[i]) ||
            (pictographic && (cps[i] == 0x200D || wsap_vtcore_grapheme_emoji_modifier(cps[i]) ||
                             (cps[i] >= 0xE0020 && cps[i] <= 0xE007F))))
            emoji = true;
    }
    return emoji ? CELL_EMOJI : 0;
}

void wsap_vtcore_sc_print_glyph(struct wsap_vtcore *t, uint32_t cp, int width, uint16_t flags)
{
    uint32_t base = cp;
    if (WSAP_VTCORE_EXPECT(wsap_vtcore_grapheme_is_handle(cp), 0)) {
        uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
        int len = wsap_vtcore_gstore_expand(t->gstore, cp, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
        base = cps[0];
        flags = (flags & ~(CELL_EMOJI | CELL_TEXT)) | cluster_presentation(cps, len);
    }
    if (WSAP_VTCORE_EXPECT(width <= 0, 0))
        width = wsap_vtcore_text_class(base);
    if (width == 2 || (flags & CELL_EMOJI))
        width = 2;
    else
        width = 1;
    if (WSAP_VTCORE_EXPECT(flags & CELL_TEXT, 0))
        width = 1;

    if (WSAP_VTCORE_EXPECT(width == 1 && !t->pending_wrap && !t->insert_mode &&
                         !t->lr_margin_mode && t->cursor_x < t->cols, 1)) {
        struct srow *srow = wsap_vtcore_row(t, t->cursor_y);
        int cx = t->cursor_x;
        int len = srow->len;
        if (WSAP_VTCORE_EXPECT(srow->kind == ROW_CELLS && cx <= len, 1)) {
            struct cell *row = srow->cells;
            if (WSAP_VTCORE_EXPECT(!((cx < len && (row[cx].flags & CELL_TRAIL)) ||
                                   (cx + 1 < len && (row[cx + 1].flags & CELL_TRAIL))), 1)) {
                row[cx] = (struct cell){cp,
                    t->inverse ? t->cur_bg : t->cur_fg,
                    t->inverse ? t->cur_fg : t->cur_bg,
                    flags, t->cur_link};
                t->last_char = cp;
                cx++;
                if (cx > len) srow->len = (uint16_t)cx;
                srow->version++;
                if (cx < t->cols) {
                    t->cursor_x = cx;
                    t->pending_wrap = false;
                } else {
                    t->cursor_x = t->cols - 1;
                    t->pending_wrap = t->autowrap;
                }
                return;
            }
        }
    }

    if (WSAP_VTCORE_EXPECT(t->pending_wrap, 0)) {
        row_set_wrapped(wsap_vtcore_row(t, t->cursor_y), true);
        t->cursor_x = t->lr_margin_mode ? t->left_margin : 0;
        sc_linefeed(t, 1);
        t->pending_wrap = false;
    }

    int p = phys_row(t, t->cursor_y);
    struct cell *row = row_cells(t, p);
    struct srow *srow = &t->scr.slot[p];
    int cx = t->cursor_x;
    int cols = t->cols;
    int right = t->lr_margin_mode ? t->right_margin : cols - 1;

    if (WSAP_VTCORE_EXPECT(width == 2 && cols > 1 && cx == right && t->autowrap, 0)) {
        wsap_vtcore_row_fill_gap(srow, cx);
        clear_wide_edges(row, cx, right + 1, srow->len);
        row[cx] = (struct cell){ ' ', WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, CELL_WRAP_PAD, 0 };
        srow->len = (uint16_t)cols;
        srow->version++;
        row_set_wrapped(srow, true);
        t->cursor_x = t->lr_margin_mode ? t->left_margin : 0;
        sc_linefeed(t, 1);
        t->pending_wrap = false;
        p = phys_row(t, t->cursor_y);
        row = row_cells(t, p);
        srow = &t->scr.slot[p];
        cx = t->cursor_x;
    }

    int span = width > right + 1 - cx ? 1 : width;
    if (span == 2) {
        flags |= CELL_WIDE;
        srow->flags |= RF_WIDE;
    } else {
        flags &= ~CELL_WIDE;
    }
    wsap_vtcore_row_fill_gap(srow, cx);
    if (WSAP_VTCORE_EXPECT(t->insert_mode, 0)) {
        if (cx == srow->len)
            row[cx] = (struct cell){ ' ', WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG, 0, 0 };
        int move = right + 1 - cx - span;
        if (move > 0)
            memmove(&row[cx + span], &row[cx], (size_t)move * sizeof(struct cell));
        /* Cells move only up to the right margin. Past it, the row keeps its
         * length, so stale cells after the length stay hidden. */
        if (srow->len <= right) {
            int new_len = srow->len + span;
            srow->len = (uint16_t)(new_len > right + 1 ? right + 1 : new_len);
        }
        repair_wide_row(row, right, right + 1, cols, srow->len);
    }
    clear_wide_edges(row, cx, cx + span, srow->len);

    uint32_t fg = t->inverse ? t->cur_bg : t->cur_fg;
    uint32_t bg = t->inverse ? t->cur_fg : t->cur_bg;
    write_cell_span(row, cx, (struct cell){ cp, fg, bg, flags, t->cur_link }, span);
    if (cx + span > srow->len)
        srow->len = (uint16_t)(cx + span);
    srow->version++;
    t->last_char = cp;

    if (cx + span <= right) {
        t->cursor_x = cx + span;
        t->pending_wrap = false;
    } else {
        t->cursor_x = right;
        t->pending_wrap = t->autowrap;
    }
}

static inline struct cell *last_written_cell(struct wsap_vtcore *t)
{
    int right = (t->lr_margin_mode && t->cursor_x <= t->right_margin)
                    ? t->right_margin : t->cols - 1;
    int x = t->pending_wrap ? right : t->cursor_x - 1;
    if (t->lr_margin_mode && x < t->left_margin)
        return NULL;

    struct srow *srow = wsap_vtcore_row(t, t->cursor_y);
    if (x < 0 || x >= srow->len)
        return NULL;
    int p = phys_row(t, t->cursor_y);
    struct cell *row = row_cells(t, p);
    if (x > 0 && (row[x].flags & CELL_TRAIL))
        x--;
    return &row[x];
}

bool wsap_vtcore_sc_extend_cluster(struct wsap_vtcore *t, uint32_t cp, int klass)
{
    if (!can_extend_cluster(t->last_char, cp, klass))
        return false;

    struct cell *prev = last_written_cell(t);
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int len = prev && prev->c > ' ' ? wsap_vtcore_gstore_expand(t->gstore, prev->c, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN) : 0;
    if (!len)
        return joins_cluster(cp, klass);
    if (!cluster_joins(cps, len, cp, klass))
        return false;
    if (len >= WSAP_VTCORE_GRAPHEME_MAX_LEN)
        return true;
    cps[len++] = cp;

    struct cell cell = *prev;
    int p = phys_row(t, t->cursor_y);
    struct cell *row = row_cells(t, p);
    int x = (int)(prev - row);
    int span = (cell.flags & CELL_WIDE) && x + 1 < t->cols ? 2 : 1;
    struct srow *srow = &t->scr.slot[p];
    if (srow->len == x + span) {
        fill_cells(prev, span, WSAP_VTCORE_COLOR_FG, WSAP_VTCORE_COLOR_BG);
        srow->len = (uint16_t)x;
    } else {
        fill_cells(prev, span, cell.fg, cell.bg);
    }
    t->cursor_x = x;
    t->pending_wrap = false;
    t->last_char = wsap_vtcore_gstore_intern(t->gstore, cps, len);
    wsap_vtcore_sc_print_glyph(t, t->last_char, -1,
                   cell.flags & ~(CELL_WIDE | CELL_EMOJI | CELL_TEXT));

    prev = last_written_cell(t);
    if (prev) {
        row = row_cells(t, phys_row(t, t->cursor_y));
        prev->fg = cell.fg;
        prev->bg = cell.bg;
        if ((prev->flags & CELL_WIDE) && prev < row + t->cols - 1) {
            prev[1].fg = cell.fg;
            prev[1].bg = cell.bg;
        }
    }
    return true;
}

static void push_blank_lines(struct wsap_vtcore *t, size_t n)
{
    if (screen_has_history(t)) {
        wsap_vtcore_hist_push_blank(&t->hist, n);
    } else {
        t->scr.top_id += n;
        t->scr.top_seg = 0;
    }
}

static void scroll_up_bce(struct wsap_vtcore *t, size_t count)
{
    size_t rows = (size_t)t->rows;
    if (t->alternate || count <= rows) {
        for (size_t i = count < rows ? count : rows; i > 0; i--)
            wsap_vtcore_screen_scroll_up(t);
        return;
    }
    size_t rem = count - rows;
    if (!using_default_colors(t)) {
        size_t keep = screen_has_history(t) ? t->hist.line_limit : 0;
        for (size_t i = rows + (rem < keep ? rem : keep); i > 0; i--)
            wsap_vtcore_screen_scroll_up(t);
        return;
    }
    bool skip = screen_has_history(t) && rem >= t->hist.line_limit;
    if (!skip && !hist_is_open(&t->hist)) {
        skip = true;
        for (size_t y = 0; y < rows && skip; y++)
            skip = t->scr.slot[y].len == 0 && !(t->scr.slot[y].flags & RF_WRAPPED);
    }
    if (skip) {
        for (size_t y = 0; y < rows; y++)
            screen_new_row(t, &t->scr.slot[y]);
        push_blank_lines(t, count);
    } else {
        for (size_t i = 0; i < rows; i++)
            wsap_vtcore_screen_scroll_up(t);
        push_blank_lines(t, rem);
    }
    if (t->prompt_y > 0)
        t->prompt_y = 0;
    clamp_selection_after_scroll(t);
    t->full_gen++;
}

void wsap_vtcore_sc_linefeed_slow(struct wsap_vtcore *t, size_t n)
{
    if (n == 0)
        return;
    t->pending_wrap = false;
    wsap_vtcore_row(t, t->cursor_y)->version++;

    if (t->cursor_y > t->scroll_bottom) {
        if (t->cursor_y + (int)n < t->rows)
            t->cursor_y += (int)n;
        else
            t->cursor_y = t->rows - 1;
        return;
    }

    if (t->cursor_y + (int)n <= t->scroll_bottom) {
        t->cursor_y += (int)n;
        return;
    }

    size_t scroll = (size_t)(t->cursor_y + (int)n - t->scroll_bottom);
    t->cursor_y = t->scroll_bottom;

    if (WSAP_VTCORE_EXPECT(t->scroll_top == 0 && t->scroll_bottom == t->rows - 1, 1)) {
        if (WSAP_VTCORE_EXPECT(scroll == 1, 1))
            wsap_vtcore_screen_scroll_up(t);
        else
            scroll_up_bce(t, scroll);
    } else if (t->scroll_top > 0 || t->alternate) {
        int height = t->scroll_bottom - t->scroll_top + 1;
        if (height <= 0) height = 1;
        int count = scroll > (size_t)height ? height : (int)scroll;
        wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom, count, false, false);
    } else {
        int height = t->scroll_bottom - t->scroll_top + 1;
        if (height <= 0) height = 1;
        if (scroll <= (size_t)height) {
            wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom, (int)scroll, false, true);
        } else {
            wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom, height, false, true);
            size_t rem = scroll - (size_t)height;
            if (using_default_colors(t)) {
                push_blank_lines(t, rem);
                clamp_selection_after_scroll(t);
            } else {
                size_t cap = t->hist.line_limit > 0 ? t->hist.line_limit : 1;
                if (rem > cap)
                    rem = cap;
                while (rem > 0) {
                    int chunk = rem > (size_t)height ? height : (int)rem;
                    wsap_vtcore_screen_scroll_region(t, t->scroll_top, t->scroll_bottom, chunk, false, true);
                    rem -= (size_t)chunk;
                }
            }
        }
    }
}

void wsap_vtcore_sc_tab(struct wsap_vtcore *t, int n)
{
    int dir = n >= 0 ? 1 : -1;
    int count = n >= 0 ? n : -n;
    if (count > t->cols) count = t->cols;
    if (!t->custom_tabs) {
        if (dir > 0) {
            int next = (t->cursor_x & ~7) + (count << 3);
            t->cursor_x = next < t->cols ? next : t->cols - 1;
        } else {
            int prev = t->cursor_x > 0 ? (((t->cursor_x - 1) & ~7) - ((count - 1) << 3)) : 0;
            t->cursor_x = prev > 0 ? prev : 0;
        }
        t->pending_wrap = false;
        struct srow *cur = wsap_vtcore_row(t, t->cursor_y);
        if (cur) cur->version++;
        return;
    }
    int max_col = t->cols - 1;
    int cur_col = t->cursor_x;
    if (dir > 0) {
        int max_w = max_col >> 6;
        while (count-- > 0 && cur_col < max_col) {
            int x = cur_col + 1;
            int w = x >> 6;
            uint64_t mask = ~0ULL << (x & 63);
            uint64_t word = t->tabs[w] & mask;
            if (word) {
                cur_col = (w << 6) + wsap_vtcore_ctz64(word);
            } else {
                bool found = false;
                while (++w <= max_w) {
                    word = t->tabs[w];
                    if (word) {
                        cur_col = (w << 6) + wsap_vtcore_ctz64(word);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    cur_col = max_col;
                    break;
                }
            }
            if (cur_col >= max_col) {
                cur_col = max_col;
                break;
            }
        }
    } else {
        while (count-- > 0 && cur_col > 0) {
            int x = cur_col - 1;
            int w = x >> 6;
            int b = x & 63;
            uint64_t mask = (b == 63) ? ~0ULL : ((1ULL << (b + 1)) - 1);
            uint64_t word = t->tabs[w] & mask;
            if (word) {
                cur_col = (w << 6) + 63 - wsap_vtcore_clz64(word);
            } else {
                bool found = false;
                while (--w >= 0) {
                    word = t->tabs[w];
                    if (word) {
                        cur_col = (w << 6) + 63 - wsap_vtcore_clz64(word);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    cur_col = 0;
                    break;
                }
            }
            if (cur_col <= 0) {
                cur_col = 0;
                break;
            }
        }
    }
    t->cursor_x = cur_col;
    t->pending_wrap = false;
    struct srow *cur = wsap_vtcore_row(t, t->cursor_y);
    if (cur) cur->version++;
}

#define CELL_SGR_FLAGS (CELL_BOLD | CELL_FAINT | CELL_ITALIC | \
    CELL_STRIKE | CELL_OVERLINE | CELL_ANY_UNDERLINE)

enum deccara_color_src { DECCARA_SRC_CUR_FG = 0, DECCARA_SRC_CUR_BG = 1, DECCARA_SRC_CONST = 2 };

struct deccara_plan {
    uint16_t clear_flags;
    uint16_t set_flags;
    uint32_t const_fg;
    uint32_t const_bg;
    uint8_t fg_src;
    uint8_t bg_src;
};

/* Precomputes SGR modifications once for the whole rectangular area. */
static void deccara_plan_init(struct deccara_plan *p, const struct csi *csi, int first)
{
    p->clear_flags = 0;
    p->set_flags = 0;
    p->const_fg = WSAP_VTCORE_COLOR_FG;
    p->const_bg = WSAP_VTCORE_COLOR_BG;
    p->fg_src = DECCARA_SRC_CUR_FG;
    p->bg_src = DECCARA_SRC_CUR_BG;

    if (first >= csi->nparams) {
        p->clear_flags = CELL_SGR_FLAGS;
        p->fg_src = DECCARA_SRC_CONST;
        p->bg_src = DECCARA_SRC_CONST;
        return;
    }

    for (int i = first; i < csi->nparams; i++) {
        int v = csi->params[i];
        uint32_t color;
        bool ok;
        switch (v) {
        case 0:
            p->clear_flags = CELL_SGR_FLAGS;
            p->set_flags = 0;
            p->fg_src = DECCARA_SRC_CONST;
            p->bg_src = DECCARA_SRC_CONST;
            p->const_fg = WSAP_VTCORE_COLOR_FG;
            p->const_bg = WSAP_VTCORE_COLOR_BG;
            break;
        case 1: p->set_flags |= CELL_BOLD; p->clear_flags &= (uint16_t)~CELL_BOLD; break;
        case 2: p->set_flags |= CELL_FAINT; p->clear_flags &= (uint16_t)~CELL_FAINT; break;
        case 3: p->set_flags |= CELL_ITALIC; p->clear_flags &= (uint16_t)~CELL_ITALIC; break;
        case 4: {
            int style = 1;
            if (i + 1 < csi->nparams && (csi->subparams & (1u << (i + 1))))
                style = csi->params[++i];
            if (style < 0 || style > 5) style = 1;
            p->clear_flags |= CELL_ANY_UNDERLINE;
            p->set_flags = (uint16_t)((p->set_flags & ~CELL_ANY_UNDERLINE) | wsap_vtcore_ul_flags[style]);
            break;
        }
        case 7: {
            uint8_t s_tmp = p->fg_src; p->fg_src = p->bg_src; p->bg_src = s_tmp;
            uint32_t c_tmp = p->const_fg; p->const_fg = p->const_bg; p->const_bg = c_tmp;
            break;
        }
        case 9: p->set_flags |= CELL_STRIKE; p->clear_flags &= (uint16_t)~CELL_STRIKE; break;
        case 21:
            p->clear_flags |= CELL_ANY_UNDERLINE;
            p->set_flags = (uint16_t)((p->set_flags & ~CELL_ANY_UNDERLINE) | wsap_vtcore_ul_flags[2]);
            break;
        case 22:
            p->clear_flags |= (CELL_BOLD | CELL_FAINT);
            p->set_flags &= (uint16_t)~(CELL_BOLD | CELL_FAINT);
            break;
        case 23:
            p->clear_flags |= CELL_ITALIC;
            p->set_flags &= (uint16_t)~CELL_ITALIC;
            break;
        case 24:
            p->clear_flags |= CELL_ANY_UNDERLINE;
            p->set_flags &= (uint16_t)~CELL_ANY_UNDERLINE;
            break;
        case 29:
            p->clear_flags |= CELL_STRIKE;
            p->set_flags &= (uint16_t)~CELL_STRIKE;
            break;
        case 38:
            i += sgr_extended_color(csi->params, csi->nparams, csi->subparams, i, &color, &ok);
            if (ok) { p->fg_src = DECCARA_SRC_CONST; p->const_fg = color; }
            break;
        case 39:
            p->fg_src = DECCARA_SRC_CONST;
            p->const_fg = WSAP_VTCORE_COLOR_FG;
            break;
        case 48:
            i += sgr_extended_color(csi->params, csi->nparams, csi->subparams, i, &color, &ok);
            if (ok) { p->bg_src = DECCARA_SRC_CONST; p->const_bg = color; }
            break;
        case 49:
            p->bg_src = DECCARA_SRC_CONST;
            p->const_bg = WSAP_VTCORE_COLOR_BG;
            break;
        case 53: p->set_flags |= CELL_OVERLINE; p->clear_flags &= (uint16_t)~CELL_OVERLINE; break;
        case 55: p->clear_flags |= CELL_OVERLINE; p->set_flags &= (uint16_t)~CELL_OVERLINE; break;
        case 58:
            i += sgr_extended_color(csi->params, csi->nparams, csi->subparams, i, &color, &ok);
            break;
        default:
            if ((unsigned)(v - 30) < 8) {
                p->fg_src = DECCARA_SRC_CONST;
                p->const_fg = wsap_vtcore_color_index((uint8_t)(v - 30));
            } else if ((unsigned)(v - 40) < 8) {
                p->bg_src = DECCARA_SRC_CONST;
                p->const_bg = wsap_vtcore_color_index((uint8_t)(v - 40));
            } else if ((unsigned)(v - 90) < 8) {
                p->fg_src = DECCARA_SRC_CONST;
                p->const_fg = wsap_vtcore_color_index((uint8_t)(v - 90 + 8));
            } else if ((unsigned)(v - 100) < 8) {
                p->bg_src = DECCARA_SRC_CONST;
                p->const_bg = wsap_vtcore_color_index((uint8_t)(v - 100 + 8));
            }
            break;
        }
    }
}

static inline void apply_deccara_plan(struct cell *cell, const struct deccara_plan *p)
{
    uint32_t orig_fg = cell->fg;
    uint32_t orig_bg = cell->bg;
    cell->fg = (p->fg_src == DECCARA_SRC_CONST) ? p->const_fg : (p->fg_src == DECCARA_SRC_CUR_FG ? orig_fg : orig_bg);
    cell->bg = (p->bg_src == DECCARA_SRC_CONST) ? p->const_bg : (p->bg_src == DECCARA_SRC_CUR_BG ? orig_bg : orig_fg);
    cell->flags = (uint16_t)((cell->flags & ~p->clear_flags) | p->set_flags);
}

static void deccara_row(struct wsap_vtcore *t, int y, int x0, int x1, const struct deccara_plan *plan)
{
    int p = phys_row(t, y);
    struct cell *row = row_cells(t, p);
    struct srow *srow = wsap_vtcore_row(t, y);
    int len = srow->len;
    if (x0 > 0 && x0 < len && (row[x0].flags & CELL_TRAIL)) x0--;
    if (x1 + 1 < t->cols && x1 < len && (row[x1].flags & CELL_WIDE)) x1++;
    wsap_vtcore_row_fill_gap(srow, x1 + 1);
    for (int x = x0; x <= x1; x++)
        apply_deccara_plan(&row[x], plan);
    srow->version++;
}

void wsap_vtcore_sc_deccara(struct wsap_vtcore *t, const struct csi *csi)
{
    int top = csi->params[0] > 0 ? csi->params[0] - 1 : 0;
    int left = csi->nparams > 1 && csi->params[1] > 0 ? csi->params[1] - 1 : 0;
    int bottom = csi->nparams > 2 && csi->params[2] > 0 ? csi->params[2] - 1 : t->rows - 1;
    int right = csi->nparams > 3 && csi->params[3] > 0 ? csi->params[3] - 1 : t->cols - 1;
    if (top > bottom || left > right || top >= t->rows || left >= t->cols) return;
    if (bottom >= t->rows) bottom = t->rows - 1;
    if (right >= t->cols) right = t->cols - 1;

    struct deccara_plan plan;
    deccara_plan_init(&plan, csi, 4);

    if (t->decsace == 2) {
        for (int y = top; y <= bottom; y++)
            deccara_row(t, y, left, right, &plan);
    } else {
        deccara_row(t, top, left, top == bottom ? right : t->cols - 1, &plan);
        for (int y = top + 1; y < bottom; y++)
            deccara_row(t, y, 0, t->cols - 1, &plan);
        if (bottom > top)
            deccara_row(t, bottom, 0, right, &plan);
    }
}

void wsap_vtcore_sc_decaln(struct wsap_vtcore *t)
{
    struct cell fill = { .c = 'E', .fg = WSAP_VTCORE_COLOR_FG, .bg = WSAP_VTCORE_COLOR_BG };
    struct cell *row0 = NULL;
    for (int y = 0; y < t->rows; y++) {
        int p = phys_row(t, y);
        struct cell *row = row_cells(t, p);
        if (row0) {
            memcpy(row, row0, (size_t)t->cols * sizeof(struct cell));
        } else {
            for (int x = 0; x < t->cols; x++) row[x] = fill;
            row0 = row;
        }
        struct srow *srow = &t->scr.slot[p];
        srow->len = (uint16_t)t->cols;
        srow->flags = RF_TAIL_PADDED;
        srow->kind = ROW_CELLS;
        srow->version++;
    }
    sc_move(t, 0, 0);
}

void wsap_vtcore_sc_reset(struct wsap_vtcore *t)
{
    t->cursor_x = t->cursor_y = 0;
    t->saved = wsap_vtcore_saved_cursor_initial();
    t->prompt_y = -1;
    t->has_prompt = false;
    t->graphics = t->pending_wrap = false;
    t->last_char = 0;
    t->charsets[0] = t->charsets[1] = 'B';
    t->charset_active = 0;
    wsap_vtcore_tabs_clear(t);
    wsap_vtcore_tabs_default(t);
    wsap_vtcore_screen_erase_all(t);
    t->full_gen++;
}

void wsap_vtcore_sc_set_cursor_style(struct wsap_vtcore *t, int ps)
{
    if (ps == 0) {
        t->cursor_style = t->default_cursor_style;
        t->cursor_blink = t->default_cursor_blink;
    } else if (ps >= 1 && ps <= 6) {
        t->cursor_style = ps < 3 ? WSAP_VTCORE_CURSOR_BLOCK
            : ps < 5 ? WSAP_VTCORE_CURSOR_UNDERLINE : WSAP_VTCORE_CURSOR_BAR;
        t->cursor_blink = (ps & 1);
    }
    wsap_vtcore_row(t, t->cursor_y)->version++;
}

static inline bool next_byte_wraps(const char *buf, size_t len, size_t pos, uint32_t last)
{
    if (pos >= len) return false;
    uint32_t cp;
    if (!wsap_vtcore_utf8_decode_one(buf + pos, len - pos, &cp)) return false;
    int w = wsap_vtcore_text_class(cp);
    return w > 0 && !can_extend_cluster(last, cp, w);
}

static void flush_orig_rows(struct wsap_vtcore *t, struct screen *s,
                            int *orig_y, int *orig_rem, int count)
{
    int y = *orig_y;
    int end = y + count;
    *orig_y = end;
    *orig_rem -= count;
    struct hist_ascii_line q[64];
    while (y < end) {
        int p = s->top + y;
        struct srow *first = &s->slot[p >= s->rows ? p - s->rows : p];
        uintptr_t begin = (uintptr_t)t->ascii_tail.data;
        uintptr_t limit = begin + t->ascii_tail.cap;
        uintptr_t base = (uintptr_t)first->text.text;
        if (first->kind == ROW_ASCII && base >= begin && base < limit) {
            uint16_t ends[64 + 65];
            ends[0] = 0;
            int term = t->ascii_tail_crlf ? 2 : 1;
            int span = 0, lines = 0, closed = y;
            for (int j = y; j < end && j - y < 64; j++) {
                int p = s->top + j;
                struct srow *row = &s->slot[p >= s->rows ? p - s->rows : p];
                bool wrapped = (row->flags & RF_WRAPPED) != 0;
                size_t size = (size_t)row->len + (wrapped ? 0 : (size_t)term);
                if (row->kind != ROW_ASCII || (uintptr_t)row->text.text != base + span ||
                    size > limit - base - span || size + span > UINT16_MAX) break;
                span += row->len;
                if (!wrapped) {
                    span += term;
                    ends[++lines] = (uint16_t)span;
                    closed = j + 1;
                }
            }
            if (lines) {
                wsap_vtcore_hist_push_ascii_lf(&t->hist, (const char *)base, ends, lines, term);
                if (t->view.anchor.active) wsap_vtcore_hist_view_note_push(&t->view, &t->hist, t->cols);
                y = closed;
                continue;
            }
        }
        int n = 0;
        while (y + n < end && n < 64) {
            int p = s->top + y + n;
            struct srow *row = &s->slot[p >= s->rows ? p - s->rows : p];
            struct text_row *tr = &row->text;
            bool wrapped = (row->flags & RF_WRAPPED) != 0;
            if (wrapped && row->len < s->cols) break;
            if (row->kind == ROW_TEXT) {
                if (!(tr->flags & HLINE_NARROW) || tr->runs.len || tr->fill_to > tr->cells ||
                    !pen_attr_is_default(&tr->start) || !pen_attr_is_default(&tr->runs.pen)) break;
            } else if (row->kind != ROW_ASCII && row->len) break;
            q[n++] = (struct hist_ascii_line){
                .ptr = (const char *)tr->text, .len = row->len, .wrapped = wrapped,
            };
        }
        if (n) {
            wsap_vtcore_hist_push_ascii_lines(&t->hist, q, n, NULL);
            if (t->view.anchor.active) wsap_vtcore_hist_view_note_push(&t->view, &t->hist, t->cols);
            y += n;
        } else wsap_vtcore_screen_push_row(t, screen_get_row(s, y++));
    }
}

struct stream_line {
    uint32_t off;
    uint16_t len;
    uint8_t wrapped;
    uint8_t pad;
};

_Static_assert(sizeof(struct stream_line) == 8,
               "struct stream_line must stay 8 bytes");

struct stream_emit_ctx {
    struct wsap_vtcore *term;
    struct screen *screen;
    const char *buf;
    struct stream_line *q;
    const struct pen_attr *attr;
    size_t flushed_scrolled;
    int q_cap;
    int q_count;
    int orig_y;
    int orig_rem;
    int cols;
};

static WSAP_VTCORE_NOINLINE void flush_stream_lines(struct hist *h, const char *base,
                                                    const struct stream_line *q, int count,
                                                    const struct pen_attr *attr)
{
    struct hist_ascii_line batch[256];
    for (int j = 0; j < count; j++) {
        struct stream_line l = q[j];
        batch[j] = (struct hist_ascii_line){
            .ptr = base + l.off,
            .len = l.len,
            .wrapped = l.wrapped,
        };
    }
    wsap_vtcore_hist_push_ascii_lines(h, batch, count, attr);
}

static WSAP_VTCORE_NOINLINE void flush_stream_queue(struct stream_emit_ctx *ctx)
{
    if (ctx->orig_rem > 0)
        flush_orig_rows(ctx->term, ctx->screen, &ctx->orig_y, &ctx->orig_rem, ctx->orig_rem);
    int flush_n = ctx->q_count - ctx->q_cap;
    if (flush_n > 0) {
        flush_stream_lines(&ctx->term->hist, ctx->buf, ctx->q, flush_n, ctx->attr);
        if (ctx->q_cap > 0) {
            if (flush_n >= ctx->q_cap)
                memcpy(ctx->q, ctx->q + flush_n, (size_t)ctx->q_cap * sizeof(ctx->q[0]));
            else
                memmove(ctx->q, ctx->q + flush_n, (size_t)ctx->q_cap * sizeof(ctx->q[0]));
        }
    }
    ctx->q_count = ctx->q_cap;
    ctx->flushed_scrolled += (size_t)flush_n;
}

static inline void emit_stream_bulk_wrapped(struct stream_emit_ctx *ctx,
                                            const char *src, int bulk_lines)
{
    struct wsap_vtcore *t = ctx->term;
    struct screen *s = ctx->screen;
    int cols = ctx->cols;
    if (WSAP_VTCORE_EXPECT(t->pending_wrap, 0)) {
        row_set_wrapped(screen_get_row(s, s->rows - 1), true);
        t->pending_wrap = false;
        t->cursor_x = 0;
        wsap_vtcore_screen_scroll_up(t);
    }
    if (bulk_lines < ctx->q_cap) {
        const char *p = src;
        for (int r = 0; r < bulk_lines; r++, p += cols) {
            if (ctx->q_count == 256) {
                flush_stream_queue(ctx);
            }
            ctx->q[ctx->q_count++] = (struct stream_line){
                .off = (uint32_t)(p - ctx->buf),
                .len = (uint16_t)cols,
                .wrapped = 1,
                .pad = 0,
            };
        }
        return;
    }

    if (ctx->orig_rem > 0)
        flush_orig_rows(t, s, &ctx->orig_y, &ctx->orig_rem, ctx->orig_rem);
    if (ctx->q_count > 0) {
        flush_stream_lines(&t->hist, ctx->buf, ctx->q, ctx->q_count, ctx->attr);
        ctx->flushed_scrolled += (size_t)ctx->q_count;
        ctx->q_count = 0;
    }
    int hist_rows = bulk_lines - ctx->q_cap;
    if (hist_rows > 0) {
        wsap_vtcore_hist_push_ascii(&t->hist, src, hist_rows * cols, ctx->attr, true);
        ctx->flushed_scrolled += (size_t)hist_rows;
    }
    const char *sp = src + (size_t)hist_rows * (size_t)cols;
    for (int r = 0; r < ctx->q_cap; r++, sp += cols)
        ctx->q[r] = (struct stream_line){
            .off = (uint32_t)(sp - ctx->buf),
            .len = (uint16_t)cols,
            .wrapped = 1,
            .pad = 0,
        };
    ctx->q_count = ctx->q_cap;
}

static inline WSAP_VTCORE_ALWAYS_INLINE void emit_stream_row_fast(struct stream_emit_ctx *ctx,
                                                           const char *row_ptr,
                                                           int row_len, bool row_wrapped)
{
    if (WSAP_VTCORE_EXPECT(ctx->q_count == 256, 0))
        flush_stream_queue(ctx);
    ctx->q[ctx->q_count++] = (struct stream_line){
        .off = (uint32_t)(row_ptr - ctx->buf),
        .len = (uint16_t)row_len,
        .wrapped = (uint8_t)row_wrapped,
        .pad = 0,
    };
}

static inline WSAP_VTCORE_ALWAYS_INLINE void emit_stream_row(struct stream_emit_ctx *ctx,
                                                      const char *row_ptr,
                                                      int row_len, bool row_wrapped)
{
    if (WSAP_VTCORE_EXPECT(ctx->term->pending_wrap, 0)) {
        row_set_wrapped(screen_get_row(ctx->screen, ctx->screen->rows - 1), true);
        ctx->term->pending_wrap = false;
        ctx->term->cursor_x = 0;
        wsap_vtcore_screen_scroll_up(ctx->term);
    }
    emit_stream_row_fast(ctx, row_ptr, row_len, row_wrapped);
}

static WSAP_VTCORE_NOINLINE size_t stream_ascii_wrapped(struct wsap_vtcore *t, const char *buf, size_t len)
{
    int limit = len > INT32_MAX ? INT32_MAX : (int)len;
    int run = scan_ascii_run(buf, limit);
    if (!run) return 0;
    int cols = t->cols;
    int count = (run - 1) / cols;
    if (!count) return 0;
    struct screen *s = &t->scr;
    int rows = s->rows, keep = rows - 1;
    int scrolls = count + t->pending_wrap;
    int old_rows = keep + t->pending_wrap;
    if (t->pending_wrap) {
        row_set_wrapped(screen_get_row(s, rows - 1), true);
        t->pending_wrap = false;
        t->cursor_x = 0;
    }
    int old_count = scrolls < old_rows ? scrolls : old_rows;
    int orig_y = 0, orig_rem = old_rows;
    flush_orig_rows(t, s, &orig_y, &orig_rem, old_count);
    int extra = count - (keep - orig_rem);
    if (extra) wsap_vtcore_hist_push_ascii(&t->hist, buf, extra * cols, NULL, true);
    s->top = (s->top + scrolls % rows) % rows;
    const char *src = buf + extra * cols;
    uint8_t *copy_dst = NULL;
    const char *copy_src = src;
    size_t copy_len = 0;
    uint64_t uid = t->next_row_uid;
    for (int y = orig_rem; y < rows - 1; y++, src += cols) {
        int p = s->top + y;
        struct srow *dst = &s->slot[p >= rows ? p - rows : p];
        dst->uid = ++uid;
        dst->version++;
        dst->kind = ROW_ASCII;
        dst->len = (uint16_t)cols;
        dst->flags = RF_WRAPPED;
        struct text_row *tr = &dst->text;
        tr->text = dst->ascii;
        if (copy_len && tr->text != copy_dst + copy_len) {
            memcpy(copy_dst, copy_src, copy_len);
            copy_len = 0;
        }
        if (!copy_len) {
            copy_dst = tr->text;
            copy_src = src;
        }
        copy_len += (size_t)cols;
    }
    if (copy_len) memcpy(copy_dst, copy_src, copy_len);
    t->next_row_uid = uid;
    screen_new_row(t, screen_get_row(s, rows - 1));
    if (t->prompt_y > 0)
        t->prompt_y = t->prompt_y > scrolls ? t->prompt_y - scrolls : 0;
    clamp_selection_after_scroll(t);
    t->last_char = (unsigned char)buf[count * cols - 1];
    t->cursor_x = 0;
    t->pending_wrap = false;
    t->full_gen++;
    return (size_t)count * cols;
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static int long_lines_avx512(const uint16_t *ends, int *count)
{
    int j = 0;
    for (; j + 32 <= *count; j += 32) {
        __m512i start = _mm512_loadu_si512(ends + j);
        __m512i end = _mm512_loadu_si512(ends + j + 1);
        uint32_t bad = _mm512_cmpgt_epu16_mask(_mm512_sub_epi16(end, start),
                                              _mm512_set1_epi16(HIST_PAGE_USABLE / 2));
        if (bad) {
            *count = j + wsap_vtcore_ctz32(bad);
            break;
        }
    }
    return j;
}
#endif

/* Stream complete lines of printable ASCII. term is the line terminator:
 * 1 for LF in newline mode, 2 for CRLF. Both give the same screen and history.
 */
static WSAP_VTCORE_NOINLINE size_t stream_ascii_lines(struct wsap_vtcore *t, const char *buf, size_t len,
                                                      int term)
{
    int limit = len < UINT16_MAX ? (int)len : UINT16_MAX;
    size_t need = ((size_t)limit + 65) * sizeof(uint16_t);
    if (need > t->ascii_ends.cap) {
        void *data = realloc(t->ascii_ends.data, need);
        if (!data) return 0;
        t->ascii_ends.data = data;
        t->ascii_ends.cap = need;
    }
    uint16_t *ends = t->ascii_ends.data;
    int count = term == 2 ? scan_ascii_crlf(buf, limit, ends) : scan_ascii_lf(buf, limit, ends);
    int j = 0;
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        j = long_lines_avx512(ends, &count);
#endif
    for (; j < count; j++) {
        if (ends[j + 1] - ends[j] > HIST_PAGE_USABLE / 2) {
            count = j;
            break;
        }
    }
    struct screen *s = &t->scr;
    int cols = t->cols, rows = t->rows;
    int keep = rows - 1, first = count;
    int tail_rows = 0;
    while (first > 0 && tail_rows < keep) {
        first--;
        tail_rows += (int)rows_narrow(ends[first + 1] - ends[first] - term, cols);
    }
    if (!first) return 0;
    int skip = (tail_rows - keep) * cols;
    int source_start = ends[first] + skip;
    size_t tail_bytes = (size_t)(ends[count] - source_start);
    uint8_t *tail_data = t->ascii_tail.data;
    if (tail_bytes > t->ascii_tail.cap) {
        tail_data = malloc(tail_bytes);
        if (!tail_data) return 0;
    }
    int orig_y = 0, orig_rem = rows - 1;
    flush_orig_rows(t, s, &orig_y, &orig_rem, orig_rem);
    if (tail_data != t->ascii_tail.data) {
        free(t->ascii_tail.data);
        t->ascii_tail.data = tail_data;
        t->ascii_tail.cap = tail_bytes;
    }
    /* A one-row screen keeps no rows, and the buffer can still be NULL. */
    if (tail_bytes)
        memcpy(tail_data, buf + source_start, tail_bytes);
    t->ascii_tail_crlf = term == 2;
    wsap_vtcore_hist_push_ascii_lf(&t->hist, buf, ends, first, term);
    if (skip)
        wsap_vtcore_hist_push_ascii(&t->hist, buf + ends[first], skip, NULL, true);
    uint32_t last = t->last_char;
    for (int j = count; j > 0; j--) {
        if (ends[j] - ends[j - 1] > term) {
            last = (unsigned char)buf[ends[j] - 1 - term];
            break;
        }
    }
    s->top = 0;
    int y = 0;
    uint64_t uid = t->next_row_uid;
    for (int j = first; j < count; j++) {
        int start = ends[j] + (j == first ? skip : 0);
        int n = ends[j + 1] - term - start;
        do {
            int k = n > cols ? cols : n;
            struct srow *dst = &s->slot[y++];
            dst->uid = ++uid;
            dst->version++;
            dst->flags = n > cols ? RF_WRAPPED : 0;
            dst->len = (uint16_t)k;
            dst->kind = ROW_ASCII;
            dst->text.text = tail_data + start - source_start;
            n -= k;
            start += k;
        } while (n > 0);
    }
    t->next_row_uid = uid;
    screen_new_row(t, &s->slot[rows - 1]);
    if (t->prompt_y > 0) t->prompt_y = 0;
    clamp_selection_after_scroll(t);
    t->last_char = last;
    t->cursor_x = 0;
    t->pending_wrap = false;
    t->full_gen++;
    return ends[count];
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static inline uint64_t line_masks_avx512(const char *buf, uint64_t *nl)
{
    __m512i v = _mm512_loadu_si512((const void *)buf);
    *nl = _mm512_cmpeq_epi8_mask(v, _mm512_set1_epi8('\n'));
    return _mm512_cmpge_epu8_mask(_mm512_sub_epi8(v, _mm512_set1_epi8(0x20)),
                                  _mm512_set1_epi8(0x7f - 0x20));
}
#endif

#if defined(WSAP_VTCORE_KERNEL_AVX2)
WSAP_VTCORE_TARGET_AVX2
static inline uint64_t line_masks_avx2(const char *buf, uint64_t *nl)
{
    uint64_t lf = 0, controls = 0;
    for (int b = 0; b < 64; b += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(buf + b));
        __m256i d = _mm256_sub_epi8(v, _mm256_set1_epi8(0x20));
        __m256i printable = _mm256_cmpeq_epi8(
            _mm256_min_epu8(d, _mm256_set1_epi8(0x7f - 0x21)), d);
        lf |= (uint64_t)(uint32_t)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\n'))) << b;
        controls |= (uint64_t)(uint32_t)~_mm256_movemask_epi8(printable) << b;
    }
    *nl = lf;
    return controls;
}
#endif

static inline uint64_t line_masks(const char *buf, uint64_t *nl)
{
#if defined(WSAP_VTCORE_KERNEL_AVX2)
    if (wsap_vtcore_use_avx2())
        return line_masks_avx2(buf, nl);
#endif
    uint64_t lf = 0, controls = 0;
#if defined(__aarch64__)
    for (int b = 0; b < 64; b += 16) {
        uint8x16_t v = vld1q_u8((const uint8_t *)buf + b);
        lf |= (uint64_t)wsap_vtcore_lane_bits(vceqq_u8(v, vdupq_n_u8('\n'))) << b;
        controls |= (uint64_t)wsap_vtcore_lane_bits(vcgeq_u8(
            vsubq_u8(v, vdupq_n_u8(0x20)), vdupq_n_u8(0x7f - 0x20))) << b;
    }
#else
    for (int b = 0; b < 64; b++) {
        unsigned char c = (unsigned char)buf[b];
        if (c == '\n') lf |= UINT64_C(1) << b;
        if (c < 0x20 || c >= 0x7f) controls |= UINT64_C(1) << b;
    }
#endif
    *nl = lf;
    return controls;
}

static WSAP_VTCORE_NOINLINE void stream_queue_finish(struct stream_emit_ctx *ctx)
{
    int cols = ctx->cols;
    size_t queued_rows = 0;
    for (int j = 0; j < ctx->q_count; j++)
        queued_rows += ctx->q[j].wrapped ? 1 : rows_narrow(ctx->q[j].len, cols);
    size_t loop_scrolled = ctx->flushed_scrolled + queued_rows;
    if (loop_scrolled == 0)
        return;

    struct wsap_vtcore *t = ctx->term;
    struct screen *s = ctx->screen;
    int rows = s->rows;

    int push_orig = queued_rows < (size_t)ctx->orig_rem ? (int)queued_rows : ctx->orig_rem;
    if (push_orig > 0)
        flush_orig_rows(t, s, &ctx->orig_y, &ctx->orig_rem, push_orig);

    int needed = rows - 1 - ctx->orig_rem;
    int tail_rows = 0;
    int k = ctx->q_count;
    while (k > 0 && tail_rows < needed) {
        k--;
        struct stream_line pr = ctx->q[k];
        int r = (pr.wrapped || pr.len == 0) ? 1 : (int)rows_narrow(pr.len, cols);
        tail_rows += r;
    }

    int flush_n = k;
    if (flush_n > 0) {
        flush_stream_lines(&t->hist, ctx->buf, ctx->q, flush_n, ctx->attr);
    }

    int skip_rows = tail_rows - needed;
    if (skip_rows > 0) {
        struct stream_line pr = ctx->q[k];
        int skip_bytes = skip_rows * cols;
        if (skip_bytes > (int)pr.len) skip_bytes = (int)pr.len;
        wsap_vtcore_hist_push_ascii(&t->hist, ctx->buf + pr.off, skip_bytes, ctx->attr, true);
    }

    if (ctx->orig_rem == 0) {
        s->top = 0;
    } else {
        s->top += tail_rows;
        if (s->top >= rows)
            s->top -= rows;
    }
    int cur_y = ctx->orig_rem;
    bool prev_wrapped = false;
    for (int j = k; j < ctx->q_count && cur_y < rows - 1; j++) {
        struct stream_line pr = ctx->q[j];
        int p_len = pr.len;
        const char *p_ptr = ctx->buf + pr.off;

        if (p_len == 0) {
            int p = phys_row(t, cur_y);
            struct srow *dst = &s->slot[p];
            dst->uid = ++t->next_row_uid;
            dst->version++;
            uint8_t tail_padded = (dst->kind == ROW_CELLS && dst->len == 0)
                                  ? (dst->flags & RF_TAIL_PADDED) : 0;
            dst->kind = ROW_CELLS;
            dst->len = 0;
            dst->flags = tail_padded;
            prev_wrapped = false;
            cur_y++;
            continue;
        }

        int off_in_line = 0;
        if (j == k && skip_rows > 0) {
            off_in_line = skip_rows * cols;
            prev_wrapped = true;
        }

        while (off_in_line < p_len && cur_y < rows - 1) {
            int chunk = p_len - off_in_line;
            bool row_wraps = false;
            if (chunk > cols) {
                chunk = cols;
                row_wraps = true;
            } else if (pr.wrapped && off_in_line + chunk == p_len) {
                row_wraps = true;
            }

            int p = phys_row(t, cur_y);
            struct srow *dst = &s->slot[p];
            dst->uid = ++t->next_row_uid;
            dst->version++;

            struct pen_attr start = prev_wrapped ? *ctx->attr : pen_attr_default();
            dst->kind = ROW_TEXT;
            dst->len = 0;
            dst->flags = row_wraps ? RF_WRAPPED : 0;
            text_row_start(&dst->text, &start);
            dst->text.text = dst->ascii;
            text_append(dst, (const uint8_t *)(p_ptr + off_in_line),
                chunk, chunk, chunk, HLINE_NARROW,
                text_pen_from_attr(ctx->attr));

            prev_wrapped = row_wraps;
            off_in_line += chunk;
            cur_y++;
        }
    }
    while (cur_y < rows - 1) {
        int p = phys_row(t, cur_y);
        struct srow *dst = &s->slot[p];
        dst->uid = ++t->next_row_uid;
        dst->version++;
        dst->kind = ROW_CELLS;
        dst->len = 0;
        dst->flags = 0;
        cur_y++;
    }
    int bot_p = phys_row(t, rows - 1);
    struct srow *bot = &s->slot[bot_p];
    uint8_t bot_tail = (bot->kind == ROW_CELLS && bot->len == 0)
                       ? (bot->flags & RF_TAIL_PADDED) : 0;
    bot->uid = ++t->next_row_uid;
    bot->version++;
    bot->kind = ROW_CELLS;
    bot->len = 0;
    bot->flags = bot_tail;
    if (t->prompt_y > 0) {
        t->prompt_y = (size_t)t->prompt_y > loop_scrolled
                      ? t->prompt_y - (int)loop_scrolled : 0;
    }
    clamp_selection_after_scroll(t);
}

static inline WSAP_VTCORE_ALWAYS_INLINE size_t bulk_ascii_lines_body(struct wsap_vtcore *t, const char *buf,
                                                              size_t len, int isa)
{
    (void)isa;
    struct screen *s = &t->scr;
    int rows = s->rows;
    if (WSAP_VTCORE_EXPECT(rows > 256, 0))
        return 0;

    size_t i = 0;
    int cols = t->cols;
    uint32_t flags = wsap_vtcore_flags_pad(t);
    bool autowrap = t->autowrap;
    bool lnm = wsap_vtcore_lnm(t);
    int last_char = (int)t->last_char;

    struct pen_attr attr = {
        .fg = t->cur_fg,
        .bg = t->cur_bg,
        .flags = (uint16_t)flags,
        .link = (uint16_t)(flags >> 16),
    };
    if (t->pending_wrap &&
        (len == 0 || (unsigned char)buf[0] < 0x20 || (unsigned char)buf[0] >= 0x7f))
        return 0;

    struct stream_line q[256];
    struct stream_emit_ctx ctx = {
        .term = t,
        .screen = s,
        .buf = buf,
        .q = q,
        .attr = &attr,
        .flushed_scrolled = 0,
        .q_cap = rows - 1,
        .q_count = 0,
        .orig_y = 0,
        .orig_rem = rows - 1,
        .cols = cols,
    };

    while (i < len) {
        if (lnm && !t->pending_wrap) {
            size_t scan = i;
            while (scan + 64 <= len) {
                uint64_t nl, controls;
#if defined(WSAP_VTCORE_KERNEL_AVX512)
                if (isa == WSAP_VTCORE_ISA_AVX512)
                    controls = line_masks_avx512(buf + scan, &nl);
                else
#endif
                    controls = line_masks(buf + scan, &nl);
                if (controls != nl)
                    break;
                while (nl) {
                    size_t end = scan + (size_t)wsap_vtcore_ctz64(nl);
                    if (!autowrap && end - i > (size_t)cols)
                        goto narrow_done;
                    if (end > i)
                        last_char = (unsigned char)buf[end - 1];
                    emit_stream_row_fast(&ctx, buf + i, (int)(end - i), false);
                    i = end + 1;
                    nl &= nl - 1;
                }
                scan += 64;
                if (!autowrap && scan - i > (size_t)cols)
                    break;
                if (scan - i > HIST_PAGE_USABLE / 2)
                    break;
            }
        }
narrow_done:
        if (i >= len)
            break;
#if defined(WSAP_VTCORE_KERNEL_AVX512)
        if (isa == WSAP_VTCORE_ISA_AVX512 && WSAP_VTCORE_EXPECT(!lnm && !t->pending_wrap, 1)) {
            while (i + 64 <= len) {
                uint64_t nl;
                uint64_t mask = line_masks_avx512(buf + i, &nl);

                if (mask == 0) {
                    int total_avail = (int)(len - i);
                    int run = scan_ascii_run(buf + i, total_avail);

                    int bulk_lines = run / cols - (run % cols == 0);
                    if (bulk_lines > 0 && autowrap && (i + run == len || ((unsigned char)buf[i + run] >= 0x20 && (unsigned char)buf[i + run] < 0x7f) || (run % cols == 0))) {
                        emit_stream_bulk_wrapped(&ctx, buf + i, bulk_lines);
                        size_t write_cells = (size_t)bulk_lines * (size_t)cols;
                        last_char = (unsigned char)buf[i + write_cells - 1];
                        i += write_cells;
                        continue;
                    }

                    int limit = run > cols ? cols : run;
                    int eol = 0;
                    if (i + limit < len && buf[i + limit] == '\n' && lnm) {
                        eol = 1;
                    } else if (i + limit + 1 < len && buf[i + limit] == '\r' && buf[i + limit + 1] == '\n') {
                        eol = 2;
                    }

                    if (eol > 0) {
                        last_char = (unsigned char)buf[i + limit - 1];
                        emit_stream_row_fast(&ctx, buf + i, limit, false);
                        i += (size_t)(limit + eol);
                        continue;
                    }

                    bool wrapped = limit == cols && autowrap &&
                                   next_byte_wraps(buf, len, i + (size_t)limit,
                                                   (unsigned char)buf[i + limit - 1]);
                    if (!wrapped) break;
                    last_char = (unsigned char)buf[i + limit - 1];
                    emit_stream_row_fast(&ctx, buf + i, limit, true);
                    i += (size_t)limit;
                    continue;
                }

                int consumed = 0;
                for (int line = 0; line < 3 && consumed < 64; line++) {
                    uint64_t rest = mask >> consumed;
                    if (!rest) break;
                    int run = wsap_vtcore_ctz64(rest);
                    if (run > cols) break;
                    int pos = consumed + run;
                    int eol = buf[i + pos] == '\n' && lnm ? 1 :
                        buf[i + pos] == '\r' && i + pos + 1 < len &&
                        buf[i + pos + 1] == '\n' ? 2 : 0;
                    if (!eol) {
                        if (line == 0 && run < cols) goto done;
                        break;
                    }
                    if (run > 0)
                        last_char = (unsigned char)buf[i + pos - 1];
                    emit_stream_row_fast(&ctx, buf + i + consumed, run, false);
                    consumed = pos + eol;
                }
                if (!consumed) break;
                i += (size_t)consumed;
            }
            if (i >= len) break;
        }
#endif
        int limit = len - i > (size_t)cols ? cols : (int)(len - i);
        int run = scan_ascii_run(buf + i, limit);
        int eol = 0;
        if (WSAP_VTCORE_EXPECT(i + run + 1 < len && buf[i + run] == '\r' && buf[i + run + 1] == '\n', 1)) {
            eol = 2;
        } else if (i + run < len && buf[i + run] == '\n' && lnm) {
            eol = 1;
        }

        if (WSAP_VTCORE_EXPECT(eol > 0, 1)) {
            if (run > 0)
                last_char = (unsigned char)buf[i + run - 1];
            emit_stream_row(&ctx, buf + i, run, false);
            i += (size_t)(run + eol);
            continue;
        }

        if (!run)
            break;

        bool wrapped = run == cols && autowrap &&
                       next_byte_wraps(buf, len, i + (size_t)run,
                                       (unsigned char)buf[i + run - 1]);
        if (!wrapped) break;
        int full = cols + (int)scan_ascii_run(buf + i + cols,
                                              (int)(len - i) - cols);
        int bulk_lines = full / cols - (full % cols == 0);
        if (bulk_lines > 0 &&
            (i + (size_t)full == len ||
             ((unsigned char)buf[i + full] >= 0x20 &&
              (unsigned char)buf[i + full] < 0x7f) ||
             full % cols == 0)) {
            emit_stream_bulk_wrapped(&ctx, buf + i, bulk_lines);
            size_t write_cells = (size_t)bulk_lines * (size_t)cols;
            last_char = (unsigned char)buf[i + write_cells - 1];
            i += write_cells;
            continue;
        }
        last_char = (unsigned char)buf[i + run - 1];
        emit_stream_row(&ctx, buf + i, run, true);
        i += (size_t)run;
    }
#if defined(WSAP_VTCORE_KERNEL_AVX512)
done:
#endif
    if (i == 0)
        return 0;

    stream_queue_finish(&ctx);

    t->last_char = (uint32_t)last_char;
    t->cursor_x = 0;
    t->cursor_y = rows - 1;
    t->pending_wrap = false;
    t->full_gen++;
    return i;
}

#if defined(WSAP_VTCORE_KERNEL_AVX512)
WSAP_VTCORE_TARGET_AVX512
static WSAP_VTCORE_NOINLINE size_t bulk_ascii_lines_avx512(struct wsap_vtcore *t, const char *buf, size_t len)
{
    return bulk_ascii_lines_body(t, buf, len, WSAP_VTCORE_ISA_AVX512);
}
#endif

static WSAP_VTCORE_NOINLINE size_t bulk_ascii_lines_base(struct wsap_vtcore *t, const char *buf, size_t len)
{
    return bulk_ascii_lines_body(t, buf, len, WSAP_VTCORE_ISA_BASE);
}

size_t wsap_vtcore_sc_bulk_ascii_lines(struct wsap_vtcore *t, const char *buf, size_t len)
{
    if (WSAP_VTCORE_EXPECT(t->scr.rows > 256, 0)) return 0;
    if (len >= 1024 && t->autowrap && using_default_colors(t) && !wsap_vtcore_flags_pad(t)) {
        size_t done = 0;
        bool lnm = wsap_vtcore_lnm(t);
        if (!lnm)
            done = stream_ascii_wrapped(t, buf, len);
        if (!done && !t->pending_wrap)
            done = stream_ascii_lines(t, buf, len, lnm ? 1 : 2);
        if (done) return done;
        if (t->pending_wrap) {
            /* A wrapped line can continue from the previous write. Finish only that
             * line here, so that the stream takes the lines after it. */
            const char *lf = memchr(buf, '\n', len);
            if (lf && len - (size_t)(lf - buf) > 1024)
                len = (size_t)(lf - buf) + 1;
        }
    }
#if defined(WSAP_VTCORE_KERNEL_AVX512)
    if (wsap_vtcore_use_avx512())
        return bulk_ascii_lines_avx512(t, buf, len);
#endif
    return bulk_ascii_lines_base(t, buf, len);
}
