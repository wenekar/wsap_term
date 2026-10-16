#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

if [[ "$(uname -s)" == Darwin ]]; then
    llvm_bin="$(brew --prefix llvm)/bin"
    export PATH="$llvm_bin:$PATH"
    export CC="$llvm_bin/clang"
    export CXX="$llvm_bin/clang++"
    export OBJC="$llvm_bin/clang"
    export AR="$llvm_bin/llvm-ar"
    export NM="$llvm_bin/llvm-nm"
    export MACOSX_DEPLOYMENT_TARGET=14.0
fi

unset LD_PRELOAD DYLD_INSERT_LIBRARIES
export WSAP_NO_PRELOAD=1

meson setup --wipe build/ci \
    --buildtype=release \
    -Disa= \
    -Db_lto=true \
    -Dtests=true \
    -Dbenchmarks=false \
    -Dintegration_tests=false
python3 tools/dev/dirgraph.py build
meson compile -C build/ci
meson test -C build/ci --print-errorlogs
