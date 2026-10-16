#ifndef YASL_PRIVATE_WAYLAND_CLIPBOARD_H
#define YASL_PRIVATE_WAYLAND_CLIPBOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "common/types.h"

struct wl_display;
struct wl_data_device_manager;
struct wl_seat;
struct wl_surface;
struct wl_data_device;
struct clipboard;
struct zwp_primary_selection_device_manager_v1;

typedef void (*clipboard_receive_fn)(const char *, size_t, bool uri_list,
                                     struct wl_surface *on, uint64_t request_id,
                                     enum yasl_private_transfer_status, void *);

struct clipboard *clipboard_create(struct wl_display *, struct wl_data_device_manager *,
                                   struct wl_seat *, struct yasl_loop *,
                                   clipboard_receive_fn, void *);
void clipboard_destroy(struct clipboard *);
bool clipboard_set_seat(struct clipboard *, struct wl_seat *);
void clipboard_set_primary_manager(struct clipboard *, struct zwp_primary_selection_device_manager_v1 *);
bool clipboard_copy(struct clipboard *, const char *, uint32_t);
bool clipboard_request_paste(struct clipboard *, enum yasl_private_selection,
                             struct wl_surface *target, uint64_t request_id);
void clipboard_forget_surface(struct clipboard *, struct wl_surface *);
bool clipboard_copy_primary(struct clipboard *, const char *, uint32_t);

typedef void (*clipboard_drag_fn)(enum yasl_private_drag_event, struct wl_surface *on,
                                  double x, double y, void *);
void clipboard_set_drag_handler(struct clipboard *, clipboard_drag_fn, void *);

typedef void (*clipboard_drop_fn)(enum yasl_private_drop_event, struct wl_surface *on,
                                  double x, double y, void *);
void clipboard_set_drop_handler(struct clipboard *, clipboard_drop_fn, void *);
const char *clipboard_drag_mime(void);
struct wl_data_device *clipboard_data_device(const struct clipboard *);

#endif
