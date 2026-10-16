#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "session/session.h"
#include "util/proc.h"

int main(void)
{
    struct session s = { .term = wsap_vtcore_new(NULL) };
    assert(s.term);
    char out[4096];

    const char cwd[] = "\033]7;file:///tmp\033\\";
    wsap_vtcore_feed(s.term, cwd, sizeof(cwd) - 1, 0);
    assert(session_cwd(&s, out, sizeof(out)));
    assert(!strcmp(out, "/tmp"));
    assert(!session_cwd(&s, out, strlen("/tmp")));
    assert(session_cwd(&s, out, strlen("/tmp") + 1));
    assert(!strcmp(out, "/tmp"));

    /* Use the shell directory when OSC 7 names a file. */
    s.pty.pid = getpid();
    const char file[] = "\033]7;file:///dev/null\033\\";
    wsap_vtcore_feed(s.term, file, sizeof(file) - 1, 0);
    char expected[sizeof(out)];
    assert(proc_read_pid_cwd_buf(getpid(), expected, sizeof(expected)));
    assert(session_cwd(&s, out, sizeof(out)));
    assert(!strcmp(out, expected));

    wsap_vtcore_free(s.term);
    return 0;
}
