#!/usr/bin/env bash
# Prints the files in a staged tree that hold an absolute path to the source
# root or the build directory.
#
# Usage: source_paths_check.sh SOURCE_ROOT BUILD_DIR STAGE
# Exit status: 0 when the tree holds no such path, 1 when it holds one.
# A release build maps the source root to ".", so a hit is a leaked path.
set -euo pipefail

source_root=$(cd "$1" && pwd -P)
build_dir=$(cd "$2" && pwd -P)
stage=$3

hits=$(
    for root in "$source_root" "$build_dir"; do
        grep -rlF "$root" "$stage" --exclude='libaccesskit*' 2>/dev/null || true
    done | sort -u
)

if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    exit 1
fi
exit 0
