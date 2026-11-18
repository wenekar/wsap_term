#ifndef PLATFORM_H
#define PLATFORM_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#ifdef __linux__

#define platform_madv_dontfork(p, n) madvise((p), (n), MADV_DONTFORK)
#define platform_madv_nohugepage(p, n) madvise((p), (n), MADV_NOHUGEPAGE)
#define PLATFORM_CLOCK_COARSE CLOCK_MONOTONIC_COARSE

#else

#define platform_madv_dontfork(p, n) ((void)(p), (void)(n))
#define platform_madv_nohugepage(p, n) ((void)(p), (void)(n))
#define PLATFORM_CLOCK_COARSE CLOCK_MONOTONIC

#endif

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x1000
#endif
#endif

int platform_eventfd(int *wr_fd);

int platform_exe_path(char *out, size_t n);

int platform_resource_dir(char *out, size_t n);

void platform_locale_default(char *out, size_t n);
bool platform_locale_valid(const char *loc);
char **platform_locales_list(size_t *count);

void platform_close_fds_from(int first);

/* Make room in the descriptor table of the process for the descriptors that
 * the process opens later. Call this before the first thread starts. */
void platform_reserve_fds(void);

void platform_thread_interactive(void);

void platform_open_url(const char *url);
void platform_notify(const char *title, const char *body);
void platform_bell(void);

bool platform_prefers_high_contrast(void);

#endif
