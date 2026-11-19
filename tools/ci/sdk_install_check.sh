#!/usr/bin/env bash
# Checks the installed framework SDK for relocatability.
#
# The checks:
# 1. No absolute source path of this machine is in the installed tree.
# 2. No installed header declares a terminal-only call.
# 3. A consumer of the installed SDK builds, links, and runs, with the C
#    compiler of this machine and, when it is there, with clang.
#
# Usage: sdk_install_check.sh BUILD_DIR [STAGE_DIR]
# The build must be a framework-only build:
#   meson setup build-sdk -Dapp=false -Dframework=true -Dtests=false \
#       -Dbenchmarks=false
set -euo pipefail

build_dir=${1:?usage: sdk_install_check.sh BUILD_DIR [STAGE_DIR]}
stage=${2:-$(mktemp -d)}
source_root=$(cd "$(dirname "$0")/../.." && pwd)

status=0

DESTDIR="$stage" meson install -C "$build_dir" > /dev/null

pc_file=$(find "$stage" -name 'wsap-render.pc' -print -quit)
if [ -z "$pc_file" ]; then
    echo "no wsap-render.pc in the staging prefix" >&2
    exit 1
fi
pkg_dir=$(dirname "$pc_file")
prefix=$(dirname "$(dirname "$pkg_dir")")

echo "staging prefix: $prefix"

# 1. Absolute source paths. A release build maps the source root to ".".
if grep -rl "/mnt/" "$stage" --exclude='libaccesskit*' > /tmp/sdk_mnt_hits.$$ 2>/dev/null; then
    echo "the installed tree holds absolute source paths:" >&2
    cat /tmp/sdk_mnt_hits.$$ >&2
    status=1
else
    echo "pass: the installed tree holds no /mnt path"
fi
rm -f /tmp/sdk_mnt_hits.$$

# 2. Terminal-only calls. The terminal layer defines them, not the SDK.
terminal_calls='^(struct|enum|bool|void|int|uint32_t) .*(render_draw|render_cursor_cell|render_gpu_draw)\('
if grep -rn "$terminal_calls" "$prefix/include" > /dev/null 2>&1; then
    echo "the installed headers declare a terminal-only call:" >&2
    grep -rn "$terminal_calls" "$prefix/include" >&2
    status=1
else
    echo "pass: the installed headers declare no terminal-only call"
fi

# 3. A real consumer. Clang cannot read the GCC LTO bytecode, so its link
#       proves that the installed archives hold machine code.
export PKG_CONFIG_PATH="$pkg_dir"
export PKG_CONFIG_SYSROOT_DIR="$stage"
out_dir=$(mktemp -d)
for compiler_name in cc clang; do
    compiler=$(command -v "$compiler_name" || true)
    if [ -z "$compiler" ]; then
        echo "skip: $compiler_name is not here"
        continue
    fi
    # shellcheck disable=SC2046
    if ! "$compiler" -std=c17 -o "$out_dir/sdk-consumer-$compiler_name" \
        "$source_root/tools/ci/sdk_consumer.c" \
        $(pkg-config --static --cflags wsap-render wsap-ui) \
        $(pkg-config --static --libs wsap-render wsap-ui); then
        echo "fail: $compiler_name did not link the consumer" >&2
        status=1
        continue
    fi
    if "$out_dir/sdk-consumer-$compiler_name"; then
        echo "pass: the $compiler_name consumer ran"
    else
        echo "fail: the $compiler_name consumer failed at run time ($?)" >&2
        status=1
    fi
done
rm -rf "$out_dir"

exit "$status"
