#include <stdlib.h>

#include "util/name.h"

const char *app_name(void)
{
    const char *name = getenv("WSAP_APP_NAME");
    if (!name || !name[0])
        name = getenv("TERM_NAME");
    if (!name || !name[0])
        name = "wsap_term";
    return name;
}
