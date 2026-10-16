#include "compat.h"
#include "kernels.h"
#include "wsap_vtcore.h"

WSAP_VTCORE_HOT
size_t wsap_vtcore_scan_printable_span(const char *buf, size_t len, unsigned char hi)
{
    /* The only entry point that can run before the first terminal. */
    wsap_vtcore_isa_init();
    return scan_printable_span(buf, len, hi);
}
