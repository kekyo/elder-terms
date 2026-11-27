#!/bin/bash
# Verify that docking keeps the colored resize frame on the outer window.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
  exec xvfb-run -a -s '-screen 0 2000x1050x24' dbus-run-session -- bash "$0" --inside "$@"
fi
binary=$(realpath "$2")
scratch=$(mktemp -d)
export XDG_CONFIG_HOME="$scratch/config" XDG_DATA_HOME="$scratch/data" GDK_BACKEND=x11
export GSETTINGS_BACKEND=memory G_DEBUG=fatal-criticals
unset ELDER_TERMS_LAUNCH_ID ELDER_TERMS_LAUNCHER_BUS
pids=()
finish() {
  kill "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  if [[ $1 != 0 ]]; then cat "$scratch/"{First,Second,wm}.log >&2; fi
  rm -rf "$scratch"
}
trap 'finish $?' EXIT
KWIN_COMPOSE=O2 kwin_x11 --replace >"$scratch/wm.log" 2>&1 & pids+=($!)
gdbus wait --session --timeout=20 org.kde.KWin
wait_until() {
  for ((i=0;i<160;i++)); do if "$@"; then return; fi; sleep .05; done
  echo "Timed out waiting for $*" >&2
  return 1
}
for tab in First Second; do
  color='#A51D2D'
  if [[ $tab == Second ]]; then color='#2D6AA5'; fi
  compact=false
  if [[ ${ELDER_TERMS_LAYOUT_COMPACT:-} == 1 ]]; then compact=true; fi
  cat >"$scratch/$tab.ini" <<CONFIG
[general]
name=$tab
type=local
auto_close=false
show_border=true
border_width=7
exterior_background=$color
compact_mode=$compact
[local]
command_line=/bin/sh
[terminal]
width=60
height=20
CONFIG
  "$binary" -c "$scratch/$tab.ini" >"$scratch/$tab.log" 2>&1 & pids+=($!)
done
window() { xdotool search --onlyvisible --name "elder-terms .* $1$" | tail -1; }
wait_until window First
wait_until window Second
first=$(window First)
second=$(window Second)
xdotool windowmove --sync "$first" 40 100 windowmove --sync "$second" 980 100
content_origin() {
  local x y left right top bottom
  read -r x y < <(xwininfo -id "$1" | awk '/Absolute upper-left [XY]:/{print $NF}' | xargs)
  left=0 right=0 top=0 bottom=0
  read -r left right top bottom < <(xprop -id "$1" _GTK_FRAME_EXTENTS | sed -n 's/.* = //p' | tr -d ',') || true
  echo "$((x+left)) $((y+top))"
}
read -r fx fy < <(content_origin "$first")
read -r sx sy < <(content_origin "$second")
header_y=27
if [[ ${ELDER_TERMS_LAYOUT_COMPACT:-} == 1 ]]; then header_y=16; fi
xdotool mousemove "$((sx+26))" "$((sy+header_y))" mousedown 1
for ((i=1;i<=16;i++)); do
  xdotool mousemove "$((sx+26+(fx+220-sx-26)*i/16))" "$((sy+header_y+(fy+150-sy-header_y)*i/16))"
  sleep .025
done
xdotool mouseup 1
merged() { [[ $(window Second) == "$first" ]]; }
wait_until merged
# Keep the terminal wider than its standalone minimum after moving the splitter.
xdotool windowsize --sync "$first" 800 460
# The frame color is set on each page. After grouping, the active page's
# border must be drawn at the outside of the whole tray, before the tab list.
pixel_at() {
  local left right top bottom
  left=0 right=0 top=0 bottom=0
  read -r left right top bottom < <(xprop -id "$first" _GTK_FRAME_EXTENTS | sed -n 's/.* = //p' | tr -d ',') || true
  xwd -silent -id "$first" | convert xwd:- -format "%[pixel:p{$((left+$1)),$2}]" info:
}
is_pixel() { [[ $(pixel_at "$1" "$2") == "$3" ]]; }
is_not_pixel() { [[ $(pixel_at "$1" "$2") != "$3" ]]; }
wait_until is_pixel 7 250 'srgb(45,106,165)'
if [[ ${ELDER_TERMS_LAYOUT_COMPACT:-} == 1 ]]; then
  window_height=$(xwininfo -id "$first" | sed -n 's/^ *Height: //p')
  wait_until is_pixel 50 "$((window_height-8))" 'srgb(45,106,165)'
fi
initial_terminal=$(pixel_at 260 250)
xdotool mousemove "$((fx+234))" "$((fy+250))" mousedown 1
xdotool mousemove --sync "$((fx+284))" "$((fy+250))" mouseup 1
wait_until is_pixel 260 250 'srgb(250,250,250)'
if [[ $initial_terminal == 'srgb(250,250,250)' ]]; then exit 1; fi
xdotool mousemove "$((fx+50))" "$((fy+69))" click 1
wait_until is_pixel 7 250 'srgb(165,29,45)'
xdotool mousemove "$((fx+50))" "$((fy+107))" click 1
# Count the uninterrupted dark terminal canvas at a row without shell text.
terminal_span() {
  xwd -silent -id "$1" | convert xwd:- -crop 0x1+0+250 +repage -depth 8 rgb:- |
    od -An -v -tu1 -w3 | awk '
      $1 <= 25 && $2 <= 25 && $3 <= 25 { run++; next }
      { if (run > longest) longest = run; run = 0 }
      END { if (run > longest) longest = run; print longest }
    '
}
before_span=$(terminal_span "$first")
xdotool key ctrl+shift+u
split() {
  detached=$(window Second) && [[ -n $detached && $detached != "$first" ]]
}
wait_until split
wait_until is_not_pixel 50 250 'srgb(250,250,250)'
wait_until is_pixel 7 250 'srgb(165,29,45)'
after_span=$(terminal_span "$detached")
if [[ $after_span != "$before_span" ]]; then
  echo "Terminal display width changed on undock: $before_span -> $after_span pixels" >&2
  exit 1
fi
xdotool windowactivate --sync "$first" key alt+F4
xdotool windowactivate --sync "$detached" key alt+F4
for pid in "${pids[@]:1}"; do wait "$pid"; done
if grep -E 'Gtk-CRITICAL|Gtk-WARNING' "$scratch/"{First,Second}.log; then exit 1; fi
