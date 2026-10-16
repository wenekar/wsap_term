#include "loop/signals.h"

#include <stddef.h>

bool signal_ignore(int signo, struct sigaction *old)
{
    struct sigaction ignore = { .sa_handler = SIG_IGN };
    sigemptyset(&ignore.sa_mask);
    return sigaction(signo, &ignore, old) == 0;
}

void signal_restore(int signo, const struct sigaction *old)
{
    sigaction(signo, old, NULL);
}
