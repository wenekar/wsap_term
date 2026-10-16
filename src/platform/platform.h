#ifndef PLATFORM_H
#define PLATFORM_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

#ifdef __linux__

#include <pty.h>

#define PLATFORM_PRELOAD_ENV "LD_PRELOAD"
#define PLATFORM_PRELOAD_LIB "libfastpty.so"
#define PLATFORM_FASTPIPE_DEFAULT 1

#define platform_madv_dontfork(p, n) madvise((p), (n), MADV_DONTFORK)
#define platform_madv_nohugepage(p, n) madvise((p), (n), MADV_NOHUGEPAGE)
#define PLATFORM_CLOCK_COARSE CLOCK_MONOTONIC_COARSE

#else

#include <util.h>

#if defined(__APPLE__)

#define PLATFORM_PRELOAD_ENV "DYLD_INSERT_LIBRARIES"
#define PLATFORM_PRELOAD_LIB "libfastpty.dylib"

#else

#define PLATFORM_PRELOAD_ENV "LD_PRELOAD"
#define PLATFORM_PRELOAD_LIB "libfastpty.so"

#endif

#define PLATFORM_FASTPIPE_DEFAULT 0

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

int platform_fastpipe_create(int fds[2], int buf_size);

int platform_exe_path(char *out, size_t n);

/* Writes the resource directory of the app bundle that holds the
 * executable. Returns -1 when the executable is not in a bundle. */
int platform_resource_dir(char *out, size_t n);

void platform_locale_default(char *out, size_t n);

void platform_close_fds_from(int first);

void platform_thread_interactive(void);

void platform_open_url(const char *url);
void platform_notify(const char *title, const char *body);
void platform_bell(void);

/* True when the desktop asks for high-contrast colors and controls. */
bool platform_prefers_high_contrast(void);

#endif
