#ifndef YASL_PRIVATE_WAYLAND_API_H
#define YASL_PRIVATE_WAYLAND_API_H

#include "common/types.h"

struct wayland_display;
struct wayland_surface;

struct wayland_display *wayland_display_open(struct yasl_loop *loop, const struct yasl_private_display_spec *spec);
void wayland_display_close(struct wayland_display *d);
uint32_t wayland_display_caps(const struct wayland_display *d);
bool wayland_display_prepare(struct wayland_display *d);
void wayland_display_check(struct wayland_display *d);
void wayland_display_flush(struct wayland_display *d);
int wayland_display_modifiers(const struct wayland_display *d);
bool wayland_display_copy_text(struct wayland_display *d, enum yasl_private_selection sel, const char *text);
bool wayland_display_request_paste(struct wayland_display *d, enum yasl_private_selection sel,
                                   struct wayland_surface *target, uint64_t request_id);
bool wayland_display_drag_start(struct wayland_display *d, struct wayland_surface *origin, const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *l, void *ud);
void wayland_display_drag_cancel(struct wayland_display *d);
struct wayland_surface *wayland_surface_create_toplevel(struct wayland_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device);
struct wayland_surface *wayland_surface_create_popup(struct wayland_surface *parent, const struct yasl_private_popup_spec *spec);
bool wayland_surface_popup_reposition(struct wayland_surface *popup, const struct yasl_private_popup_spec *spec);
void wayland_surface_destroy(struct wayland_surface *s);
void wayland_surface_set_callbacks(struct wayland_surface *s, const struct yasl_private_window_cbs *cbs, void *ud);
void wayland_surface_set_title(struct wayland_surface *s, const char *title);
void wayland_surface_schedule_redraw(struct wayland_surface *s);
bool wayland_surface_present(struct wayland_surface *s, const struct yasl_private_framebuffer *fb, const struct yasl_private_damage_list *damage);
struct yasl_private_geometry wayland_surface_geometry(const struct wayland_surface *s);
struct yasl_private_timing wayland_surface_timing(const struct wayland_surface *s);
void wayland_surface_set_cursor(struct wayland_surface *s, enum yasl_private_cursor cursor);
bool wayland_surface_set_pointer_mode(struct wayland_surface *s, enum yasl_private_pointer_mode mode);
void wayland_surface_set_background_color(struct wayland_surface *s, uint32_t bg);
bool wayland_surface_ime_preedit_cursor(const struct wayland_surface *s, int *begin, int *end);
void wayland_surface_set_ime_caret(struct wayland_surface *s, struct yasl_private_rect2i caret);
bool wayland_surface_start_interactive_move(struct wayland_surface *s);
bool wayland_surface_start_interactive_resize(struct wayland_surface *s, enum yasl_private_resize_edge edge);
bool wayland_surface_show_window_menu(struct wayland_surface *s, struct yasl_private_vector2i pos);
bool wayland_surface_drag_carry(struct wayland_surface *s, struct yasl_private_vector2i offset);
bool wayland_surface_place_under_pointer(struct wayland_surface *s, struct yasl_private_vector2i at);
bool wayland_surface_bell(struct wayland_surface *s);
bool wayland_surface_read_pixels(struct wayland_surface *s, uint32_t *out, size_t cap,
                                 struct yasl_private_vector2i *size);

#endif
