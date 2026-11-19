#!/usr/bin/env bash
# Build the framework SDK, the X11 and CPU-only configurations, and the
# benchmarks. Then compile and run a pkg-config consumer against the SDK.
set -euo pipefail

cd "$(dirname "$0")/../.."

unset LD_PRELOAD DYLD_INSERT_LIBRARIES

sdk_prefix="$PWD/build/sdk"
sdk_pc="$sdk_prefix/lib/pkgconfig"

meson setup --wipe build/ci-framework \
    --buildtype=release \
    -Disa= \
    -Db_lto=false \
    -Dtests=false \
    -Dbenchmarks=false \
    --prefix="$sdk_prefix"
ninja -C build/ci-framework
meson install -C build/ci-framework --no-rebuild --skip-subprojects

export PKG_CONFIG_PATH="$sdk_pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cc tools/ci/pkgconfig_consumer.c -o build/ci-framework/pkgconfig-consumer \
    $(pkg-config --cflags --libs wsap-ui)
build/ci-framework/pkgconfig-consumer

meson setup --wipe build/ci-x11 \
    --buildtype=release \
    -Disa= \
    -Db_lto=false \
    -Dwindow_backend=x11 \
    -Dvulkan_render=disabled \
    -Dtests=false \
    -Dbenchmarks=false
ninja -C build/ci-x11

meson setup --wipe build/ci-cpu \
    --buildtype=release \
    -Disa= \
    -Db_lto=false \
    -Dvulkan_render=disabled \
    -Dtests=false \
    -Dbenchmarks=false
ninja -C build/ci-cpu

meson setup --wipe build/ci-bench \
    --buildtype=release \
    -Disa= \
    -Db_lto=false \
    -Dtests=false \
    -Dbenchmarks=true
ninja -C build/ci-bench
