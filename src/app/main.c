#include <stdio.h>

#include "app/app.h"
#include "input/keybind.h"
#include "platform/platform.h"
#include "ui/ui.h"

int main(int argc, char **argv)
{
    static char out_buf[1 << 14];
    setvbuf(stdout, out_buf, _IOFBF, sizeof(out_buf));

    platform_reserve_fds();
    input_config_init();
    int status = 0;
    struct app *app = app_create(argc, argv, &status);
    if (!app)
        return status;

    int result = app_run(app);
    app_destroy(app);
    /* The process owns the UI tables. Free them once, after the last app. */
    ui_shutdown();
    return result;
}
