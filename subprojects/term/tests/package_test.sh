#!/usr/bin/env bash
set -euo pipefail
root=$(cd "${1:-$(dirname "$0")/..}" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
read -r -a compiler <<<"${CC:-cc}"
read -r -a cxx <<<"${CXX:-c++}"

meson setup "$work/build" "$root" -Dtests=false -Dkitty=disabled \
    -Dfuzzing=false -Db_lto=false --prefix "$work/prefix" --libdir lib
meson compile -C "$work/build" -j 2
meson install -C "$work/build"

export PKG_CONFIG_PATH="$work/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_SYSROOT_DIR=
export LD_LIBRARY_PATH="$work/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="$work/prefix/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
pkg-config --exists wsap_vtcore
for header in wsap_vtcore.h wsap_vtcore_input.h wsap_vtcore_types.h \
    unicode/grapheme.h unicode/unicode.h unicode/utf8.h unicode/width.h unicode/unicode_data.inc; do
    test -f "$work/prefix/include/wsap_vtcore/$header"
done
cat > "$work/consumer.c" <<'EOF'
#ifdef WSAP_VTCORE_AMALGAMATION
#include "wsap_vtcore.h"
#else
#include <wsap_vtcore/wsap_vtcore.h>
#endif
int main(void)
{
    struct wsap_vtcore *term = wsap_vtcore_new(NULL);
    struct wsap_vtcore_snapshot *snapshot = wsap_vtcore_snapshot_new();
    int failed = 0;
    if (!term || !snapshot) {
        wsap_vtcore_snapshot_free(snapshot);
        wsap_vtcore_free(term);
        return 1;
    }
    const char input[] = "e\xcc\x81\xe4\xb8\xad";
    if (!wsap_vtcore_feed(term, input, sizeof(input) - 1, 0) ||
        !wsap_vtcore_snapshot_update(snapshot, term)) {
        failed = 1;
    } else {
        const struct wsap_vtcore_cell *row = wsap_vtcore_snapshot_row(snapshot, 0);
        uint32_t cps[4];
        failed = !row || row[0].width != 1 || row[1].width != 2 || row[2].width != 0;
        if (!failed)
            failed = wsap_vtcore_snapshot_cluster(snapshot, row[0].text, cps, 4) != 2 ||
                     cps[0] != 'e' || cps[1] != 0x301;
    }
    wsap_vtcore_snapshot_free(snapshot);
    wsap_vtcore_free(term);
    return failed;
}
EOF
read -r -a shared_flags <<<"$(pkg-config --cflags --libs wsap_vtcore)"
read -r -a include_flags <<<"$(pkg-config --cflags wsap_vtcore)"
"${compiler[@]}" -std=c11 -Wall -Wextra -Wpedantic -Werror "$work/consumer.c" \
    "${shared_flags[@]}" -o "$work/shared-consumer"
"$work/shared-consumer"
read -r -a static_flags <<<"$(pkg-config --static --libs wsap_vtcore)"
consumer_libs=()
for flag in "${static_flags[@]}"; do
    [[ $flag == -lwsap_vtcore ]] || consumer_libs+=("$flag")
done
"${compiler[@]}" -std=c11 -Wall -Wextra -Wpedantic -Werror "${include_flags[@]}" \
    "$work/consumer.c" "$work/prefix/lib/libwsap_vtcore.a" \
    "${consumer_libs[@]}" -o "$work/static-consumer"
"$work/static-consumer"
"${cxx[@]}" -std=c++17 -Wall -Wextra -Wpedantic -Werror -x c++ "$work/consumer.c" \
    "${shared_flags[@]}" -o "$work/cxx-consumer"
"$work/cxx-consumer"

python3 "$root/tools/amalgamate.py" "$root" "$work/amalgamation"
"${compiler[@]}" -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -DWSAP_VTCORE_AMALGAMATION -I"$work/amalgamation" \
    "$work/consumer.c" "$work/amalgamation/wsap_vtcore.c" -pthread -lm \
    -o "$work/amalgamation-consumer"
"$work/amalgamation-consumer"

meson setup "$work/example" "$root/examples"
meson compile -C "$work/example" -j 2
printf 'e\314\201\344\270\255\n' | "$work/example/stdin-snapshot" > "$work/example.out"
python3 - "$work/example.out" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_bytes()
assert b'\0' not in text
assert text.splitlines()[0].rstrip() == 'e\u0301\u4e2d'.encode()
PY
"${compiler[@]}" -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -DWSAP_VTCORE_AMALGAMATION -I"$work/amalgamation" \
    "$root/examples/stdin_snapshot.c" "$work/amalgamation/wsap_vtcore.c" \
    -pthread -lm -o "$work/amalgamation-example"
printf 'e\314\201\344\270\255\n' | "$work/amalgamation-example" > "$work/amalgamation.out"
cmp "$work/example.out" "$work/amalgamation.out"
echo 'installed shared, static, C++, amalgamation, and example consumers passed'
