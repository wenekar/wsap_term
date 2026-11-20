#include <stdio.h>
#include <stdlib.h>

#include "app/app.h"
#include "input/keybind.h"
#include "util/app_identity.h"
#include "platform/platform.h"
#include "ui/ui.h"
#include "ui/yasl/drop.h"

static const char *app_id_from_env(void)
{
    const char *name = getenv("WSAP_APP_NAME");
    if (!name || !name[0])
        name = getenv("TERM_NAME");
    if (!name || !name[0])
        name = "wsap_term";
    return name;
}

int main(int argc, char **argv)
{
    static char out_buf[1 << 14];
    setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf));

    platform_reserve_fds();
    static struct app_identity identity;
    if (!app_identity_init(&identity, app_id_from_env()))
        fprintf(stderr, "cannot set up the app identity\n");
    app_identity_set_current(&identity);
    input_config_init(&identity);
    int status = 0;
    /* The program opens a window next, so the GPU device starts first. */
    struct app *app = app_create_gpu_first(argc, argv, &status);
    if (!app)
        return status;

    int result = app_run(app);
    app_destroy(app);
    /* A drop writes no file, so this call removes nothing. */
    drop_cleanup();
    /* The UI caches are process-wide. Free them once, after the last app. */
    ui_shutdown();
    return result;
}
