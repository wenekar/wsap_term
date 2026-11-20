#include "desktop/window.h"
#include "paint/scene.h"

int main(void)
{
    enum { WIDTH = 320, HEIGHT = 240 };
    static uint32_t pixels[WIDTH * HEIGHT];
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            bool dark = ((x / 32) + (y / 32)) % 2;
            pixels[y * WIDTH + x] = dark ? 0xff25283du : 0xff42c6a5u;
        }
    }
    return wsap_window_show_image("WSAP Pixel Example", &(struct ui_image){
        .pixels = pixels,
        .size = { .x = WIDTH, .y = HEIGHT },
    });
}
