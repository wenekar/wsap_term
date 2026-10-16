/* Built as strict C11 and C17 with -pedantic-errors: the public header must not need newer C. */
#include <yasl/yasl.h>
#include <assert.h>

int main(void)
{
    enum yasl_backend backend = YASL_BACKEND_AUTO;
    struct yasl_surface_options options = {.title = "portable", .width = 64, .height = 64};
    assert(backend == 0 && options.type == YASL_SURFACE_CPU && YASL_VERSION_MAJOR == 0);
    return 0;
}
