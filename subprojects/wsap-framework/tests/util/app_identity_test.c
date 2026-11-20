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
#include <sys/wait.h>
#include <unistd.h>

#include "util/app_identity.h"

#if defined(__linux__)
static void copy_self(const char *dest)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    assert(n > 0);
    self[n] = '\0';
    FILE *in = fopen(self, "rb");
    assert(in);
    FILE *out = fopen(dest, "wb");
    assert(out);
    char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
        assert(fwrite(buf, 1, got, out) == got);
    fclose(in);
    fclose(out);
    assert(chmod(dest, 0700) == 0);
}

/*
 * An installed program finds its data in "<prefix>/share/<app_id>", next to
 * its executable. The test copies this program into a temporary prefix and
 * runs the copy, so the path of the executable is the path of the copy.
 */
static void test_data_dir_follows_the_executable(void)
{
    char base[] = "/tmp/app-identity-data-XXXXXX";
    assert(mkdtemp(base));
    char bin[PATH_MAX], share[PATH_MAX], app[PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/bin", base);
    snprintf(share, sizeof(share), "%s/share/demo", base);
    snprintf(app, sizeof(app), "%s/app-identity-test", bin);
    assert(mkdir(bin, 0700) == 0);
    char share_root[PATH_MAX];
    snprintf(share_root, sizeof(share_root), "%s/share", base);
    assert(mkdir(share_root, 0700) == 0);
    assert(mkdir(share, 0700) == 0);
    copy_self(app);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        char value[PATH_MAX];
        snprintf(value, sizeof(value), "%s", base);
        setenv("WSAP_APP_IDENTITY_PREFIX", value, 1);
        execl(app, app, NULL);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unlink(app);
    rmdir(bin);
    rmdir(share);
    rmdir(share_root);
    rmdir(base);
}
#endif

/* Two apps in one process must not share a config path or a cache path. */
int main(void)
{
#if defined(__linux__)
    const char *prefix = getenv("WSAP_APP_IDENTITY_PREFIX");
    if (prefix && prefix[0]) {
        struct app_identity installed;
        assert(app_identity_init(&installed, "demo"));
        char expect[PATH_MAX];
        snprintf(expect, sizeof(expect), "%s/share/demo", prefix);
        assert(!strcmp(installed.data_dir, expect));
        return 0;
    }
#endif
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
#if defined(__linux__)
    test_data_dir_follows_the_executable();
#endif
    return 0;
}
