#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/.."

status=0
deep=0

for arg in "$@"; do
	case "$arg" in
		--deep) deep=1 ;;
		--help|-h)
			echo "Usage: ./tools/analyze.sh [--deep]"
			echo "  Default: Fast multi-compiler diagnostics (GCC, Clang) and parallel cppcheck (~3-5s)."
			echo "  --deep:  Also runs Clang scan-build, clang-tidy, and GCC -fanalyzer."
			exit 0
			;;
	esac
done

section()
{
	printf '\n==> %s\n' "$1"
}

run()
{
	if ! "$@"; then
		status=1
	fi
}

available()
{
	if command -v "$1" >/dev/null 2>&1; then
		return 0
	fi
	printf 'warning: %s is not installed; skipping this analysis\n' "$1" >&2
	return 1
}

common_warnings='-Wall -Wextra -Wpedantic -Wformat=2'

build_analysis()
{
	local name=$1 compiler=$2 flags=$3
	local build="build/analysis-$name"
	local reconfigure=()
	[[ -f "$build/meson-private/coredata.dat" ]] && reconfigure=(--reconfigure)
	if ! env CC="$compiler" meson setup "$build" "${reconfigure[@]}" \
		--buildtype=debugoptimized -Doptimization=1 -Db_lto=false \
		"-Dc_args=$flags"; then
		status=1
		return
	fi
	run meson compile -C "$build"
}

section 'GCC compiler diagnostics'
if available gcc; then
	build_analysis gcc gcc "$common_warnings"
fi

section 'Clang compiler diagnostics'
if available clang; then
	build_analysis clang clang "$common_warnings"
fi

section 'Cppcheck (parallel)'
if available cppcheck; then
	threads=$(nproc 2>/dev/null || echo 4)
	run cppcheck -j"$threads" --std=c23 --enable=warning,portability,performance \
		--error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem \
		--suppress=constParameterCallback \
		--suppress=knownConditionTrueFalse \
		--suppress='negativeIndex:subprojects/term/src/escape.c' \
		--suppress='*:src/ui/extern/*' \
		--suppress='assertWithSideEffect:tests/*' \
		--suppress='assertWithSideEffect:subprojects/term/tests/*' \
		--library=gtk \
		-Isrc -Isubprojects/term/include -Isubprojects/term/include/wsap_vtcore \
		-Isubprojects/term/src -Isubprojects/term/tests/helpers \
		src tests benchmarks subprojects/term/src subprojects/term/tests \
		subprojects/term/benchmarks
fi

if ((deep)); then
	section 'clang-tidy path-sensitive checks'
	if available clang-tidy && [[ -f build/analysis-clang/compile_commands.json ]]; then
		while IFS= read -r source; do
			run clang-tidy -p build/analysis-clang "$source" \
				--checks='-*,clang-analyzer-core.*,clang-analyzer-unix.*' \
				--warnings-as-errors='clang-analyzer-core.*,clang-analyzer-unix.*' \
				--header-filter='.*(src|tests)/' --quiet
		done < <(python3 -c 'import json, pathlib; print("\n".join(sorted({str((pathlib.Path(x["directory"]) / x["file"]).resolve()) for x in json.load(open("build/analysis-clang/compile_commands.json"))})))')
	fi

	section 'Clang Static Analyzer (scan-build)'
	if available scan-build && available clang; then
		run ninja -C build/analysis-clang scan-build
	fi

	section 'GCC -fanalyzer'
	if available gcc && gcc --version | head -1 | grep -qi gcc; then
		build_analysis fanalyzer gcc '-Wall -Wextra -fanalyzer -Werror=analyzer-null-dereference -Werror=analyzer-possible-null-dereference -Werror=analyzer-use-after-free -Werror=analyzer-double-free -Werror=analyzer-malloc-leak'
	fi
fi

if ((status)); then
	printf '\nStatic analysis reported findings.\n' >&2
else
	printf '\nStatic analysis completed without findings.\n'
fi
exit "$status"
