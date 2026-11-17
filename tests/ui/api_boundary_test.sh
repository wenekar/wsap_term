#!/usr/bin/env bash
set -euo pipefail
cd "${1:-$(dirname "$0")/../..}"

allowed=(
    benchmarks/ui_bench.c
    tests/ui/core_contract_test.c
    tests/ui/debug_test.c
    tests/ui/focus_test.c
    tests/ui/paint_test.c
    tests/paint/scene_test.c
    tests/ui/style_context_test.c
    tests/ui/surface_test.c
    tests/ui/typed_widgets_test.c
    tests/ui/widget_test.c
    tests/ui/widgets/scrollview_test.c
    tests/app/tabs/ui_tree_test.c
)

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

for file in "${allowed[@]}"; do
    if ! grep -qE '(\.|->)impl\b|#include "ui/internal/' "$file"; then
        echo "$file: uses only public UI headers; remove it from the list" >&2
        status=1
    fi
done
exit $status
