#!/usr/bin/env bash
set -euo pipefail
root=${1:-$(dirname "$0")/..}
root=$(cd "$root" && pwd)
cd "$root"
command -v zig >/dev/null || { echo "zig not found; skipping"; exit 77; }

# Source list from src/meson.build and src/osc/meson.build.
# Keep this synchronized with those files.
sources=(
    src/core.c
    src/csi/fast.c
    src/escape.c
    src/hist/view.c
    src/input.c
    src/isa.c
    src/marks.c
    src/palette.c
    src/rect.c
    src/scan.c
    src/screen.c
    src/selection.c
    src/snapshot.c
    src/style.c
    src/term/state.c
    src/text/charset.c
    src/text/run.c
    src/text/text.c
    src/osc/clipboard.c
    src/osc/dispatch.c
    src/osc/link.c
    src/osc/misc.c
    src/osc/shell.c
)

# Drift guard: the list above leaves out the three sources that
# src/term/state.c includes: gstore.c, hist/hist.c, and term/term.c.
state_includes=3
found=$(find src -name '*.c' -type f | wc -l | tr -d ' ')
listed=$(( ${#sources[@]} + state_includes ))
if [ "$found" -ne "$listed" ]; then
    echo "cross_test.sh: src/ holds $found .c files and the list covers $listed; add the new file to the list" >&2
    exit 1
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 "$root/tools/amalgamate.py" "$root" "$out/amalgamation"
status=0

for target in aarch64-linux-gnu x86_64-macos aarch64-macos x86_64-windows-gnu; do
    mkdir -p "$out/$target"
    objs=()
    for src in "${sources[@]}"; do
        obj="$out/$target/$(basename "$src" .c).o"
        zig cc -target "$target" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
            -Isrc -Iinclude -Iinclude/wsap_vtcore -c "$src" -o "$obj" ||
            { echo "$target: $src fails" >&2; status=1; continue; }
        objs+=("$obj")
    done
    # Link check: build one executable from all objects.
    if [ ${#objs[@]} -eq ${#sources[@]} ]; then
        echo 'int main(void) { return 0; }' > "$out/$target/main.c"
        zig cc -target "$target" -std=c11 -c "$out/$target/main.c" -o "$out/$target/main.o" ||
            { echo "$target: main.c fails" >&2; status=1; continue; }
        case "$target" in
            *-windows-*) link_libs=() ;;
            *) link_libs=(-lm) ;;
        esac
        zig cc -target "$target" "${objs[@]}" "$out/$target/main.o" ${link_libs[@]+"${link_libs[@]}"} -o "$out/$target/test.exe" ||
            { echo "$target: link fails" >&2; status=1; }
    fi
    zig cc -target "$target" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
        -c "$out/amalgamation/wsap_vtcore.c" -o "$out/$target/amalgam.o" ||
        { echo "$target: the amalgamation fails" >&2; status=1; }
done
exit $status
