#!/usr/bin/env bash
set -euo pipefail
cd "${1:-$(dirname "$0")/..}"
if (( $# > 1 )); then
    cc=("${@:2}")
else
    read -r -a cc <<<"${CC:-cc}"
fi
read -r -a cxx <<<"${CXX:-c++}"
have_cxx=true
command -v "${cxx[0]}" >/dev/null || have_cxx=false

status=0

check() {
    local label=$1 src
    src=$(cat)
    printf '%s\n' "$src" | "${cc[@]}" -std=c11 -Wall -Wextra -Wpedantic -Werror \
        -Iinclude -x c -fsyntax-only - || { echo "$label: not valid C11" >&2; status=1; }
    $have_cxx || return 0
    printf '%s\n' "$src" | "${cxx[@]}" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
        -Iinclude -x c++ -fsyntax-only - || { echo "$label: not valid C++" >&2; status=1; }
}

check wsap_vtcore.h <<'EOF'
#include <wsap_vtcore/wsap_vtcore.h>
EOF

check "unicode/width.h: conflicts with host types" <<'EOF'
typedef struct wide { int value; } wide;
#include <wsap_vtcore/unicode/width.h>
EOF

check "unicode API: host names declared first" <<'EOF'
int unicode_width(int);
static int in_ranges;
static int zero_width;
static int utf8d;
struct unicode_range { int lo; };
#define UTF8_ACCEPT 7
#define GRAPHEME_MAX_LEN 3
#define IN_TABLE 1
#define UNICODE_DATA_VERSION "host"
#define UTF_8_H
#include <wsap_vtcore/unicode/utf8.h>
#include <wsap_vtcore/unicode/width.h>
#include <wsap_vtcore/unicode/grapheme.h>
int host_use(void)
{
    return in_ranges + zero_width + utf8d + unicode_width(1) + UTF8_ACCEPT + GRAPHEME_MAX_LEN;
}
EOF

check "unicode API: host names declared after" <<'EOF'
#include <wsap_vtcore/unicode/utf8.h>
#include <wsap_vtcore/unicode/width.h>
#include <wsap_vtcore/unicode/grapheme.h>
int unicode_width(int);
static int in_ranges;
#define UTF8_ACCEPT 7
#define GRAPHEME_MAX_LEN 3
int host_use(void)
{
    return in_ranges + unicode_width(1) + UTF8_ACCEPT + GRAPHEME_MAX_LEN;
}
EOF
(( status == 0 )) || exit 1
# Meson reports exit 77 as a skip. Do not pass without the C++ check.
$have_cxx || { echo "no C++ compiler; set CXX to check the headers as C++" >&2; exit 77; }
