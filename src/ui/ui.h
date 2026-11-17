#ifndef WSAP_UI_H
#define WSAP_UI_H

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
#include "ui/widgets/scrollview.h"

/* Releases global UI caches including the default stylesheet and interned parts. */
void ui_shutdown(void);

#endif
