#!/bin/sh

set -eu

os=$(uname -s)
case "$os" in
	Darwin)
		profiler=sample
		;;
	Linux)
		profiler=perf
		command -v perf >/dev/null 2>&1 || {
			echo "term-live-bench on Linux requires perf(1)" >&2
			exit 1
		}
		;;
	 *)
		echo "term-live-bench supports macOS and Linux" >&2
		exit 1
		;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(dirname -- "$script_dir")
terminal_bin=${1:-$repo_root/build/bin/wsap_term}
profile_tmp=$(mktemp -d)
term_pid=

cleanup()
{
	if [ -n "$term_pid" ]; then
		kill -INT "$term_pid" 2>/dev/null || true
		wait "$term_pid" 2>/dev/null || true
	fi
	rm -rf "$profile_tmp"
}
trap cleanup EXIT INT TERM

mkdir -p "$profile_tmp/fish" "$profile_tmp/wsap_term"
printf '%s\n' 'cat /dev/random' > "$profile_tmp/fish/config.fish"
printf 'cursor-blink = false\ncommand = fish\n' \
	> "$profile_tmp/wsap_term/term.conf"

XDG_CONFIG_HOME="$profile_tmp" \
	"$terminal_bin" --stats-ms 1000 \
	>"$profile_tmp/wsap_term.log" 2>&1 &
term_pid=$!

sleep 1
if ! kill -0 "$term_pid" 2>/dev/null; then
	echo "wsap_term exited before profiling; check the Wayland session" >&2
	cat "$profile_tmp/wsap_term.log" >&2
	exit 1
fi
case "$profiler" in
	sample)
		sample "$term_pid" 2 1 -file "$script_dir/term-live-bench.sample.txt"
		;;
	perf)
		perf_data="$script_dir/term-live-bench.perf.data"
		perf_report="$script_dir/term-live-bench.perf.annotated.txt"
		perf record --quiet -F 1000 --call-graph dwarf -p "$term_pid" \
			-o "$perf_data" -- sleep 2
		perf script -i "$perf_data" > "$script_dir/term-live-bench.perf.txt"
		annotate_sym=term_write
		nm "$terminal_bin" 2>/dev/null | grep -q ' term_write$' ||
			annotate_sym=drain_pty_fd
		{
			echo '=== perf report ==='
			perf report --stdio -i "$perf_data"
			echo
			echo "=== $annotate_sym annotation (including inlined term_write) ==="
			perf annotate --stdio --percent-type global-period -i "$perf_data" "$annotate_sym"
		} > "$perf_report"
		echo "wrote $script_dir/term-live-bench.perf.txt"
		echo "wrote $perf_report"
		;;
esac
