#ifndef CORE_SIGNALS_H
#define CORE_SIGNALS_H

#include <signal.h>
#include <stdbool.h>

/* Ignores signo and saves the old action. For signal watching,
 * see yasl_loop_watch_signals(). */
bool signal_ignore(int signo, struct sigaction *old);
void signal_restore(int signo, const struct sigaction *old);

#endif
