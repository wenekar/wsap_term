#define _GNU_SOURCE

#include "util/app_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"

static const struct app_identity *current;

/* Copies "<base>/<rel>" into `out`. */
static bool join(const char *base, const char *rel, char *out, size_t sz)
{
    if (!base || !base[0])
        return false;
    int n = snprintf(out, sz, "%s/%s", base, rel);
    return n > 0 && n < (int)sz;
}

bool app_identity_init(struct app_identity *out, const char *app_id)
{
    if (!out || !app_id || !app_id[0])
        return false;
    *out = (struct app_identity){0};
    if (strlen(app_id) >= sizeof(out->id))
        return false;
    snprintf(out->id, sizeof(out->id), "%s", app_id);
    if (!xdg_config_home(out->config_dir, sizeof(out->config_dir)) ||
        !xdg_cache_home(out->cache_dir, sizeof(out->cache_dir)))
        return false;
    size_t len = strlen(out->config_dir);
    if (snprintf(out->config_dir + len, sizeof(out->config_dir) - len, "/%s",
                 app_id) >= (int)(sizeof(out->config_dir) - len))
        return false;
    len = strlen(out->cache_dir);
    if (snprintf(out->cache_dir + len, sizeof(out->cache_dir) - len, "/%s",
                 app_id) >= (int)(sizeof(out->cache_dir) - len))
        return false;
#ifdef WSAP_DATADIR
    if (!join(WSAP_DATADIR, app_id, out->data_dir, sizeof(out->data_dir)))
        return false;
#endif
    return true;
}

bool app_identity_config_path(const struct app_identity *identity, char *out,
                              size_t sz, const char *rel)
{
    return identity && join(identity->config_dir, rel, out, sz);
}

bool app_identity_cache_path(const struct app_identity *identity, char *out,
                             size_t sz, const char *rel)
{
    return identity && join(identity->cache_dir, rel, out, sz);
}

bool app_identity_data_path(const struct app_identity *identity, char *out,
                            size_t sz, const char *rel)
{
    return identity && join(identity->data_dir, rel, out, sz);
}

void app_identity_set_current(const struct app_identity *identity)
{
    current = identity;
}

const struct app_identity *app_identity_current(void)
{
    return current;
}
