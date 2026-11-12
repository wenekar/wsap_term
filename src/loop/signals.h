#ifndef CORE_SIGNALS_H
#define CORE_SIGNALS_H

#include <signal.h>
#include <stdbool.h>

/*
 * Sets the disposition of signal `signo` to SIG_IGN.
 * Saves the previous action in `*old` if `old` is not NULL.
 * Returns true on success, or false on error.
 */
bool signal_ignore(int signo, struct sigaction *old);

/*
 * Restores the signal action for `signo` from `*old`.
 */
void signal_restore(int signo, const struct sigaction *old);

#endif
