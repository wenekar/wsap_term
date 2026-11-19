#!/usr/bin/env bash
# The SDK path check must find the paths of this machine in a staged tree.
set -euo pipefail

here=$(cd "$(dirname "$0")/../.." && pwd)
check=$here/tools/ci/source_paths_check.sh
source_root=$(mktemp -d)
build_dir=$(mktemp -d)
stage=$(mktemp -d)
out_dir=$(mktemp -d)
trap 'rm -rf "$source_root" "$build_dir" "$stage" "$out_dir"' EXIT

# A tree with no path of this machine passes.
printf 'plain text\n' > "$stage/clean.txt"
bash "$check" "$source_root" "$build_dir" "$stage"

# A tree that holds the source root fails, and the check names the file.
printf 'built at %s/src/main.c\n' "$source_root" > "$stage/leak.txt"
if bash "$check" "$source_root" "$build_dir" "$stage" > "$out_dir/hits"; then
    echo "FAIL: the check accepted a tree that holds the source root" >&2
    exit 1
fi
grep -qF "$stage/leak.txt" "$out_dir/hits"
rm "$stage/leak.txt"

# A tree that holds the build dir fails too.
printf 'built in %s/src/main.o\n' "$build_dir" > "$stage/build-leak.txt"
if bash "$check" "$source_root" "$build_dir" "$stage" > "$out_dir/hits"; then
    echo "FAIL: the check accepted a tree that holds the build dir" >&2
    exit 1
fi
grep -qF "$stage/build-leak.txt" "$out_dir/hits"

echo "sdk-source-paths-test: ok"
