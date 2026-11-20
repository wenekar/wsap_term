#!/usr/bin/env bash
# Build the X11-only app and the CPU-only app.
set -euo pipefail

cd "$(dirname "$0")/../.."

unset LD_PRELOAD DYLD_INSERT_LIBRARIES
export WSAP_NO_PRELOAD=1

meson setup --wipe build/ci-x11 \
    --buildtype=release \
    -Disa= \
    -Db_lto=false \
    -Dwindow_system=x11 \
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
