#ifndef WSAP_UI_H
#define WSAP_UI_H

/* The screen API. A surface shows one widget tree and owns its focus,
 * input, style, and frames. Widgets report through one action handler
 * type, set on a control or on any ancestor. A new widget type also
 * includes ui/widget_impl.h. */
#include "ui/action.h"
#include "ui/box.h"
#include "ui/describe.h"
#include "ui/event.h"
#include "paint/scene.h"
#include "ui/surface.h"
#include "ui/widget.h"
#include "ui/widgets/button.h"
#include "ui/widgets/dropdown.h"
#include "ui/widgets/input.h"
#include "ui/widgets/label.h"
#include "ui/widgets/list.h"
#include "ui/widgets/menu.h"
#include "ui/widgets/palette.h"
#include "ui/widgets/scrollview.h"
#include "ui/widgets/tab_preview.h"
#include "ui/widgets/tabbar.h"

#endif
