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

#include "util/proc.h"

/* A working directory that does not fit is rejected, not cut short to
 * the name of a parent directory. */
int main(void)
{
    char parent[] = "/tmp/proc-test-XXXXXX";
    assert(mkdtemp(parent));
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/nested", parent);
    assert(mkdir(child, 0700) == 0);
    assert(chdir(child) == 0);
    char real_child[PATH_MAX];
    assert(getcwd(real_child, sizeof(real_child)));

    char out[PATH_MAX];
    assert(proc_read_pid_cwd_buf(getpid(), out, sizeof(out)));
    assert(!strcmp(out, real_child));

    size_t parent_len = strlen(real_child) - strlen("/nested");
    assert(!proc_read_pid_cwd_buf(getpid(), out, parent_len + 1));
    assert(!proc_read_pid_cwd_buf(getpid(), out, strlen(real_child)));
    assert(proc_read_pid_cwd_buf(getpid(), out, strlen(real_child) + 1));
    assert(!strcmp(out, real_child));

    assert(chdir("/") == 0);
    rmdir(child);
    rmdir(parent);
    return 0;
}
