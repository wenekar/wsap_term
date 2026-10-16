#!/usr/bin/env bash
set -euo pipefail
cd "${1:-$(dirname "$0")/..}"
cc=${CC:-cc}
cxx=${CXX:-c++}

status=0

check() {
    local label=$1 src
    src=$(cat)
    printf '%s\n' "$src" | "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
        -Iinclude -x c -fsyntax-only - || { echo "$label: not valid C11" >&2; status=1; }
    if command -v "$cxx" >/dev/null; then
        printf '%s\n' "$src" | "$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
            -Iinclude -x c++ -fsyntax-only - || { echo "$label: not valid C++" >&2; status=1; }
    fi
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
exit $status
