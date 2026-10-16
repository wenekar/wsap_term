#ifndef WSAP_UI_STYLE_CONTEXT_H
#define WSAP_UI_STYLE_CONTEXT_H

#include "ui/css.h"
#include <stdbool.h>
#include <stdint.h>

struct ui_widget;

struct ui_style_context {
    const struct css_sheet *sheet;
    const struct css_sheet *host_sheet;
    struct css_sheet *owned_sheet;
    uint64_t generation;
    struct css_scale_context scale;
    uint32_t pad;
};

/* Styling contract.
 *
 * Precedence, lowest to highest: the built-in default sheet
 * (css_init_default), then the host sheet. Rules from a higher tier
 * override lower tiers at equal specificity; specificity (id > class >
 * tag, then compound-class rank) orders rules within and across tiers.
 *
 * The context carries a generation counter that widgets record in
 * style_generation when styled. ui_style_context_apply() skips
 * re-resolving a widget whose style_generation already matches the
 * current generation (it still recurses so newly added descendants get
 * styled). set_sheet() and set_scale() both bump the generation, so
 * either one forces a full re-resolve on the next apply. set_sheet()
 * compares by content-effect, not pointer: calling it with the same
 * sheet pointer still rebuilds, so a sheet mutated or reparsed in place
 * takes effect when it is set again. */
/* Call once for fresh storage. Destroy before initializing it again. */
void ui_style_context_init(struct ui_style_context *context);
void ui_style_context_destroy(struct ui_style_context *context);
/* Borrows sheet until the next set_sheet call or context destruction. */
void ui_style_context_set_sheet(struct ui_style_context *context,
                                const struct css_sheet *sheet);
/* Sets the scale used to resolve px/em/rem/ch style values and copied
 * into each widget's style_scale at apply time. Non-positive components
 * keep the previous value; bumps the generation only when the effective
 * scale changes. */
void ui_style_context_set_scale(struct ui_style_context *context,
                                struct css_scale_context scale);
const struct css_sheet *ui_style_context_sheet(
    const struct ui_style_context *context);
struct css_style ui_style_context_style(const struct ui_style_context *context,
                                        const char *class_name,
                                        unsigned pseudo_flags);
uint64_t ui_style_context_generation(const struct ui_style_context *context);

/* A part handle names a part class such as "tab active". Zero means no
 * part. Handles never change and never repeat for the process lifetime,
 * so one handle works on every surface. Call from the UI thread only. */
typedef uint32_t ui_part_id;
/* Returns the handle for name, and interns name on first use. Returns 0
 * for NULL or an empty name. Never returns 0 for a valid name. When
 * memory runs out, prints a message and aborts, in every build. The
 * handle has no error value besides 0, so a failure cannot be reported
 * to the caller. */
ui_part_id ui_part_intern(const char *name);
/* Returns the interned text, or NULL for 0. A handle that ui_part_intern()
 * never returned is a programming error. The call prints a message and
 * aborts, in every build. */
const char *ui_part_name(ui_part_id part);

/* Resolves each widget's style from the merged sheet (skipping subtrees
 * already valid at the current generation), copies scale into
 * style_scale, stamps style_generation/style_valid, inherits text color
 * down the tree, and multiplies opacity by the parent's. */
void ui_style_context_apply(struct ui_style_context *context,
                            struct ui_widget *root);

const struct css_style *ui_widget_resolved_style(const struct ui_widget *widget);

#endif
