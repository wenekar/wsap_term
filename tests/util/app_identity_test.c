#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/app_identity.h"

/* Two apps in one process must not share a config path or a cache path. */
int main(void)
{
    char base[] = "/tmp/app-identity-test-XXXXXX";
    assert(mkdtemp(base));
    char config_home[PATH_MAX], cache_home[PATH_MAX];
    snprintf(config_home, sizeof(config_home), "%s/config", base);
    snprintf(cache_home, sizeof(cache_home), "%s/cache", base);
    assert(setenv("XDG_CONFIG_HOME", config_home, 1) == 0);
    assert(setenv("XDG_CACHE_HOME", cache_home, 1) == 0);

    struct app_identity one, two;
    assert(app_identity_init(&one, "alpha"));
    assert(app_identity_init(&two, "beta"));
    assert(!strcmp(one.id, "alpha"));
    assert(!strcmp(two.id, "beta"));

    char one_config[APP_IDENTITY_PATH_MAX], two_config[APP_IDENTITY_PATH_MAX];
    char one_cache[APP_IDENTITY_PATH_MAX], two_cache[APP_IDENTITY_PATH_MAX];
    assert(app_identity_config_path(&one, one_config, sizeof(one_config),
                                    "input.conf"));
    assert(app_identity_config_path(&two, two_config, sizeof(two_config),
                                    "input.conf"));
    assert(app_identity_cache_path(&one, one_cache, sizeof(one_cache),
                                   "fonts"));
    assert(app_identity_cache_path(&two, two_cache, sizeof(two_cache),
                                   "fonts"));
    assert(strcmp(one_config, two_config));
    assert(strcmp(one_cache, two_cache));

    char expect[PATH_MAX];
    snprintf(expect, sizeof(expect), "%s/alpha/input.conf", config_home);
    assert(!strcmp(one_config, expect));
    snprintf(expect, sizeof(expect), "%s/beta/input.conf", config_home);
    assert(!strcmp(two_config, expect));
    snprintf(expect, sizeof(expect), "%s/alpha/fonts", cache_home);
    assert(!strcmp(one_cache, expect));
    snprintf(expect, sizeof(expect), "%s/beta/fonts", cache_home);
    assert(!strcmp(two_cache, expect));

    /* A short buffer must fail, not truncate the path. */
    char small[8];
    assert(!app_identity_config_path(&one, small, sizeof(small), "input.conf"));

    /* An empty identity names no path and no name. */
    struct app_identity none = {0};
    assert(!app_identity_config_path(&none, one_config, sizeof(one_config),
                                     "input.conf"));
    assert(!app_identity_init(&none, ""));

    app_identity_set_current(&one);
    assert(app_identity_current() == &one);
    app_identity_set_current(&two);
    assert(!strcmp(app_identity_current()->id, "beta"));

    rmdir(base);
    return 0;
}
