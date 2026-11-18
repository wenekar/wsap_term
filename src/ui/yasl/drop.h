#ifndef UI_YASL_DROP_H
#define UI_YASL_DROP_H

#include <stdbool.h>
#include <stddef.h>

/* The drag and drop helpers of the window layer. A drop carries its type and
 * its data. It never becomes a file. */

/* Returns the best type that the source offers and the target accepts, or
 * NULL when the two lists share no type. Both lists are best first, so the
 * result is the first entry of `accepted` that `offered` holds. An entry of
 * `offered` with a star, such as "image/" "*", matches every type with its
 * major type. */
const char *drop_pick_mime(const char *const *offered, size_t offered_count,
                           const char *const *accepted, size_t accepted_count);

/* Returns true when `mime` names a text type. */
bool drop_mime_is_text(const char *mime);

/* Returns true when `mime` names a URI list. An empty type counts as one,
 * because yasl reports it that way. */
bool drop_mime_is_uri_list(const char *mime);

/* The widget that a drag is over. It sends one leave to a widget that the
 * drag leaves, and none to a widget that it stays on. */
struct drop_hover {
    const void *target;
    /* The widget that the last retarget left, or NULL. It gets one leave. */
    const void *previous;
    bool active;
    /* True after the first event on `target`. The first one is the enter. */
    bool entered;
};

/* Sets the widget that the drag is over. Returns true when the previous
 * widget must get one leave: the drag moved to another widget, or it left the
 * surface. A move within one widget returns false. Call it with `target` NULL
 * when the drag leaves the surface. The call sets `previous` and clears
 * `entered` on a move to another widget. */
bool drop_hover_retarget(struct drop_hover *hover, const void *target);

/* The old file path of a dropped image. A drop writes no file now, so it
 * returns NULL for every payload. Drop it when the native drop path sends a
 * MIME drop. Call it on the loop thread. */
const char *drop_save_image(const char *mime, const char *data, size_t len);

/* Does nothing: a drop writes no file. Call it on the normal exit path for
 * the same reason as before. */
void drop_cleanup(void);

#endif
