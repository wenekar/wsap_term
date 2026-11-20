#ifndef YASL_PRIVATE_COMMON_LOOP_H
#define YASL_PRIVATE_COMMON_LOOP_H

#include "common/private_loop.h"

struct pollfd;

int yasl_private_loop_next_timeout_ms(const yasl_loop *l);
/* The loop's current poll set, valid until the next watch change. A backend
 * whose native wait cannot poll these descriptors mirrors them; Cocoa builds a
 * kqueue from the set. */
int yasl_private_loop_poll_set(const yasl_loop *l, const struct pollfd **fds);

#endif /* YASL_PRIVATE_COMMON_LOOP_H */
