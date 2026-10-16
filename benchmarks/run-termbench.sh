#!/bin/bash

# Usage: run-termbench.sh [size] [test]
# Env: RUNS (default 5) timed runs, WARMUP (default 1) discarded runs.
# The script does not build wsap_term. Build it first.

set -euo pipefail

SIZE="${1:-normal}"
FILTER="${2:-}"
RUNS="${RUNS:-5}"
WARMUP="${WARMUP:-1}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
TERM_BIN="$BUILD_DIR/bin/wsap_term"
BENCH_SRC="$ROOT_DIR/benchmarks/termbench.cpp"
BENCH_BIN="$BUILD_DIR/benchmarks/termbench"

if [ ! -x "$TERM_BIN" ]; then
    echo "Error: $TERM_BIN not found. Build it first." >&2
    exit 1
fi

if [ ! -x "$BENCH_BIN" ] || [ "$BENCH_SRC" -nt "$BENCH_BIN" ]; then
    echo "=== Building termbench ==="
    mkdir -p "$(dirname "$BENCH_BIN")"
    g++ -O3 -march=native "$BENCH_SRC" -I"$ROOT_DIR/benchmarks" -o "$BENCH_BIN"
fi

TMP_CONF=$(mktemp -d)
trap 'rm -rf "$TMP_CONF"' EXIT
REPORT_DIR="$TMP_CONF/reports"
mkdir -p "$REPORT_DIR"

cat << SCRIPT > "$TMP_CONF/run.sh"
sleep 0.5
for i in \$(seq 1 $((WARMUP + RUNS))); do
    TERMBENCH_REPORT="$REPORT_DIR/\$i.txt" taskset -c 5 "$BENCH_BIN" "$SIZE" $FILTER
    sleep 0.5
done
sleep 0.5
SCRIPT
chmod +x "$TMP_CONF/run.sh"

mkdir -p "$TMP_CONF/wsap_term"
cat << CONF > "$TMP_CONF/wsap_term/term.conf"
cursor-blink = false
command = $TMP_CONF/run.sh
CONF

echo "=== Running Live Termbench ($SIZE, $WARMUP warmup + $RUNS runs) ==="
XDG_CONFIG_HOME="$TMP_CONF" taskset -c 4,6 "$TERM_BIN" || true

REPORTS=()
for i in $(seq $((WARMUP + 1)) $((WARMUP + RUNS))); do
    [ -f "$REPORT_DIR/$i.txt" ] && REPORTS+=("$REPORT_DIR/$i.txt")
done
if [ ${#REPORTS[@]} -eq 0 ]; then
    echo "Error: no report was generated." >&2
    exit 1
fi

REPORT_FILE="$BUILD_DIR/benchmarks/termbench-report.txt"
echo "=== TermMark Results (${#REPORTS[@]} runs, median [min-max] gb/s) ==="
grep -h "^CPU:" "${REPORTS[0]}" | tee "$REPORT_FILE"
grep -h "gb/s)" "${REPORTS[@]}" | awk '
    {
        name = $0; sub(/: [0-9.]+s \(.*$/, "", name)
        v = $0; sub(/^.*\(/, "", v); sub(/gb\/s\).*$/, "", v)
        if (!(name in n)) order[++k] = name
        vals[name, ++n[name]] = v + 0
    }
    END {
        for (o = 1; o <= k; o++) {
            name = order[o]; c = n[name]
            for (i = 1; i <= c; i++) a[i] = vals[name, i]
            for (i = 2; i <= c; i++)
                for (j = i; j > 1 && a[j-1] > a[j]; j--) { t = a[j]; a[j] = a[j-1]; a[j-1] = t }
            med = c % 2 ? a[(c+1)/2] : (a[c/2] + a[c/2+1]) / 2
            printf "%s: %.3f [%.3f-%.3f]\n", name, med, a[1], a[c]
        }
    }' | tee -a "$REPORT_FILE"
