#!/bin/bash
# Drags tabs between windows on a headless Sway with a virtual pointer,
# and checks where they land through the UI inspector. Sway carries no
# windows, so a tab dropped off every bar opens its window on release.

set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="${BIN:-$ROOT/build/bin/wsap_term}"
VPTR="${VPTR:-$ROOT/build/tests/virtual-pointer}"

command -v sway >/dev/null || { echo "SKIP: sway is not installed"; exit 77; }
command -v python3 >/dev/null || { echo "SKIP: python3 is not installed"; exit 77; }

TMP="$(mktemp -d)"
# A private runtime directory keeps the test's Sway out of the session.
export XDG_RUNTIME_DIR="$TMP"
unset WAYLAND_DISPLAY SWAYSOCK LD_PRELOAD
SWAY_PID=""
PI_PID=""
VP_PID=""

cleanup() {
	[[ -n "${VP_PID:-}" ]] && kill -TERM "$VP_PID" 2>/dev/null
	[[ -n "${PI_PID:-}" ]] && kill -TERM "$PI_PID" 2>/dev/null
	[[ -n "${SWAY_PID:-}" ]] && kill -TERM "$SWAY_PID" 2>/dev/null
	wait 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $1"
	echo "--- windows ---"
	dbg windows 2>&1 | head -40
	echo "--- pi log ---"
	cat "$TMP/pi.log"
	exit 1
}

cat >"$TMP/sway.conf" <<'EOF'
output * resolution 1600x1000 position 0 0
default_border none
default_floating_border none
for_window [app_id=".*"] floating enable
focus_follows_mouse no
EOF

BEFORE="$(ls "$XDG_RUNTIME_DIR" | sort)"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
	sway --unsupported-gpu -c "$TMP/sway.conf" >"$TMP/sway.log" 2>&1 &
SWAY_PID=$!
SOCK=""
for _ in $(seq 1 50); do
	SOCK="$(ls "$XDG_RUNTIME_DIR" | sort | comm -13 <(printf '%s\n' "$BEFORE") \
		- | grep -oE '^wayland-[0-9]+$' | tail -1)"
	[[ -n "$SOCK" ]] && break
	sleep 0.1
done
[[ -n "$SOCK" ]] || fail "headless sway did not start"
export WAYLAND_DISPLAY="$SOCK"
export SWAYSOCK="$XDG_RUNTIME_DIR/sway-ipc.$(id -u).$SWAY_PID.sock"
for _ in $(seq 1 50); do [[ -S "$SWAYSOCK" ]] && break; sleep 0.1; done

coproc VP { exec "$VPTR" 1600 1000 2>"$TMP/vp.log"; }
VP_PID=$VP_PID
read -r -t 5 -u "${VP[0]}" reply && [[ "$reply" == ok ]] \
	|| fail "virtual pointer did not start: $(cat "$TMP/vp.log")"

"$BIN" --config NONE --command /bin/sh --width 700 --height 400 \
	--debug-socket "$TMP/dbg.sock" >"$TMP/pi.log" 2>&1 &
PI_PID=$!
for _ in $(seq 1 50); do [[ -S "$TMP/dbg.sock" ]] && break; sleep 0.1; done
[[ -S "$TMP/dbg.sock" ]] || fail "the terminal did not open its inspector socket"

dbg() { WSAP_DEBUG_SOCKET="$TMP/dbg.sock" python3 "$ROOT/scripts/wsap-debug" "$@"; }
# The tab counts of the windows, in the host's order.
tabs() {
	dbg windows | python3 -c 'import json,sys; print(" ".join(str(w["tabs"]) for w in json.load(sys.stdin)))'
}
titles() {
	dbg windows | python3 -c "import json,sys; print(' '.join(json.load(sys.stdin)[$1]['tab_titles']))"
}
# The layout positions of the windows' contents, oldest first.
windows_at() {
	swaymsg -t get_tree -r | python3 -c '
import json, sys
found = []
def walk(n):
    for k in n.get("nodes", []) + n.get("floating_nodes", []):
        if k.get("app_id"):
            found.append((k["id"], k["rect"]["x"], k["rect"]["y"]))
        walk(k)
walk(json.load(sys.stdin))
for con, x, y in sorted(found):
    print(con, x, y)'
}
# Where tab INDEX of window WIN is, in layout pixels: its center.
tab_at() {
	local win=$1 index=$2 x y
	read -r _ x y < <(windows_at | sed -n "$((win + 1))p")
	dbg -w "$win" widget tabbar | python3 -c "
import json, sys
tabs = [p['rect'] for p in json.load(sys.stdin)['parts'] if p['name'] == 'tab']
r = tabs[$index]
print($x + r[0] + r[2] // 2, $y + r[1] + r[3] // 2)"
}
ptr() {
	echo "$*" >&"${VP[1]}"
	read -r -t 5 -u "${VP[0]}" reply || fail "virtual pointer stopped"
}
# drag X0 Y0 X1 Y1 [X2 Y2 ...]: press at the first point, glide through
# the others, release at the last.
drag() {
	local px=$1 py=$2 x y i
	shift 2
	ptr "$px" "$py"
	ptr press
	while [[ $# -ge 2 ]]; do
		x=$1 y=$2
		shift 2
		for i in $(seq 1 12); do
			ptr "$((px + (x - px) * i / 12))" "$((py + (y - py) * i / 12))"
			sleep 0.02
		done
		px=$x py=$y
	done
	sleep 0.2
	if [[ -n "${CHECK_PREVIEW_WIN:-}" ]]; then
		dbg -w "$CHECK_PREVIEW_WIN" widget tabbar | python3 -c '
import json, sys
parts = json.load(sys.stdin)["parts"]
assert any(part["name"] == "drop-preview" for part in parts)
' || fail "the incoming tab has no drop preview"
		dbg -w "$CHECK_PREVIEW_WIN" lint | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert result["count"] == 0, result["findings"]
' || fail "the destination tab bar has alignment findings"
		dbg -w "$CHECK_PREVIEW_WIN" screenshot "${TAB_DRAG_SCREENSHOT:-$TMP/drop-preview.png}" >/dev/null \
			|| fail "the drop preview screenshot failed"
		unset CHECK_PREVIEW_WIN
	fi
	ptr release
	sleep 0.6
}
expect_tabs() {
	local got
	got="$(tabs)"
	[[ "$got" == "$1" ]] || fail "$2: windows hold tabs '$got', expected '$1'"
	echo "ok: $2 ($got)"
}

dbg shortcut new_tab >/dev/null
dbg shortcut new_tab >/dev/null
sleep 0.3
expect_tabs "3" "three tabs in one window"
read -r x y < <(tab_at 0 0)
ptr "$x" "$y"
sleep 0.7
dbg -w 0 widget tab-preview | python3 -c '
import json, sys
parts = json.load(sys.stdin)["parts"]
images = [part["rect"] for part in parts if part["name"] == "image"]
assert len(images) == 1 and images[0][2] > 300, images
' || fail "the hover preview did not grow"
dbg -w 0 lint | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert result["count"] == 0, result["findings"]
' || fail "the hover preview has alignment findings"
dbg -w 0 screenshot "${TAB_HOVER_SCREENSHOT:-$TMP/hover-preview.png}" >/dev/null \
	|| fail "the hover preview screenshot failed"

read -r x y < <(tab_at 0 2)
drag "$x" "$y" "$x" 900
expect_tabs "2 1" "a tab dropped on the desktop opens its own window"

mapfile -t CONS < <(windows_at | cut -d' ' -f1)
swaymsg -q "[con_id=${CONS[0]}] move position 0 0"
swaymsg -q "[con_id=${CONS[1]}] move position 850 0"
sleep 0.3

# The regression: straight across out of a two-tab window, never leaving
# the bar's height, into the other window's top edge.
read -r x y < <(tab_at 0 1)
CHECK_PREVIEW_WIN=1
drag "$x" "$y" 1200 "$y"
expect_tabs "1 2" "a tab dragged sideways joins the next window"
dbg -w 1 lint | python3 -c '
import json, sys
result = json.load(sys.stdin)
assert result["count"] == 0, result["findings"]
' || fail "the dropped tab has alignment findings"
dbg -w 1 screenshot "${TAB_DROP_RESULT_SCREENSHOT:-$TMP/drop-result.png}" >/dev/null \
	|| fail "the dropped tab screenshot failed"

dbg -w 1 text "printf '\\033]0;moved\\007'" >/dev/null
dbg -w 1 key enter >/dev/null
sleep 0.4
before="$(titles 1)"
case "$before" in
"moved "*) from=0 to=1 ;;
*" moved") from=1 to=0 ;;
*) fail "the active tab did not take its title: '$before'" ;;
esac
read -r x y < <(tab_at 1 "$from")
read -r tx ty < <(tab_at 1 "$to")
drag "$x" "$y" "$x" 300 "$tx" "$ty"
expect_tabs "1 2" "a tab dropped on its own bar stays in its window"
after="$(titles 1)"
[[ "$after" != "$before" ]] || fail "a drop on the tab's own bar did not reorder: '$after'"
echo "ok: a drop on its own bar reorders ($before -> $after)"

kill -0 "$PI_PID" 2>/dev/null || fail "the terminal died"
echo "PASS: tabs detach, move between windows, and reorder by drag and drop"
exit 0
