#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include "render/render.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "font/font.h"
#include "paint/damage.h"
#include "util/geom.h"

void renderer_init(struct renderer *r);

#define RENDER_GRID_BANDS_MAX 8

struct render_damage { int y, height; };

struct render_row {
    uint64_t gen;
    int sel_c0, sel_c1;
    bool ui;
};

struct renderer_history {
    struct render_row *buf_rows[FRAMEBUFFER_BUFFERS];
    struct render_row *front_rows;
    int rows_cap;
    uint64_t buf_full_gen[FRAMEBUFFER_BUFFERS];
    int buf_w[FRAMEBUFFER_BUFFERS];
    int buf_h[FRAMEBUFFER_BUFFERS];
    struct vector2i buf_term_size[FRAMEBUFFER_BUFFERS];
    uint64_t buf_font_gen[FRAMEBUFFER_BUFFERS];
    uint32_t buf_fg[FRAMEBUFFER_BUFFERS], buf_bg[FRAMEBUFFER_BUFFERS], buf_cursor_color[FRAMEBUFFER_BUFFERS];
    int buf_cur_x[FRAMEBUFFER_BUFFERS];
    int buf_cur_y[FRAMEBUFFER_BUFFERS];
    int buf_cur_style[FRAMEBUFFER_BUFFERS];
    bool buf_reverse_video[FRAMEBUFFER_BUFFERS];
    uint64_t front_full_gen;
    int front_w, front_h, front_origin;
    struct vector2i front_term_size;
    uint64_t front_font_gen;
    int front_cur_x, front_cur_y, front_cur_style;
    uint32_t front_fg, front_bg, front_cursor_color;
    bool front_reverse_video, front_valid;
    struct rect2i buf_grid_bands[FRAMEBUFFER_BUFFERS][RENDER_GRID_BANDS_MAX];
    int buf_grid_bands_n[FRAMEBUFFER_BUFFERS];
    bool buf_bottom_ui[FRAMEBUFFER_BUFFERS];

    struct ui_damage ui_hist[FRAMEBUFFER_BUFFERS];
    uint64_t ui_frame;
    uint64_t buf_ui_frame[FRAMEBUFFER_BUFFERS];
    struct vector2i buf_ui_size[FRAMEBUFFER_BUFFERS];

    int ui_buf;
    struct ui_damage ui_frame_damage;
    struct ui_damage ui_repaint;
    uint32_t ui_base;
};

struct renderer {
    struct framebuffer_gpu *gpu;
    struct font_instance *font;
    struct font_instance *ui_font;
};

#endif
