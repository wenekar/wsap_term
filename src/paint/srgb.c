#include <math.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>

#include "paint/blend.h"

/* Built by the first blend that needs them. The glyph cache blends its
 * tiles with them, so they sit below the renderer. */
uint16_t render_srgb_to_linear[258];
alignas(64) uint8_t render_linear_to_srgb[65540];
_Atomic bool render_srgb_luts_ready;

void render_build_srgb_luts(void)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (atomic_load_explicit(&render_srgb_luts_ready, memory_order_acquire))
        return;
    pthread_mutex_lock(&mu);
    if (!atomic_load_explicit(&render_srgb_luts_ready, memory_order_relaxed)) {
        for (int i = 0; i < 256; i++) {
            double c = i / 255.0;
            double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
            render_srgb_to_linear[i] = (uint16_t)(l * 65535.0 + 0.5);
        }
        for (int i = 0; i < 65536; i++) {
            double l = i / 65535.0;
            double c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
            render_linear_to_srgb[i] = (uint8_t)(c * 255.0 + 0.5);
        }
        atomic_store_explicit(&render_srgb_luts_ready, true, memory_order_release);
    }
    pthread_mutex_unlock(&mu);
}
