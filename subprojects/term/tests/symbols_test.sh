#!/usr/bin/env bash
set -euo pipefail
lib=$1
src=${2:-$(dirname "$0")/../include/wsap_vtcore}
nm=${NM:-nm}
read -r -a compiler <<<"${CC:-cc}"
# Ignore symbols that instrumentation adds. ASan adds ODR indicators for each
# global and section bounds for its table: GCC names them __odr_asan.NAME and
# Clang __odr_asan_gen_NAME. Clang coverage adds __covrec_* and __llvm_* names.
instrumented='^_*(odr_asan(_gen)?[._]wsap_vtcore_|asan_globals_registered$|(start|stop)_+(asan_globals$|llvm_prf_)|covrec_|llvm_)'

defined_args=(-g --defined-only -P)
exported_args=(-D --defined-only -P)
if [[ $(uname -s) == Darwin ]]; then
    defined_args=(-g -U -P)
    exported_args=(-g -U -P)
fi
syms=$("$nm" "${defined_args[@]}" "$lib")
defined=$(awk 'NF >= 2 && $2 ~ /^[A-Z]$/ && $2 != "U" { print $1 }' <<<"$syms" |
          { grep -Ev "$instrumented" || true; } | sed 's/^_//' | sort -u)

status=0
bad=$(grep -v '^wsap_vtcore_' <<<"$defined" || true)
if [[ -n $bad ]]; then
    echo "$lib: global symbols without the wsap_vtcore_ prefix:" >&2
    echo "$bad" >&2
    status=1
fi

declared=$("${compiler[@]}" -std=c11 -E -P -I"$src" "$src/wsap_vtcore.h" | python3 -c '
import re, sys
text = re.sub(r"\s+", " ", sys.stdin.read())
depth, flat = 0, []
for ch in text:
    depth += ch == "{"
    if depth == 0:
        flat.append(ch)
    depth -= ch == "}"
    if ch == "}" and depth == 0:
        flat.append(";")
for stmt in "".join(flat).split(";"):
    if "static" in stmt or "typedef" in stmt:
        continue
    m = re.search(r"\b(wsap_vtcore_\w+)\s*\(", stmt)
    if m:
        print(m.group(1))
' | sort -u)
missing=$(comm -23 <(echo "$declared") <(echo "$defined"))
if [[ -n $missing ]]; then
    echo "$lib: functions declared in wsap_vtcore.h but not defined:" >&2
    echo "$missing" >&2
    status=1
fi

shared=${3:-}
if [[ -n $shared ]]; then
    exported=$("$nm" "${exported_args[@]}" "$shared" | awk '$2 ~ /^[TDBRVW]$/ { print $1 }' |
               { grep -Ev "$instrumented" || true; } | sed 's/^_//' | sort -u)
    extra=$(comm -13 <(echo "$declared") <(echo "$exported"))
    absent=$(comm -23 <(echo "$declared") <(echo "$exported"))
    if [[ -n $extra || -n $absent ]]; then
        echo "$shared: exports differ from wsap_vtcore.h" >&2
        [[ -n $extra ]] && echo "not in wsap_vtcore.h: $extra" >&2
        [[ -n $absent ]] && echo "not exported: $absent" >&2
        status=1
    fi
fi
exit $status
