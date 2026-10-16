#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "app/thumbnail.h"
#include "font/font.h"
#include "helpers/app_fixture.h"
#include "term_api.h"
#include "paint/scene.h"

static void thumbnail_tracks_content(void)
{
    struct test_app fixture = {0};
    test_app_init(&fixture, NULL, (struct vector2i){ .x = 800, .y = 600 });
    struct wsap_vtcore *term = tt_new(40, 10);
    assert(term);
    tt_write(term, "hello", 5);

    void *state = NULL;
    struct ui_image image = {0};
    assert(app_thumbnail_capture(fixture.app, &state, font_instance_default(),
                                 term, 100, &image));
    assert(image.size.x == 100 && image.size.y > 0);
    uint64_t gen = image.gen;
    size_t bytes = (size_t)image.size.x * image.size.y * sizeof(*image.pixels);
    uint32_t *before = malloc(bytes);
    assert(before);
    memcpy(before, image.pixels, bytes);
    assert(app_thumbnail_capture(fixture.app, &state, font_instance_default(),
                                 term, 100, &image));
    assert(image.gen == gen);

    tt_write(term, "\r\nworld", 7);
    assert(app_thumbnail_capture(fixture.app, &state, font_instance_default(),
                                 term, 100, &image));
    assert(image.gen != gen);
    assert((size_t)image.size.x * image.size.y * sizeof(*image.pixels) == bytes);
    assert(memcmp(before, image.pixels, bytes) != 0);
    free(before);

    gen = image.gen;
    struct font_instance *other = font_instance_acquire(
        font_instance_size_pt(font_instance_default()) + 2,
        font_instance_scale_120(font_instance_default()));
    assert(other);
    assert(app_thumbnail_capture(fixture.app, &state, other, term, 100, &image));
    assert(image.gen != gen);
    font_instance_unref(other);

    app_thumbnail_release(state);
    wsap_vtcore_free(term);
    test_app_fini(&fixture);
}

int main(void)
{
    thumbnail_tracks_content();
    return 0;
}
