#include "wsap_vtcore/wsap_vtcore.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef WSAP_VTCORE_TEST_PROJECT_VERSION
#error "define WSAP_VTCORE_TEST_PROJECT_VERSION as the project version, such as \"0.1.0\""
#endif

int main(void)
{
    char text[32];
    snprintf(text, sizeof(text), "%d.%d.%d",
        WSAP_VTCORE_VERSION_MAJOR, WSAP_VTCORE_VERSION_MINOR, WSAP_VTCORE_VERSION_PATCH);
    assert(!strcmp(text, WSAP_VTCORE_TEST_PROJECT_VERSION));

    int major, minor, patch;
    assert(sscanf(WSAP_VTCORE_TEST_PROJECT_VERSION, "%d.%d.%d", &major, &minor, &patch) == 3);
    assert(WSAP_VTCORE_VERSION_CODE == WSAP_VTCORE_VERSION(major, minor, patch));
    assert(wsap_vtcore_version() == WSAP_VTCORE_VERSION_CODE);

    assert(WSAP_VTCORE_VERSION(0, 2, 0) > WSAP_VTCORE_VERSION(0, 1, 255));
    assert(WSAP_VTCORE_VERSION(1, 0, 0) > WSAP_VTCORE_VERSION(0, 255, 255));
    return 0;
}
