#!/bin/bash
# Run benchmarks/run-termbench.sh against a private headless Sway, for
# machines with no graphical session (CI, remote shells, agents).
#
# Absolute numbers are NOT comparable to a real session: pixman software
# rendering and headless frame pacing differ from a GPU-backed compositor.
# Use this only to compare two builds on the same machine, and confirm
# anything that matters on a real session with run-termbench.sh directly.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v sway >/dev/null || { echo "sway is required" >&2; exit 1; }

RUNTIME_DIR=$(mktemp -d)
SWAY_LOG="$RUNTIME_DIR/sway.log"
sway_pid=""
cleanup() {
    [ -n "$sway_pid" ] && kill "$sway_pid" 2>/dev/null || true
    [ -n "$sway_pid" ] && wait "$sway_pid" 2>/dev/null || true
    rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

XDG_RUNTIME_DIR="$RUNTIME_DIR" WLR_BACKENDS=headless \
    WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    sway --unsupported-gpu -c /dev/null >"$SWAY_LOG" 2>&1 &
sway_pid=$!

socket=""
for _ in $(seq 100); do
    for candidate in "$RUNTIME_DIR"/wayland-*; do
        [ -S "$candidate" ] && socket="$candidate" && break
    done
    [ -n "$socket" ] && break
    kill -0 "$sway_pid" 2>/dev/null || { cat "$SWAY_LOG" >&2; exit 1; }
    sleep 0.05
done
[ -n "$socket" ] || { echo "headless sway did not start:" >&2; cat "$SWAY_LOG" >&2; exit 1; }

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export WAYLAND_DISPLAY="$(basename "$socket")"
exec "$ROOT_DIR/benchmarks/run-termbench.sh" "$@"
