#!/usr/bin/env bash
set -euo pipefail
cd "${1:-$(dirname "$0")/../..}"

# Files outside the UI core that may use UI internals. Add a reason with each
# entry: the public API must not state the same behavior. The list is empty.
allowed=()

is_allowed() {
    local f
    for f in "${allowed[@]}"; do
        [[ $1 == "$f" ]] && return 0
    done
    return 1
}

# The tree owners read and write the impl of a widget and of a surface. Every
# other file in the UI core uses the public accessors only.
tree_owners=(
    src/ui/accessibility.c
    src/ui/debug.c
    src/ui/describe.c
    src/ui/layout.c
    src/ui/paint.c
    src/ui/style_context.c
    src/ui/surface.c
    src/ui/widget.c
)

is_tree_owner() {
    local f
    for f in "${tree_owners[@]}"; do
        [[ $1 == "$f" ]] && return 0
    done
    return 1
}

status=0
if ! python3 tools/dev/dirgraph.py . --private-owner ui; then
    status=1
fi
while IFS= read -r file; do
    if is_allowed "$file"; then
        continue
    fi
    if grep -nE '(\.|->)impl\b|#include "ui/internal/' "$file"; then
        echo "$file: uses UI internals" >&2
        status=1
    fi
done < <({ find src tests benchmarks -name '*.[chm]' -not -path 'src/ui/*'; find src/ui/yasl -name '*.[chm]'; } | sort)

while IFS= read -r file; do
    is_tree_owner "$file" && continue
    if grep -nE '(\.|->)impl\b' "$file"; then
        echo "$file: reads the widget tree impl" >&2
        status=1
    fi
done < <(find src/ui -name '*.[chm]' -not -path 'src/ui/yasl/*' | sort)

# A widget uses the widget author API in ui/widget_ops.h, as a widget outside
# the UI core does.
while IFS= read -r file; do
    if grep -n '#include "ui/internal/' "$file"; then
        echo "$file: a widget includes a UI core internal header" >&2
        status=1
    fi
done < <(find src/ui/widgets -name '*.[chm]' | sort)

# The editor library sits above base and below ui. It has no window system,
# terminal core, or UI core dependency.
while IFS= read -r file; do
    if grep -nE '#include [<"](yasl/|ui/)|wsap_vtcore' "$file"; then
        echo "$file: the editor library uses a layer above it" >&2
        status=1
    fi
done < <(find src/editor -name '*.[chm]' | sort)

# The runtime library sits above render and ui_yasl. The lower layers may not
# use it. It uses the window UI API and the window host, not their internals.
while IFS= read -r file; do
    if grep -n '#include "runtime/' "$file"; then
        echo "$file: a lower library uses the app runtime" >&2
        status=1
    fi
done < <({ find src/ui src/render -name '*.[chm]'; } | sort)

# The render library draws a grid through render/grid.h. The terminal grid
# renderer sits above it and owns the terminal frame and the VT cells. The
# forwarder at src/render/term_frame.h is part of the terminal grid renderer.
while IFS= read -r file; do
    if grep -nE '#include [<"](wsap_vtcore|terminal/)' "$file"; then
        echo "$file: the render library uses the terminal" >&2
        status=1
    fi
done < <(find src/render -name '*.[chm]' ! -path 'src/render/term_frame.h' | sort)

for file in "${allowed[@]}"; do
    if ! grep -qE '(\.|->)impl\b|#include "ui/internal/' "$file"; then
        echo "$file: uses only public UI headers; remove it from the list" >&2
        status=1
    fi
done
exit $status
