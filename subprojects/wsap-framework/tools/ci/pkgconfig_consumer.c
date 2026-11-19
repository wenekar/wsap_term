#include <string.h>

#include "ui/ui.h"
#include "ui/widgets/label.h"

int main(void)
{
    struct ui_label label;
    ui_label_init(&label);
    if (!ui_label_set_text(&label, "wsap framework"))
        return 1;
    if (strcmp(ui_label_text(&label), "wsap framework") != 0)
        return 1;
    ui_shutdown();
    return 0;
}
