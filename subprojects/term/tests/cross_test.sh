#!/usr/bin/env bash
set -euo pipefail
root=${1:-$(dirname "$0")/..}
cd "$root/src"
command -v zig >/dev/null || { echo "zig not found; skipping"; exit 77; }

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 "$root/tools/amalgamate.py" "$root" "$out/amalgamation"
status=0
for target in aarch64-linux-gnu x86_64-macos aarch64-macos x86_64-windows-gnu; do
    for src in *.c osc/*.c; do
        zig cc -target "$target" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
            -I. -I../include/wsap_vtcore -c "$src" -o "$out/obj.o" || { echo "$target: $src fails" >&2; status=1; }
    done
    zig cc -target "$target" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror \
        -c "$out/amalgamation/wsap_vtcore.c" -o "$out/obj.o" ||
        { echo "$target: the amalgamation fails" >&2; status=1; }
done
exit $status
