/* Standard user folders, file stat, directory listing, and asset loading. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "yasl/yasl.h"

/*
 * Tests of the standard paths and file helpers (common/controls.c).
 *
 * The test uses a temporary directory, so it needs no fixture in the tree.
 */

static void test_user_dirs(void)
{
    char home[4096];
    size_t len = yasl_get_user_dir(YASL_DIR_HOME, home, sizeof(home));
    assert(len > 0);
    assert(home[0] == '/');

    /* A short buffer reports the full length and writes nothing. */
    char small[2] = {'x', 'x'};
    assert(yasl_get_user_dir(YASL_DIR_HOME, small, sizeof(small)) == len);
    assert(small[0] == 'x' && small[1] == 'x');

    /* Every user directory is absolute. */
    const enum yasl_user_dir dirs[] = {YASL_DIR_PREFERENCE, YASL_DIR_DATA, YASL_DIR_CACHE,
                                       YASL_DIR_DOCUMENTS};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[4096];
        assert(yasl_get_user_dir(dirs[i], path, sizeof(path)) > 0);
        assert(path[0] == '/');
    }

    errno = 0;
    assert(yasl_get_user_dir((enum yasl_user_dir)99, home, sizeof(home)) == 0);
    assert(errno == EINVAL);
}

static void test_stat_and_listing(void)
{
    char root[] = "/tmp/yasl-files-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char file[4096], sub[4096];
    snprintf(file, sizeof(file), "%s/one.txt", root);
    snprintf(sub, sizeof(sub), "%s/sub", root);
    FILE *f = fopen(file, "wb");
    assert(f != NULL);
    assert(fwrite("hello", 1, 5, f) == 5);
    fclose(f);
    assert(mkdir(sub, 0700) == 0);

    struct yasl_file_info info;
    assert(yasl_file_stat(file, &info) == 0);
    assert(info.is_file && !info.is_dir);
    assert(info.size == 5);
    assert(yasl_file_stat(sub, &info) == 0);
    assert(info.is_dir && !info.is_file);

    errno = 0;
    assert(yasl_file_stat("/nonexistent/yasl/path", &info) < 0);
    assert(errno == ENOENT);

    yasl_dir *dir = yasl_dir_open(root);
    assert(dir != NULL);
    int files = 0, dirs = 0;
    struct yasl_dir_entry entry;
    int status;
    while ((status = yasl_dir_next(dir, &entry)) == 1) {
        assert(entry.name[0] != '.');
        if (entry.info.is_file) files++;
        if (entry.info.is_dir) dirs++;
    }
    assert(status == 0);
    assert(files == 1);
    assert(dirs == 1);
    yasl_dir_close(dir);

    yasl_dir_close(NULL);
    errno = 0;
    assert(yasl_dir_open("/nonexistent/yasl/path") == NULL);
    unlink(file);
    rmdir(sub);
    rmdir(root);
}

static void test_load_asset(void)
{
    /* The working directory is the last place that yasl_load_asset looks. */
    char root[] = "/tmp/yasl-asset-XXXXXX";
    assert(mkdtemp(root) != NULL);
    char previous[4096];
    assert(getcwd(previous, sizeof(previous)) != NULL);
    assert(chdir(root) == 0);
    FILE *f = fopen("asset.bin", "wb");
    assert(f != NULL);
    assert(fwrite("abc", 1, 3, f) == 3);
    fclose(f);

    void *data = NULL;
    size_t size = 0;
    assert(yasl_load_asset("asset.bin", &data, &size) == 0);
    assert(size == 3);
    const char *bytes = data;
    assert(bytes[0] == 'a' && bytes[2] == 'c');
    /* The buffer has a NUL after the data. */
    assert(bytes[3] == '\0');
    free(data);

    errno = 0;
    assert(yasl_load_asset("missing.bin", &data, &size) < 0);
    assert(errno == ENOENT);

    assert(chdir(previous) == 0);
    char path[4096];
    snprintf(path, sizeof(path), "%s/asset.bin", root);
    unlink(path);
    rmdir(root);
}

int main(void)
{
    test_user_dirs();
    test_stat_and_listing();
    test_load_asset();
    return 0;
}
