#ifndef YASL_PRIVATE_COCOA_API_H
#define YASL_PRIVATE_COCOA_API_H

#include "common/types.h"

struct cocoa_display;
struct cocoa_surface;

struct cocoa_display *cocoa_display_open(struct yasl_loop *loop, const struct yasl_private_display_spec *spec);
void cocoa_display_close(struct cocoa_display *d);
uint32_t cocoa_display_caps(const struct cocoa_display *d);
bool cocoa_display_prepare(struct cocoa_display *d);
int cocoa_display_wait(struct cocoa_display *d, int timeout_ms);
void cocoa_display_flush(struct cocoa_display *d);
int cocoa_display_modifiers(const struct cocoa_display *d);
bool cocoa_display_drag_start(struct cocoa_display *d, struct cocoa_surface *origin, const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *l, void *ud);
struct cocoa_surface *cocoa_surface_create_toplevel(struct cocoa_display *d,
        const struct yasl_private_window_spec *spec, void *graphics_device);
struct cocoa_surface *cocoa_surface_create_popup(struct cocoa_surface *parent, const struct yasl_private_popup_spec *spec);
bool cocoa_surface_popup_reposition(struct cocoa_surface *popup, const struct yasl_private_popup_spec *spec);
void cocoa_surface_destroy(struct cocoa_surface *s);
void cocoa_surface_set_callbacks(struct cocoa_surface *s, const struct yasl_private_window_cbs *cbs, void *ud);
void cocoa_surface_set_title(struct cocoa_surface *s, const char *title);
void cocoa_surface_schedule_redraw(struct cocoa_surface *s);
bool cocoa_surface_present(struct cocoa_surface *s, const struct yasl_private_framebuffer *fb, const struct yasl_private_damage_list *damage);
struct yasl_private_geometry cocoa_surface_geometry(const struct cocoa_surface *s);
struct yasl_private_timing cocoa_surface_timing(const struct cocoa_surface *s);
void cocoa_surface_set_cursor(struct cocoa_surface *s, enum yasl_private_cursor cursor);
void cocoa_surface_set_background_color(struct cocoa_surface *s, uint32_t bg);
bool cocoa_surface_ime_preedit_cursor(const struct cocoa_surface *s, int *begin, int *end);
void cocoa_surface_set_ime_caret(struct cocoa_surface *s, struct yasl_private_rect2i caret);
bool cocoa_surface_start_interactive_move(struct cocoa_surface *s);
bool cocoa_surface_start_interactive_resize(struct cocoa_surface *s, enum yasl_private_resize_edge edge);
bool cocoa_surface_show_window_menu(struct cocoa_surface *s, struct yasl_private_vector2i pos);
bool cocoa_surface_drag_carry(struct cocoa_surface *s, struct yasl_private_vector2i offset);
bool cocoa_surface_place_under_pointer(struct cocoa_surface *s, struct yasl_private_vector2i at);

#endif
