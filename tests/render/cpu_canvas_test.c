#define _POSIX_C_SOURCE 200809L
/*
 * Tests of the CPU canvas that render_draw() takes on demand
 * (framebuffer.acquire_pixels).
 *
 * A Metal window draws its frames into a drawable, so it keeps no canvas in
 * the heap. The renderer asks for a canvas only when the CPU paints the
 * frame. These cases go through render_draw() and check the pixels and the
 * damage of that path against a canvas that the framebuffer holds.
 */
#include "helpers/render_draw.h"

static struct font_instance *font;
static struct renderer *rend;
static int cw, ch;

#define COLS 40
#define ROWS 12

/* The terminal of one case, and the canvas that only the supplier has. */
struct fixture {
    struct tterm tt;
    struct tcanvas lazy; /* pixels reachable through the supplier alone */
    struct tcanvas ref;  /* pixels held by the framebuffer */
    uint32_t *before;
    /* The supplier: it counts its calls and can fail on demand. */
    int calls;
    bool provide;
    /* The size that the renderer asked for. */
    struct vector2i asked_size;
    /* The frame asks for a drawable, so the GPU decides the frame first. */
    bool attempt;
};

static struct vector2i canvas_size(void)
{
    return (struct vector2i){ .x = COLS * cw, .y = ROWS * ch };
}

static uint32_t *supply(void *ud, struct vector2i size)
{
    struct fixture *f = ud;
    f->calls++;
    f->asked_size = size;
    if (!f->provide || !vector2i_eq(size, canvas_size()))
        return NULL;
    return f->lazy.px;
}

/* The layer gives no drawable, so a frame that the GPU attempts is dropped.
 * The backend reads no drawable that this call did not give it. */
static void *no_drawable(void *ud)
{
    (void)ud;
    return NULL;
}

static void fx_open(struct fixture *f)
{
    *f = (struct fixture){ .provide = true };
    tterm_open(&f->tt, COLS, ROWS);
    for (int i = 0; i < ROWS; i++) {
        char line[64];
        snprintf(line, sizeof(line), "line %d: \033[3mWf\033[0m -> \033[38;5;%dmcolor\033[0m%s", i,
                 100 + i, i + 1 < ROWS ? "\r\n" : "");
        tterm_feed(&f->tt, line);
    }
    struct vector2i c = canvas_size();
    tcanvas_init(&f->lazy, c.x, c.y);
    tcanvas_init(&f->ref, c.x, c.y);
    f->before = malloc(tcanvas_px(&f->ref) * sizeof(uint32_t));
    TCHECK(f->before, "before buffer");
}

static void fx_close(struct fixture *f)
{
    free(f->before);
    tcanvas_free(&f->lazy);
    tcanvas_free(&f->ref);
    tterm_close(&f->tt);
}

/* One frame with no canvas in the framebuffer. The renderer must take the
 * canvas from the supplier. `attempt` adds the request for a drawable, so
 * the frame asks the GPU first. */
static struct render_result draw_demand(struct fixture *f, const struct render_frame *frame)
{
    struct framebuffer fb = {
        .size = f->ref.size,
        .fresh = f->lazy.fresh,
        .history = &f->lazy.history,
        .acquire = f->attempt ? no_drawable : NULL,
        .acquire_pixels = supply,
        .acquire_pixels_ud = f,
    };
    f->lazy.fresh = false;
    return render_draw(rend, &fb, frame);
}

/* The pixels of both canvases and the damage of the on-demand frame must
 * match the frame with a canvas in the framebuffer. */
static void draw_both(struct fixture *f, const char *what)
{
    const struct terminal_frame *tf = tterm_frame(&f->tt, true);
    struct render_frame frame = tframe(tf, font, f->ref.size);
    memcpy(f->before, f->lazy.px, tcanvas_px(&f->lazy) * sizeof(uint32_t));
    int calls = f->calls;
    f->attempt = false;
    struct render_result demand = draw_demand(f, &frame);
    TCHECK(f->calls == calls + 1, "%s: the supplier ran %d times for one frame", what,
           f->calls - calls);
    TCHECK(vector2i_eq(f->asked_size, f->ref.size), "%s: the supplier saw %dx%d", what,
           f->asked_size.x, f->asked_size.y);
    struct render_result held = tdraw(rend, &f->ref, &frame);
    TCHECK(!demand.presented, "%s: a canvas-only frame presented", what);
    tcheck_same(what, f->lazy.px, f->ref.px, f->ref.size);
    tcheck_damage(what, f->before, f->lazy.px, f->ref.size, &demand.damage);
    TCHECK(tdamage_area(&demand.damage, f->ref.size) ==
               tdamage_area(&held.damage, f->ref.size),
           "%s: the damage area differs from the frame with a canvas", what);
}

/* A canvas on demand gives the same pixels and damage as a canvas in the
 * framebuffer, over a first paint, idle frames, typing, and a scroll. */
static void test_on_demand_canvas(void)
{
    struct fixture f;
    fx_open(&f);
    f.lazy.fresh = true;
    f.ref.fresh = true;
    draw_both(&f, "first frame");
    uint32_t *first = f.lazy.px;
    for (int i = 0; i < 3; i++)
        draw_both(&f, "idle");
    for (int i = 0; i < 4; i++) {
        tterm_feed(&f.tt, i % 2 ? "x" : "\033[3mW\033[23m");
        draw_both(&f, "typing");
    }
    tterm_feed(&f.tt, "\033[12;1H\r\nnew line");
    draw_both(&f, "scroll");
    /* The frames kept the canvas size, so the supplier gave the same buffer
     * to every frame. */
    TCHECK(f.lazy.px == first, "a frame of the same size got a new canvas");
    fx_close(&f);
}

/* A supplier that fails paints nothing, and the surface forgets its state,
 * so the next frame paints every pixel again. */
static void test_failed_supplier(void)
{
    struct fixture f;
    fx_open(&f);
    f.lazy.fresh = true;
    f.ref.fresh = true;
    draw_both(&f, "setup");
    f.provide = false;
    int calls = f.calls;
    const struct terminal_frame *tf = tterm_frame(&f.tt, true);
    struct render_frame frame = tframe(tf, font, f.ref.size);
    struct render_result res = draw_demand(&f, &frame);
    TCHECK(f.calls == calls + 1, "the supplier did not run for the failed frame");
    TCHECK(res.damage.any && res.damage.all, "a frame without a canvas must report full damage");
    TCHECK(!res.presented, "a frame without a canvas presented");
    /* The canvas is untouched. */
    tcheck_same("failed supplier", f.lazy.px, f.ref.px, f.ref.size);

    f.provide = true;
    tterm_feed(&f.tt, "after");
    tf = tterm_frame(&f.tt, true);
    frame = tframe(tf, font, f.ref.size);
    res = draw_demand(&f, &frame);
    TCHECK(res.damage.all, "the frame after a failure must paint every pixel");
    tdraw(rend, &f.ref, &frame);
    tcheck_same("after failure", f.lazy.px, f.ref.px, f.ref.size);
    fx_close(&f);
}

/* A frame with no canvas and no supplier paints nothing. */
static void test_no_canvas(void)
{
    struct fixture f;
    fx_open(&f);
    const struct terminal_frame *tf = tterm_frame(&f.tt, true);
    struct render_frame frame = tframe(tf, font, f.ref.size);
    struct framebuffer fb = { .size = f.ref.size, .history = &f.lazy.history };
    struct render_result res = render_draw(rend, &fb, &frame);
    TCHECK(res.damage.any && res.damage.all, "a frame with no canvas must report full damage");
    TCHECK(!res.presented, "a frame with no canvas presented");
    TCHECK(fb.pixels == NULL, "a frame with no supplier got pixels");
    fx_close(&f);
}

/* A frame with a drawable asks the GPU first. When the frame is dropped, no
 * canvas is needed. The CPU-only build has no GPU, so the frame falls back
 * to the canvas of the supplier. */
static void test_drawable_attempt(void)
{
    struct fixture f;
    fx_open(&f);
    f.lazy.fresh = true;
    f.ref.fresh = true;
    draw_both(&f, "setup");
    const struct terminal_frame *tf = tterm_frame(&f.tt, true);
    struct render_frame frame = tframe(tf, font, f.ref.size);
    int calls = f.calls;
    f.attempt = true;
    struct render_result res = draw_demand(&f, &frame);
    if (renderer_device(rend)) {
        TCHECK(!res.damage.any && !res.presented, "a dropped frame must damage nothing");
        TCHECK(f.calls == calls, "a dropped frame took a canvas");
    } else {
        TCHECK(f.calls == calls + 1, "the CPU-only build must take the canvas and paint");
        TCHECK(!res.presented && res.damage.any, "the CPU-only build must paint the frame");
    }
    fx_close(&f);
}

int main(void)
{
    TCHECK(render_init(), "render_init");
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    font = font_instance_default();
    TCHECK(font, "font");
    cw = font_instance_metrics(font)->cell_w;
    ch = font_instance_metrics(font)->cell_h;
    rend = renderer_create(NULL);
    TCHECK(rend, "renderer");
    test_on_demand_canvas();
    test_failed_supplier();
    test_no_canvas();
    test_drawable_attempt();
    renderer_destroy(rend);
    render_shutdown();
    puts("PASS: cpu canvas");
    return 0;
}
