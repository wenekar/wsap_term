#!/bin/sh
set -eu

self_dir=$(cd "$(dirname "$0")" && pwd -P)
src=${WSAP_TERMINFO_SRC:-$self_dir/../share/wsap_term/terminfo/wsap_term.terminfo}
dest=${WSAP_TERMINFO_DEST:-/usr/share/terminfo}
stale=${WSAP_TERMINFO_STALE-/usr/local/share/terminfo}

if [ ! -r "$src" ]; then
    echo "install-terminfo.sh: terminfo source not found: $src" >&2
    exit 1
fi
if ! command -v tic >/dev/null 2>&1; then
    echo "install-terminfo.sh: tic not found; install ncurses first" >&2
    exit 1
fi
if ! mkdir -p "$dest" 2>/dev/null || [ ! -w "$dest" ]; then
    echo "install-terminfo.sh: cannot write to $dest; run this script with sudo" >&2
    exit 1
fi

tic -x -o "$dest" "$src"
echo "Installed wsap_term terminfo to $dest"

if [ -n "$stale" ] && [ "$stale" != "$dest" ] && [ -d "$stale" ]; then
    names=$(awk -F'[|,]' '/^[^# \t]/ { for (i = 1; i < NF; i++) if ($i !~ / /) print $i }' "$src")
    for name in $names; do
        letter=$(printf '%s' "$name" | cut -c1)
        rm -f "$stale/$letter/$name"
    done
    rm -f "$stale/$(basename "$src")"
    echo "Removed old wsap_term terminfo from $stale"
fi
