#include "compat.h"
#include "kernels.h"
#include "wsap_vtcore.h"

WSAP_VTCORE_HOT
size_t wsap_vtcore_scan_printable_span(const char *buf, size_t len, unsigned char hi)
{
    /* The kernels build their limit from hi in one-byte arithmetic, so they
     * require hi above 0x20. At or below 0x20 no byte is in range. */
    if (!buf || hi <= 0x20)
        return 0;
    wsap_vtcore_isa_init();
    return scan_printable_span(buf, len, hi);
}
