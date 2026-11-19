#ifndef TERMINAL_DROP_H
#define TERMINAL_DROP_H

#include <stdbool.h>
#include <stddef.h>

/* Returns true when `mime` names an image type with a subtype. */
bool terminal_mime_is_image(const char *mime);

/* Returns a quoted path that a shell takes as one argument. A single quote
 * becomes '\''. Returns NULL when `path` is empty, has a control character,
 * or is too long. The caller frees the result. */
char *terminal_quote_path(const char *path);

/* Turns a local file uri list into quoted paths separated by spaces. Each
 * line must start with file://. The other lines are skipped. Returns NULL
 * when no line holds a path. The caller frees the result. */
char *terminal_quote_uri_list(const char *data, size_t len);

/* Joins the lines of a uri list with one space. It skips comment lines and
 * blank lines, and it trims the spaces and tabs at the end of a line. Returns
 * NULL when no line holds text. The caller frees the result. */
char *terminal_join_uri_lines(const char *data, size_t len);

/* Writes a dropped image to a new private file and returns its path. The
 * directory lies under XDG_RUNTIME_DIR, or under /tmp when it is unusable.
 * The `mime` argument picks the file extension. Returns NULL when the
 * payload is empty or over 64 MiB, or when the save fails. The caller frees
 * the result. An exit handler removes the file. */
char *terminal_save_dropped_image(const char *mime, const char *data,
                                  size_t len);

#endif
